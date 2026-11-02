#include "store/LedgerDb.h"

#include <sqlcipher/sqlite3.h>

#include <QTimeZone>

#include <utility>

namespace store {

namespace {

constexpr int kSchemaVersion = 1;

// Finalizes on every path out of a query.
class Statement
{
public:
    Statement() = default;
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt** slot() { return &stmt_; }
    sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

void bindText(sqlite3_stmt* stmt, int position, const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    sqlite3_bind_text(stmt, position, utf8.constData(), int(utf8.size()), SQLITE_TRANSIENT);
}

QString columnText(sqlite3_stmt* stmt, int column)
{
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    return text ? QString::fromUtf8(text, sqlite3_column_bytes(stmt, column)) : QString();
}

// Rolls back unless told the work went through, so a failure leaves the file as it was.
class Transaction
{
public:
    explicit Transaction(sqlite3* db)
        : db_(db), began_(sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK)
    {
    }
    ~Transaction()
    {
        if (began_ && !done_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    // Nothing goes in without one: each write would stand on its own.
    bool began() const { return began_; }
    bool commit()
    {
        done_ = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
        return done_;
    }

private:
    sqlite3* db_;
    bool began_;
    bool done_ = false;
};

// For the walk in index order, whose rows lie wherever they were first written. Decrypting a
// page costs more than keeping it.
constexpr const char* kLoadCache = "PRAGMA cache_size = -64000";
constexpr const char* kUsualCache = "PRAGMA cache_size = -8000";

} // namespace

LedgerDb::LedgerDb() = default;

LedgerDb::~LedgerDb()
{
    close();
}

QString LedgerDb::lastError() const
{
    return db_ ? QString::fromUtf8(sqlite3_errmsg(db_)) : QStringLiteral("the database is closed");
}

bool LedgerDb::exec(const char* sql, QString* why)
{
    if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK)
        return true;
    if (why)
        *why = lastError();
    return false;
}

bool LedgerDb::prepare(const char* sql, sqlite3_stmt** out, QString* why)
{
    if (sqlite3_prepare_v2(db_, sql, -1, out, nullptr) == SQLITE_OK)
        return true;
    if (why)
        *why = lastError();
    return false;
}

LedgerDb::Open LedgerDb::open(const QString& path, const QByteArray& key, QString* why)
{
    close();
    const QByteArray file = path.toUtf8();
    if (sqlite3_open_v2(file.constData(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (why)
            *why = lastError();
        close();
        return Open::Failed;
    }
    sqlite3_busy_timeout(db_, 5000);

    // The raw key form: the key is random and full length, so nothing is gained from
    // stretching it, and opening stays instant.
    QByteArray pragma = "PRAGMA key = \"x'" + key.toHex() + "'\"";
    const int keyed = sqlite3_exec(db_, pragma.constData(), nullptr, nullptr, nullptr);
    pragma.fill('\0');
    if (keyed != SQLITE_OK) {
        if (why)
            *why = lastError();
        close();
        return Open::Failed;
    }

    // The first read tells whether this is a database under this key. A database of
    // another key, or a damaged one, reads as "not a database".
    const int read = sqlite3_exec(db_, "SELECT count(*) FROM sqlite_master", nullptr, nullptr, nullptr);
    if (read != SQLITE_OK) {
        const bool notOurs = damaged();
        if (why)
            *why = lastError();
        close();
        return notOurs ? Open::NotOurs : Open::Failed;
    }

    // The first read only saw the first page, and every page has its code: the check below
    // reads them all, without decrypting one. It reads the main file alone, where a power
    // cut during a checkpoint leaves torn pages the WAL still holds whole: those are written
    // back first, and if that cannot finish the reads that follow judge for themselves.
    bool backIn = false;
    {
        Statement checkpoint;
        if (prepare("PRAGMA wal_checkpoint(TRUNCATE)", checkpoint.slot(), nullptr))
            backIn = sqlite3_step(checkpoint.get()) == SQLITE_ROW && sqlite3_column_int(checkpoint.get(), 0) == 0;
    }
    if (backIn) {
        int checked = SQLITE_DONE;
        QString failure;
        {
            Statement check;
            if (!prepare("PRAGMA cipher_integrity_check", check.slot(), why)) {
                close();
                return Open::Failed;
            }
            checked = sqlite3_step(check.get());
            if (checked != SQLITE_DONE)
                failure = checked == SQLITE_ROW ? columnText(check.get(), 0) : lastError();
        }
        if (checked != SQLITE_DONE) {
            if (why)
                *why = failure;
            close();
            // A row names what failed. One that could not be read is the disk's doing, not the file's.
            return checked == SQLITE_ROW && !failure.startsWith(QLatin1String("error reading")) ? Open::NotOurs : Open::Failed;
        }
    }

    // WAL: a write does not wait on the disk twice. Pages in the WAL are encrypted too.
    if (!exec("PRAGMA journal_mode = WAL", why) || !exec("PRAGMA synchronous = NORMAL", why)
        || !exec("PRAGMA foreign_keys = ON", why) || !migrate(why) || !loadNames(why)) {
        const bool notOurs = damaged();
        close();
        return notOurs ? Open::NotOurs : Open::Failed;
    }
    return Open::Ready;
}

bool LedgerDb::damaged() const
{
    const int code = sqlite3_errcode(db_);
    return code == SQLITE_NOTADB || code == SQLITE_CORRUPT;
}

void LedgerDb::close()
{
    if (db_)
        sqlite3_close_v2(db_);
    db_ = nullptr;
    nameIds_.clear();
    names_.clear();
}

bool LedgerDb::migrate(QString* why)
{
    Statement version;
    if (!prepare("PRAGMA user_version", version.slot(), why))
        return false;
    const int have = sqlite3_step(version.get()) == SQLITE_ROW ? sqlite3_column_int(version.get(), 0) : -1;
    if (have < 0) {
        if (why)
            *why = lastError();
        return false;
    }
    if (have > kSchemaVersion) {
        if (why)
            *why = QStringLiteral("it was written by a newer GitWallet");
        return false;
    }
    if (have == kSchemaVersion)
        return true;

    // Stage by stage from here on: each version adds to the last.
    Transaction work(db_);
    if (!work.began()) {
        if (why)
            *why = lastError();
        return false;
    }
    if (have < 1) {
        const char* schema = R"sql(
            CREATE TABLE name(id INTEGER PRIMARY KEY, text TEXT NOT NULL UNIQUE);
            CREATE TABLE address(
                id INTEGER PRIMARY KEY,
                wallet INTEGER NOT NULL,
                network INTEGER NOT NULL REFERENCES name(id),
                type INTEGER NOT NULL REFERENCES name(id),
                account INTEGER NOT NULL,
                branch INTEGER NOT NULL,
                idx INTEGER NOT NULL,
                address TEXT NOT NULL,
                scripthash TEXT,
                status TEXT,
                checked INTEGER NOT NULL,
                used INTEGER NOT NULL,
                confirmed INTEGER NOT NULL,
                unconfirmed INTEGER NOT NULL,
                nonce INTEGER NOT NULL,
                UNIQUE(wallet, network, type, account, branch, idx)
            );
            CREATE TABLE balance(
                address INTEGER NOT NULL REFERENCES address(id) ON DELETE CASCADE,
                asset INTEGER NOT NULL REFERENCES name(id),
                amount BLOB NOT NULL,
                PRIMARY KEY(address, asset)
            ) WITHOUT ROWID;
            CREATE TABLE price(
                asset TEXT PRIMARY KEY,
                usd REAL NOT NULL,
                at INTEGER NOT NULL,
                source TEXT NOT NULL
            ) WITHOUT ROWID;
            CREATE TABLE network_time(
                network TEXT PRIMARY KEY,
                at INTEGER,
                nonces_at INTEGER,
                sweep_from INTEGER,
                sweep_wallet INTEGER,
                sweep_account INTEGER,
                sweep_index INTEGER,
                sweep_clears INTEGER
            ) WITHOUT ROWID;
        )sql";
        if (!exec(schema, why))
            return false;
    }
    const QByteArray set = "PRAGMA user_version = " + QByteArray::number(kSchemaVersion);
    if (!exec(set.constData(), why))
        return false;
    if (!work.commit()) {
        if (why)
            *why = lastError();
        return false;
    }
    return true;
}

bool LedgerDb::loadNames(QString* why)
{
    Statement names;
    if (!prepare("SELECT id, text FROM name", names.slot(), why))
        return false;
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(names.get())) == SQLITE_ROW) {
        const int id = sqlite3_column_int(names.get(), 0);
        const QString text = columnText(names.get(), 1);
        nameIds_.insert(text, id);
        names_.insert(id, text);
    }
    if (step != SQLITE_DONE) {
        if (why)
            *why = lastError();
        return false;
    }
    return true;
}

int LedgerDb::nameId(const QString& text, QString* why)
{
    const auto found = nameIds_.constFind(text);
    if (found != nameIds_.cend())
        return *found;
    Statement insert;
    if (!prepare("INSERT INTO name(text) VALUES(?)", insert.slot(), why))
        return -1;
    bindText(insert.get(), 1, text);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) {
        if (why)
            *why = lastError();
        return -1;
    }
    const int id = int(sqlite3_last_insert_rowid(db_));
    nameIds_.insert(text, id);
    names_.insert(id, text);
    return id;
}

bool LedgerDb::loadRows(const QSet<quint32>& wallets, int chunk,
                        const std::function<void(QVector<wallet::AddressRow>&&)>& sink, QString* why)
{
    readDamaged_ = false;
    if (!db_) {
        if (why)
            *why = lastError();
        return false;
    }
    // Before anything else runs on the connection: it would replace the error.
    const auto failed = [this, why] {
        if (why)
            *why = lastError();
        readDamaged_ = damaged();
        return false;
    };
    // What a wallet removed earlier left behind. On a full disk that has to wait: the rows
    // read below go no further, and it is no reason not to read them.
    {
        Statement list;
        if (!prepare("SELECT DISTINCT wallet FROM address", list.slot(), why))
            return failed();
        QList<quint32> gone;
        int step = SQLITE_DONE;
        while ((step = sqlite3_step(list.get())) == SQLITE_ROW) {
            const quint32 id = quint32(sqlite3_column_int64(list.get(), 0));
            if (!wallets.contains(id))
                gone.append(id);
        }
        if (step != SQLITE_DONE)
            return failed();
        for (const quint32 id : std::as_const(gone))
            removeWallet(id, nullptr);
    }

    // Few rows hold anything, and the rows come in another order than their ids.
    struct Held {
        int asset = 0;
        QByteArray amount;
    };
    QHash<qint64, QList<Held>> held;
    {
        Statement balances;
        if (!prepare("SELECT address, asset, amount FROM balance", balances.slot(), why))
            return failed();
        int step = SQLITE_DONE;
        while ((step = sqlite3_step(balances.get())) == SQLITE_ROW) {
            const auto* bytes = static_cast<const char*>(sqlite3_column_blob(balances.get(), 2));
            held[sqlite3_column_int64(balances.get(), 0)].append(
                {sqlite3_column_int(balances.get(), 1), QByteArray(bytes, sqlite3_column_bytes(balances.get(), 2))});
        }
        if (step != SQLITE_DONE)
            return failed();
    }

    // In the order of the table on screen, which sorts by path until told otherwise: rows
    // that land together cost it one insertion each chunk, and scattered ones one each.
    sqlite3_exec(db_, kLoadCache, nullptr, nullptr, nullptr);
    Statement rows;
    if (!prepare("SELECT id, wallet, network, type, account, branch, idx, address, scripthash, status, checked, used, "
                 "confirmed, unconfirmed, nonce FROM address ORDER BY wallet, network, type, account, branch, idx",
                 rows.slot(), why))
        return failed();

    QVector<wallet::AddressRow> out;
    out.reserve(chunk);
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
        sqlite3_stmt* r = rows.get();
        const qint64 id = sqlite3_column_int64(r, 0);
        const wallet::NetworkInfo* network = wallet::findNetwork(names_.value(sqlite3_column_int(r, 2)));
        chain::ScriptType type = chain::ScriptType::P2WPKH;
        const bool typed = chain::scriptTypeFromId(names_.value(sqlite3_column_int(r, 3)).toStdString(), &type);

        wallet::AddressRow row;
        row.wallet = quint32(sqlite3_column_int64(r, 1));
        row.type = type;
        row.account = quint32(sqlite3_column_int64(r, 4));
        row.change = quint32(sqlite3_column_int(r, 5));
        row.index = quint32(sqlite3_column_int64(r, 6));
        row.address = columnText(r, 7);
        row.scripthash = columnText(r, 8);
        row.status = columnText(r, 9);
        row.checked = sqlite3_column_int(r, 10) != 0;
        row.used = sqlite3_column_int(r, 11) != 0;
        row.confirmed = sqlite3_column_int64(r, 12);
        row.unconfirmed = sqlite3_column_int64(r, 13);
        row.nonce = quint64(sqlite3_column_int64(r, 14));
        const bool known = network && typed;
        if (known)
            row.network = network->network;

        const auto own = held.constFind(id);
        if (own != held.cend() && known) {
            for (const Held& one : *own) {
                const wallet::AssetInfo* asset = wallet::findAsset(names_.value(one.asset));
                const auto* bytes = reinterpret_cast<const uint8_t*>(one.amount.constData());
                if (asset && !one.amount.isEmpty() && asset->network == row.network)
                    row.setBalance(asset->asset, chain::Amount::fromBigEndian(chain::Bytes(bytes, bytes + one.amount.size())));
            }
        }

        if (!known)
            continue; // of a network not on the list now: stays in the file, not on screen
        out.append(std::move(row));
        if (out.size() >= chunk) {
            sink(std::exchange(out, {}));
            out.reserve(chunk);
        }
    }
    if (step != SQLITE_DONE) {
        failed();
        sqlite3_exec(db_, kUsualCache, nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_exec(db_, kUsualCache, nullptr, nullptr, nullptr);
    if (!out.isEmpty())
        sink(std::move(out));
    return true;
}

bool LedgerDb::writeRows(const QVector<wallet::AddressRow>& rows, QString* why)
{
    Statement upsert;
    if (!prepare("INSERT INTO address(wallet, network, type, account, branch, idx, address, scripthash, status, checked, "
                 "used, confirmed, unconfirmed, nonce) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
                 "ON CONFLICT(wallet, network, type, account, branch, idx) DO UPDATE SET address=excluded.address, "
                 "scripthash=excluded.scripthash, status=excluded.status, checked=excluded.checked, used=excluded.used, "
                 "confirmed=excluded.confirmed, unconfirmed=excluded.unconfirmed, nonce=excluded.nonce RETURNING id",
                 upsert.slot(), why))
        return false;
    Statement drop;
    if (!prepare("DELETE FROM balance WHERE address = ?", drop.slot(), why))
        return false;
    Statement put;
    if (!prepare("INSERT INTO balance(address, asset, amount) VALUES(?,?,?)", put.slot(), why))
        return false;

    for (const wallet::AddressRow& row : rows) {
        const int network = nameId(wallet::networkInfo(row.network).id, why);
        const int type = nameId(QLatin1String(chain::scriptTypeId(row.type)), why);
        if (network < 0 || type < 0)
            return false;
        sqlite3_stmt* u = upsert.get();
        sqlite3_bind_int64(u, 1, row.wallet);
        sqlite3_bind_int(u, 2, network);
        sqlite3_bind_int(u, 3, type);
        sqlite3_bind_int64(u, 4, row.account);
        sqlite3_bind_int(u, 5, int(row.change));
        sqlite3_bind_int64(u, 6, row.index);
        bindText(u, 7, row.address);
        bindText(u, 8, row.scripthash);
        bindText(u, 9, row.status);
        sqlite3_bind_int(u, 10, row.checked ? 1 : 0);
        sqlite3_bind_int(u, 11, row.used ? 1 : 0);
        sqlite3_bind_int64(u, 12, row.confirmed);
        sqlite3_bind_int64(u, 13, row.unconfirmed);
        sqlite3_bind_int64(u, 14, qint64(row.nonce));
        if (sqlite3_step(u) != SQLITE_ROW) {
            if (why)
                *why = lastError();
            sqlite3_reset(u);
            return false;
        }
        const qint64 id = sqlite3_column_int64(u, 0);
        sqlite3_reset(u);
        sqlite3_clear_bindings(u);

        // Only an Evm row has balances of its own; a UTXO row's are the columns above.
        if (!row.isUtxo()) {
            sqlite3_bind_int64(drop.get(), 1, id);
            const int dropped = sqlite3_step(drop.get());
            sqlite3_reset(drop.get());
            if (dropped != SQLITE_DONE) {
                if (why)
                    *why = lastError();
                return false;
            }
            for (auto it = row.balances.cbegin(); it != row.balances.cend(); ++it) {
                const int asset = nameId(wallet::assetInfo(it.key()).key, why);
                if (asset < 0)
                    return false;
                const chain::Bytes& amount = it.value().bigEndian();
                sqlite3_bind_int64(put.get(), 1, id);
                sqlite3_bind_int(put.get(), 2, asset);
                sqlite3_bind_blob(put.get(), 3, amount.data(), int(amount.size()), SQLITE_TRANSIENT);
                const int done = sqlite3_step(put.get());
                sqlite3_reset(put.get());
                if (done != SQLITE_DONE) {
                    if (why)
                        *why = lastError();
                    return false;
                }
            }
        }
    }
    return true;
}

bool LedgerDb::writeInTransaction(const std::function<bool(QString*)>& work, QString* why)
{
    if (!db_) {
        if (why)
            *why = lastError();
        return false;
    }
    {
        Transaction transaction(db_);
        if (!transaction.began()) {
            if (why)
                *why = lastError();
            return false;
        }
        if (work(why)) {
            if (transaction.commit())
                return true;
            if (why)
                *why = lastError();
        }
    }
    // Rolled back: a name it added is gone from the file, and its id may go to another. The
    // WAL stays as large as what was tried, on a disk that may have failed for that.
    nameIds_.clear();
    names_.clear();
    loadNames(nullptr);
    sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, nullptr, nullptr);
    return false;
}

bool LedgerDb::saveRows(const QVector<wallet::AddressRow>& rows, QString* why)
{
    return writeInTransaction([this, &rows](QString* failure) { return writeRows(rows, failure); }, why);
}

bool LedgerDb::replaceRows(const QVector<wallet::AddressRow>& rows, QString* why)
{
    return writeInTransaction([this, &rows](QString* failure) {
        // A network not on the list right now keeps its rows.
        Statement remove;
        if (!prepare("DELETE FROM address WHERE network = ?", remove.slot(), failure))
            return false;
        for (const wallet::NetworkInfo& network : wallet::networks()) {
            const auto id = nameIds_.constFind(network.id);
            if (id == nameIds_.cend())
                continue;
            sqlite3_bind_int(remove.get(), 1, *id);
            const int done = sqlite3_step(remove.get());
            sqlite3_reset(remove.get());
            if (done != SQLITE_DONE) {
                if (failure)
                    *failure = lastError();
                return false;
            }
        }
        return writeRows(rows, failure);
    }, why);
}

bool LedgerDb::removeWallet(quint32 wallet, QString* why)
{
    if (!db_) {
        if (why)
            *why = lastError();
        return false;
    }
    Statement remove;
    if (!prepare("DELETE FROM address WHERE wallet = ?", remove.slot(), why))
        return false;
    sqlite3_bind_int64(remove.get(), 1, wallet);
    if (sqlite3_step(remove.get()) != SQLITE_DONE) {
        if (why)
            *why = lastError();
        return false;
    }
    return true;
}

int LedgerDb::rowCount()
{
    if (!db_)
        return -1;
    Statement count;
    if (!prepare("SELECT count(*) FROM address", count.slot(), nullptr))
        return -1;
    return sqlite3_step(count.get()) == SQLITE_ROW ? sqlite3_column_int(count.get(), 0) : -1;
}

bool LedgerDb::savePrices(const QList<SavedPrice>& prices, QString* why)
{
    return writeInTransaction([this, &prices](QString* failure) {
        Statement put;
        if (!prepare("INSERT OR REPLACE INTO price(asset, usd, at, source) VALUES(?,?,?,?)", put.slot(), failure))
            return false;
        for (const SavedPrice& price : prices) {
            bindText(put.get(), 1, price.asset);
            sqlite3_bind_double(put.get(), 2, price.usd);
            sqlite3_bind_int64(put.get(), 3, price.at.toSecsSinceEpoch());
            bindText(put.get(), 4, price.source);
            const int done = sqlite3_step(put.get());
            sqlite3_reset(put.get());
            if (done != SQLITE_DONE) {
                if (failure)
                    *failure = lastError();
                return false;
            }
        }
        return true;
    }, why);
}

QList<SavedPrice> LedgerDb::loadPrices(QString* why)
{
    QList<SavedPrice> out;
    Statement list;
    if (!db_ || !prepare("SELECT asset, usd, at, source FROM price", list.slot(), why))
        return out;
    while (sqlite3_step(list.get()) == SQLITE_ROW) {
        SavedPrice price;
        price.asset = columnText(list.get(), 0);
        price.usd = sqlite3_column_double(list.get(), 1);
        price.at = QDateTime::fromSecsSinceEpoch(sqlite3_column_int64(list.get(), 2), QTimeZone::UTC);
        price.source = columnText(list.get(), 3);
        out.append(price);
    }
    return out;
}

bool LedgerDb::saveTimes(const QList<SavedTime>& times, QString* why)
{
    return writeInTransaction([this, &times](QString* failure) {
        Statement put;
        if (!prepare("INSERT OR REPLACE INTO network_time(network, at, nonces_at, sweep_from, sweep_wallet, sweep_account, "
                     "sweep_index, sweep_clears) VALUES(?,?,?,?,?,?,?,?)",
                     put.slot(), failure))
            return false;
        for (const SavedTime& time : times) {
            bindText(put.get(), 1, time.network);
            if (time.at.isValid())
                sqlite3_bind_int64(put.get(), 2, time.at.toSecsSinceEpoch());
            else
                sqlite3_bind_null(put.get(), 2);
            if (time.noncesAt.isValid())
                sqlite3_bind_int64(put.get(), 3, time.noncesAt.toSecsSinceEpoch());
            else
                sqlite3_bind_null(put.get(), 3);
            if (time.sweep.from.isValid()) {
                sqlite3_bind_int64(put.get(), 4, time.sweep.from.toSecsSinceEpoch());
                sqlite3_bind_int64(put.get(), 5, time.sweep.wallet);
                sqlite3_bind_int64(put.get(), 6, time.sweep.account);
                sqlite3_bind_int64(put.get(), 7, time.sweep.index);
                sqlite3_bind_int(put.get(), 8, time.sweep.clears ? 1 : 0);
            } else {
                for (int column = 4; column <= 8; ++column)
                    sqlite3_bind_null(put.get(), column);
            }
            const int done = sqlite3_step(put.get());
            sqlite3_reset(put.get());
            if (done != SQLITE_DONE) {
                if (failure)
                    *failure = lastError();
                return false;
            }
        }
        return true;
    }, why);
}

bool LedgerDb::clearTimes(QString* why)
{
    return db_ ? exec("DELETE FROM network_time", why) : false;
}

QList<SavedTime> LedgerDb::loadTimes(QString* why)
{
    QList<SavedTime> out;
    Statement list;
    if (!db_ || !prepare("SELECT network, at, nonces_at, sweep_from, sweep_wallet, sweep_account, sweep_index, sweep_clears "
                         "FROM network_time",
                         list.slot(), why))
        return out;
    while (sqlite3_step(list.get()) == SQLITE_ROW) {
        SavedTime time;
        time.network = columnText(list.get(), 0);
        if (sqlite3_column_type(list.get(), 1) != SQLITE_NULL)
            time.at = QDateTime::fromSecsSinceEpoch(sqlite3_column_int64(list.get(), 1), QTimeZone::UTC);
        if (sqlite3_column_type(list.get(), 2) != SQLITE_NULL)
            time.noncesAt = QDateTime::fromSecsSinceEpoch(sqlite3_column_int64(list.get(), 2), QTimeZone::UTC);
        if (sqlite3_column_type(list.get(), 3) != SQLITE_NULL) {
            time.sweep.from = QDateTime::fromSecsSinceEpoch(sqlite3_column_int64(list.get(), 3), QTimeZone::UTC);
            time.sweep.wallet = quint32(sqlite3_column_int64(list.get(), 4));
            time.sweep.account = quint32(sqlite3_column_int64(list.get(), 5));
            time.sweep.index = quint32(sqlite3_column_int64(list.get(), 6));
            time.sweep.clears = sqlite3_column_int(list.get(), 7) != 0;
        }
        out.append(time);
    }
    return out;
}

} // namespace store
