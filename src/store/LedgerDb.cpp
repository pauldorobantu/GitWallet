#include "store/LedgerDb.h"

#include <sqlcipher/sqlite3.h>

#include <QTimeZone>

#include <cstring>
#include <utility>

namespace store {

namespace {

constexpr int kSchemaVersion = 4;
constexpr int kOutPointBytes = 36;

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

void bindHash(sqlite3_stmt* stmt, int position, const chain::Hash256& hash)
{
    sqlite3_bind_blob(stmt, position, hash.data(), int(hash.size()), SQLITE_TRANSIENT);
}

chain::Hash256 columnHash(sqlite3_stmt* stmt, int column)
{
    chain::Hash256 hash{};
    if (sqlite3_column_bytes(stmt, column) == int(hash.size()))
        std::memcpy(hash.data(), sqlite3_column_blob(stmt, column), hash.size());
    return hash;
}

// An address by the 32 bytes its scripthash stands for: half the room of its hex, on every row.
QByteArray scripthashBytes(const QString& hex)
{
    return QByteArray::fromHex(hex.toLatin1());
}

void bindScripthash(sqlite3_stmt* stmt, int position, const QString& hex)
{
    const QByteArray raw = scripthashBytes(hex);
    sqlite3_bind_blob(stmt, position, raw.constData(), int(raw.size()), SQLITE_TRANSIENT);
}

QString columnScripthash(sqlite3_stmt* stmt, int column)
{
    const auto* bytes = static_cast<const char*>(sqlite3_column_blob(stmt, column));
    return QString::fromLatin1(QByteArray(bytes, sqlite3_column_bytes(stmt, column)).toHex());
}

// For the walk in index order, whose rows lie wherever they were first written. Decrypting a
// page costs more than keeping it.
int gBusyTimeoutMs = 5000;
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
    sqlite3_busy_timeout(db_, gBusyTimeoutMs);

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
    if (have < 2) {
        // The history. Inputs go in one blob, 36 bytes each: it is only ever read whole.
        const char* schema = R"sql(
            CREATE TABLE tx(
                id INTEGER PRIMARY KEY,
                network INTEGER NOT NULL REFERENCES name(id),
                txid BLOB NOT NULL,
                height INTEGER NOT NULL,
                time INTEGER NOT NULL,
                fee INTEGER,
                vsize INTEGER NOT NULL,
                block_pos INTEGER NOT NULL,
                out_total INTEGER NOT NULL,
                seen INTEGER NOT NULL,
                proven INTEGER NOT NULL,
                replaceable INTEGER NOT NULL,
                usd_then REAL,
                usd_then_at INTEGER,
                replaced_by BLOB,
                inputs BLOB NOT NULL,
                UNIQUE(network, txid)
            );
            CREATE TABLE txo(
                tx INTEGER NOT NULL REFERENCES tx(id) ON DELETE CASCADE,
                n INTEGER NOT NULL,
                scripthash BLOB NOT NULL,
                value INTEGER NOT NULL,
                PRIMARY KEY(tx, n)
            ) WITHOUT ROWID;
            CREATE TABLE address_history(
                network INTEGER NOT NULL REFERENCES name(id),
                scripthash BLOB NOT NULL,
                wallet INTEGER NOT NULL,
                address TEXT NOT NULL,
                status TEXT NOT NULL,
                PRIMARY KEY(network, scripthash)
            ) WITHOUT ROWID;
            CREATE TABLE header(
                network INTEGER NOT NULL REFERENCES name(id),
                height INTEGER NOT NULL,
                raw BLOB NOT NULL,
                PRIMARY KEY(network, height)
            ) WITHOUT ROWID;
            CREATE TABLE tx_note(
                network INTEGER NOT NULL REFERENCES name(id),
                txid BLOB NOT NULL,
                label TEXT NOT NULL,
                note TEXT NOT NULL,
                PRIMARY KEY(network, txid)
            ) WITHOUT ROWID;
            CREATE TABLE address_label(
                address TEXT PRIMARY KEY,
                label TEXT NOT NULL
            ) WITHOUT ROWID;
        )sql";
        if (!exec(schema, why))
            return false;
    }
    if (have < 3) {
        const char* schema = R"sql(
            CREATE TABLE payment(
                id INTEGER PRIMARY KEY,
                at INTEGER NOT NULL,
                asset TEXT NOT NULL,
                amount BLOB NOT NULL,
                usd REAL NOT NULL,
                wallet INTEGER NOT NULL,
                address TEXT NOT NULL,
                addresses INTEGER NOT NULL,
                txid BLOB,
                is_read INTEGER NOT NULL,
                while_closed INTEGER NOT NULL
            );
        )sql";
        if (!exec(schema, why))
            return false;
    }
    if (have < 4) {
        // What the list of payments has to know at the next start, by name.
        const char* schema = R"sql(
            CREATE TABLE payment_state(
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
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

void LedgerDb::setBusyTimeoutForTest(int ms)
{
    gBusyTimeoutMs = ms > 0 ? ms : 5000;
}

bool LedgerDb::loadRows(const QSet<quint32>& wallets, int chunk,
                        const std::function<void(QVector<wallet::AddressRow>&&)>& sink, QString* why,
                        quint32 nextWallet, QStringList* labelsGone)
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
            removeWallet(id, nullptr, id < nextWallet ? labelsGone : nullptr);
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

bool LedgerDb::removeWallet(quint32 wallet, QString* why, QStringList* labelsGone)
{
    return writeInTransaction([this, wallet, labelsGone](QString* failure) {
        // What only this PC knew of those addresses goes with them, whatever rows the window has now.
        if (labelsGone) {
            Statement labels;
            if (!prepare("DELETE FROM address_label WHERE address IN (SELECT address FROM address WHERE wallet = ?1) "
                         "AND address NOT IN (SELECT address FROM address WHERE wallet != ?1) RETURNING address",
                         labels.slot(), failure))
                return false;
            sqlite3_bind_int64(labels.get(), 1, wallet);
            int step = SQLITE_DONE;
            while ((step = sqlite3_step(labels.get())) == SQLITE_ROW)
                labelsGone->append(columnText(labels.get(), 0));
            if (step != SQLITE_DONE) {
                if (failure)
                    *failure = lastError();
                return false;
            }
        }
        Statement payments;
        if (!prepare("DELETE FROM payment WHERE wallet = ?", payments.slot(), failure))
            return false;
        sqlite3_bind_int64(payments.get(), 1, wallet);
        if (sqlite3_step(payments.get()) != SQLITE_DONE) {
            if (failure)
                *failure = lastError();
            return false;
        }
        Statement remove;
        if (!prepare("DELETE FROM address WHERE wallet = ?", remove.slot(), failure))
            return false;
        sqlite3_bind_int64(remove.get(), 1, wallet);
        if (sqlite3_step(remove.get()) != SQLITE_DONE) {
            if (failure)
                *failure = lastError();
            return false;
        }
        return true;
    }, why);
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

bool LedgerDb::savePayment(const wallet::Payment& payment, int kept, QString* why)
{
    return writeInTransaction([this, &payment, kept](QString* failure) {
        Statement put;
        if (!prepare("INSERT OR REPLACE INTO payment(id, at, asset, amount, usd, wallet, address, addresses, txid, is_read, while_closed) "
                     "VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                     put.slot(), failure))
            return false;
        sqlite3_bind_int64(put.get(), 1, payment.id);
        sqlite3_bind_int64(put.get(), 2, payment.at);
        bindText(put.get(), 3, payment.asset);
        const chain::Bytes& amount = payment.amount.bigEndian();
        // An empty blob is not a null one: the column takes no null.
        sqlite3_bind_blob(put.get(), 4, amount.empty() ? "" : reinterpret_cast<const char*>(amount.data()), int(amount.size()), SQLITE_TRANSIENT);
        sqlite3_bind_double(put.get(), 5, payment.usd);
        sqlite3_bind_int64(put.get(), 6, payment.wallet);
        bindText(put.get(), 7, payment.address);
        sqlite3_bind_int(put.get(), 8, payment.addresses);
        if (payment.hasTx)
            sqlite3_bind_blob(put.get(), 9, payment.txid.data(), int(payment.txid.size()), SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(put.get(), 9);
        sqlite3_bind_int(put.get(), 10, payment.read ? 1 : 0);
        sqlite3_bind_int(put.get(), 11, payment.whileClosed ? 1 : 0);
        if (sqlite3_step(put.get()) != SQLITE_DONE) {
            if (failure)
                *failure = lastError();
            return false;
        }
        Statement old;
        if (!prepare("DELETE FROM payment WHERE id NOT IN (SELECT id FROM payment ORDER BY id DESC LIMIT ?)", old.slot(), failure))
            return false;
        sqlite3_bind_int(old.get(), 1, kept);
        if (sqlite3_step(old.get()) != SQLITE_DONE) {
            if (failure)
                *failure = lastError();
            return false;
        }
        return true;
    }, why);
}

bool LedgerDb::setPaymentsRead(qint64 upTo, QString* why)
{
    return writeInTransaction([this, upTo](QString* failure) {
        Statement set;
        if (!prepare("UPDATE payment SET is_read = 1 WHERE id <= ?", set.slot(), failure))
            return false;
        sqlite3_bind_int64(set.get(), 1, upTo);
        if (sqlite3_step(set.get()) != SQLITE_DONE) {
            if (failure)
                *failure = lastError();
            return false;
        }
        return true;
    }, why);
}

bool LedgerDb::deletePayment(qint64 id, QString* why)
{
    return writeInTransaction([this, id](QString* failure) {
        Statement remove;
        if (!prepare("DELETE FROM payment WHERE id = ?", remove.slot(), failure))
            return false;
        sqlite3_bind_int64(remove.get(), 1, id);
        if (sqlite3_step(remove.get()) != SQLITE_DONE) {
            if (failure)
                *failure = lastError();
            return false;
        }
        return true;
    }, why);
}

bool LedgerDb::savePaymentState(const QHash<QString, QString>& changes, QString* why)
{
    return writeInTransaction([this, &changes](QString* failure) {
        Statement put;
        Statement remove;
        if (!prepare("INSERT OR REPLACE INTO payment_state(key, value) VALUES(?,?)", put.slot(), failure)
            || !prepare("DELETE FROM payment_state WHERE key = ?", remove.slot(), failure))
            return false;
        for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
            sqlite3_stmt* one = it.value().isEmpty() ? remove.get() : put.get();
            bindText(one, 1, it.key());
            if (!it.value().isEmpty())
                bindText(one, 2, it.value());
            const int done = sqlite3_step(one);
            sqlite3_reset(one);
            if (done != SQLITE_DONE) {
                if (failure)
                    *failure = lastError();
                return false;
            }
        }
        return true;
    }, why);
}

QHash<QString, QString> LedgerDb::loadPaymentState(QString* why)
{
    QHash<QString, QString> out;
    Statement list;
    if (!db_ || !prepare("SELECT key, value FROM payment_state", list.slot(), why))
        return out;
    while (sqlite3_step(list.get()) == SQLITE_ROW)
        out.insert(columnText(list.get(), 0), columnText(list.get(), 1));
    return out;
}

QList<wallet::Payment> LedgerDb::loadPayments(QString* why)
{
    QList<wallet::Payment> out;
    Statement list;
    if (!db_ || !prepare("SELECT id, at, asset, amount, usd, wallet, address, addresses, txid, is_read, while_closed FROM payment ORDER BY id", list.slot(), why))
        return out;
    while (sqlite3_step(list.get()) == SQLITE_ROW) {
        wallet::Payment payment;
        payment.id = sqlite3_column_int64(list.get(), 0);
        payment.at = sqlite3_column_int64(list.get(), 1);
        payment.asset = columnText(list.get(), 2);
        const auto* amount = static_cast<const uint8_t*>(sqlite3_column_blob(list.get(), 3));
        const int amountBytes = sqlite3_column_bytes(list.get(), 3);
        payment.amount = chain::Amount::fromBigEndian(amount && amountBytes > 0 ? chain::Bytes(amount, amount + amountBytes) : chain::Bytes());
        payment.usd = sqlite3_column_double(list.get(), 4);
        payment.wallet = quint32(sqlite3_column_int64(list.get(), 5));
        payment.address = columnText(list.get(), 6);
        payment.addresses = sqlite3_column_int(list.get(), 7);
        if (sqlite3_column_bytes(list.get(), 8) == int(payment.txid.size())) {
            std::memcpy(payment.txid.data(), sqlite3_column_blob(list.get(), 8), payment.txid.size());
            payment.hasTx = true;
        }
        payment.read = sqlite3_column_int(list.get(), 9) != 0;
        payment.whileClosed = sqlite3_column_int(list.get(), 10) != 0;
        out.append(payment);
    }
    return out;
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

bool LedgerDb::writeHistory(int network, const wallet::HistoryDelta& delta, QString* why)
{
    const auto fail = [this, why] {
        if (why)
            *why = lastError();
        return false;
    };
    const auto ran = [](sqlite3_stmt* stmt) {
        const int done = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        return done == SQLITE_DONE;
    };

    if (!delta.removedTxs.isEmpty()) {
        Statement removeTx;
        if (!prepare("DELETE FROM tx WHERE network = ? AND txid = ?", removeTx.slot(), why))
            return false;
        for (const chain::Hash256& txid : delta.removedTxs) {
            sqlite3_bind_int(removeTx.get(), 1, network);
            bindHash(removeTx.get(), 2, txid);
            if (!ran(removeTx.get()))
                return fail();
        }
    }
    if (!delta.removedNotes.isEmpty()) {
        Statement removeNote;
        if (!prepare("DELETE FROM tx_note WHERE network = ? AND txid = ?", removeNote.slot(), why))
            return false;
        for (const chain::Hash256& txid : delta.removedNotes) {
            sqlite3_bind_int(removeNote.get(), 1, network);
            bindHash(removeNote.get(), 2, txid);
            if (!ran(removeNote.get()))
                return fail();
        }
    }
    if (!delta.removedAddresses.isEmpty()) {
        Statement remove;
        if (!prepare("DELETE FROM address_history WHERE network = ? AND scripthash = ?", remove.slot(), why))
            return false;
        for (const QString& scripthash : delta.removedAddresses) {
            sqlite3_bind_int(remove.get(), 1, network);
            bindScripthash(remove.get(), 2, scripthash);
            if (!ran(remove.get()))
                return fail();
        }
    }

    if (!delta.txs.isEmpty()) {
        Statement upsert;
        if (!prepare("INSERT INTO tx(network, txid, height, time, fee, vsize, out_total, seen, proven, replaceable, usd_then, "
                     "usd_then_at, replaced_by, inputs, block_pos) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
                     "ON CONFLICT(network, txid) DO UPDATE SET height=excluded.height, time=excluded.time, fee=excluded.fee, "
                     "block_pos=excluded.block_pos, "
                     "vsize=excluded.vsize, out_total=excluded.out_total, seen=excluded.seen, proven=excluded.proven, "
                     "replaceable=excluded.replaceable, usd_then=excluded.usd_then, usd_then_at=excluded.usd_then_at, "
                     "replaced_by=excluded.replaced_by, inputs=excluded.inputs RETURNING id",
                     upsert.slot(), why))
            return false;
        Statement clearOutputs;
        Statement putOutput;
        if (!prepare("DELETE FROM txo WHERE tx = ?", clearOutputs.slot(), why)
            || !prepare("INSERT INTO txo(tx, n, scripthash, value) VALUES(?,?,?,?)", putOutput.slot(), why))
            return false;
        QByteArray inputs;
        for (const wallet::TxRecord& tx : delta.txs) {
            sqlite3_stmt* u = upsert.get();
            sqlite3_bind_int(u, 1, network);
            bindHash(u, 2, tx.txid);
            sqlite3_bind_int(u, 3, tx.height);
            sqlite3_bind_int64(u, 4, tx.time);
            if (tx.fee >= 0)
                sqlite3_bind_int64(u, 5, tx.fee);
            else
                sqlite3_bind_null(u, 5);
            sqlite3_bind_int(u, 6, tx.vsize);
            sqlite3_bind_int64(u, 7, tx.outTotal);
            sqlite3_bind_int(u, 8, int(tx.seen));
            sqlite3_bind_int(u, 9, tx.proven ? 1 : 0);
            sqlite3_bind_int(u, 10, tx.replaceable ? 1 : 0);
            if (tx.usdThen > 0.0) {
                sqlite3_bind_double(u, 11, tx.usdThen);
                sqlite3_bind_int64(u, 12, tx.usdThenAt);
            } else {
                sqlite3_bind_null(u, 11);
                sqlite3_bind_null(u, 12);
            }
            if (tx.replacedBy != chain::Hash256{})
                bindHash(u, 13, tx.replacedBy);
            else
                sqlite3_bind_null(u, 13);
            inputs.resize(qsizetype(tx.inputs.size()) * kOutPointBytes);
            char* out = inputs.data();
            for (const chain::OutPoint& point : tx.inputs) {
                std::memcpy(out, point.txid.data(), point.txid.size());
                out += point.txid.size();
                std::memcpy(out, &point.n, sizeof point.n);
                out += sizeof point.n;
            }
            sqlite3_bind_blob(u, 14, inputs.constData(), int(inputs.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int(u, 15, tx.blockPos);
            if (sqlite3_step(u) != SQLITE_ROW) {
                const bool failed = fail();
                sqlite3_reset(u);
                return failed;
            }
            const qint64 id = sqlite3_column_int64(u, 0);
            sqlite3_reset(u);
            sqlite3_bind_int64(clearOutputs.get(), 1, id);
            if (!ran(clearOutputs.get()))
                return fail();
            for (const wallet::OurOutput& output : tx.outputs) {
                sqlite3_stmt* p = putOutput.get();
                sqlite3_bind_int64(p, 1, id);
                sqlite3_bind_int64(p, 2, output.n);
                bindScripthash(p, 3, output.scripthash);
                sqlite3_bind_int64(p, 4, output.value);
                if (!ran(p))
                    return fail();
            }
        }
    }

    if (!delta.addresses.isEmpty()) {
        Statement put;
        if (!prepare("INSERT OR REPLACE INTO address_history(network, scripthash, wallet, address, status) VALUES(?,?,?,?,?)", put.slot(), why))
            return false;
        for (const wallet::AddressHistory& address : delta.addresses) {
            sqlite3_bind_int(put.get(), 1, network);
            bindScripthash(put.get(), 2, address.scripthash);
            sqlite3_bind_int64(put.get(), 3, address.wallet);
            bindText(put.get(), 4, address.address);
            bindText(put.get(), 5, address.status);
            if (!ran(put.get()))
                return fail();
        }
    }

    if (delta.dropHeadersFrom >= 0) {
        Statement drop;
        if (!prepare("DELETE FROM header WHERE network = ? AND height >= ?", drop.slot(), why))
            return false;
        sqlite3_bind_int(drop.get(), 1, network);
        sqlite3_bind_int(drop.get(), 2, delta.dropHeadersFrom);
        if (!ran(drop.get()))
            return fail();
    }
    if (!delta.headers.isEmpty()) {
        Statement put;
        if (!prepare("INSERT OR REPLACE INTO header(network, height, raw) VALUES(?,?,?)", put.slot(), why))
            return false;
        for (auto it = delta.headers.cbegin(); it != delta.headers.cend(); ++it) {
            sqlite3_bind_int(put.get(), 1, network);
            sqlite3_bind_int(put.get(), 2, it.key());
            sqlite3_bind_blob(put.get(), 3, it.value().constData(), int(it.value().size()), SQLITE_TRANSIENT);
            if (!ran(put.get()))
                return fail();
        }
    }
    return true;
}

bool LedgerDb::saveHistory(wallet::Network network, const wallet::HistoryDelta& delta, QString* why)
{
    return writeInTransaction([this, network, &delta](QString* failure) {
        const int id = nameId(wallet::networkInfo(network).id, failure);
        return id >= 0 && writeHistory(id, delta, failure);
    }, why);
}

bool LedgerDb::loadHistory(wallet::Network network, int chunk, const std::function<void(QVector<wallet::TxRecord>&&)>& sink, QString* why)
{
    const auto id = nameIds_.constFind(wallet::networkInfo(network).id);
    if (!db_ || id == nameIds_.cend())
        return db_ != nullptr; // nothing was ever kept of it
    readDamaged_ = false;
    const auto failed = [this, why] {
        if (why)
            *why = lastError();
        readDamaged_ = damaged();
        return false;
    };
    // Both in the order of the transactions: a walk down the two together keeps no more than a chunk.
    // Not by the index on the network, which holds every row in another order and would have to read
    // and sort them all before the first came back.
    sqlite3_exec(db_, kLoadCache, nullptr, nullptr, nullptr);
    const struct UsualCache {
        sqlite3* db;
        ~UsualCache() { sqlite3_exec(db, kUsualCache, nullptr, nullptr, nullptr); }
    } usualCache{db_};
    Statement rows;
    Statement outputs;
    if (!prepare("SELECT id, txid, height, time, fee, vsize, out_total, seen, proven, replaceable, usd_then, usd_then_at, "
                 "replaced_by, inputs, block_pos FROM tx WHERE +network = ? ORDER BY id",
                 rows.slot(), why)
        || !prepare("SELECT tx, n, scripthash, value FROM txo WHERE tx IN (SELECT id FROM tx WHERE network = ?) ORDER BY tx, n",
                    outputs.slot(), why))
        return failed();
    sqlite3_bind_int(rows.get(), 1, *id);
    sqlite3_bind_int(outputs.get(), 1, *id);

    int outputStep = sqlite3_step(outputs.get());
    if (outputStep != SQLITE_ROW && outputStep != SQLITE_DONE)
        return failed();
    QVector<wallet::TxRecord> out;
    out.reserve(chunk);
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
        sqlite3_stmt* r = rows.get();
        const qint64 rowId = sqlite3_column_int64(r, 0);
        wallet::TxRecord tx;
        tx.network = network;
        tx.txid = columnHash(r, 1);
        tx.height = sqlite3_column_int(r, 2);
        tx.time = sqlite3_column_int64(r, 3);
        tx.fee = sqlite3_column_type(r, 4) == SQLITE_NULL ? -1 : sqlite3_column_int64(r, 4);
        tx.vsize = sqlite3_column_int(r, 5);
        tx.outTotal = sqlite3_column_int64(r, 6);
        tx.seen = wallet::TxSeen(sqlite3_column_int(r, 7));
        tx.proven = sqlite3_column_int(r, 8) != 0;
        tx.replaceable = sqlite3_column_int(r, 9) != 0;
        tx.usdThen = sqlite3_column_type(r, 10) == SQLITE_NULL ? 0.0 : sqlite3_column_double(r, 10);
        tx.usdThenAt = sqlite3_column_type(r, 11) == SQLITE_NULL ? 0 : sqlite3_column_int64(r, 11);
        if (sqlite3_column_type(r, 12) != SQLITE_NULL)
            tx.replacedBy = columnHash(r, 12);
        tx.blockPos = sqlite3_column_int(r, 14);
        const int inputBytes = sqlite3_column_bytes(r, 13);
        const auto* inputs = static_cast<const char*>(sqlite3_column_blob(r, 13));
        tx.inputs.resize(size_t(inputBytes / kOutPointBytes));
        for (size_t i = 0; i < tx.inputs.size(); ++i) {
            std::memcpy(tx.inputs[i].txid.data(), inputs + i * kOutPointBytes, tx.inputs[i].txid.size());
            std::memcpy(&tx.inputs[i].n, inputs + i * kOutPointBytes + tx.inputs[i].txid.size(), sizeof tx.inputs[i].n);
        }
        while (outputStep == SQLITE_ROW && sqlite3_column_int64(outputs.get(), 0) < rowId)
            outputStep = sqlite3_step(outputs.get());
        while (outputStep == SQLITE_ROW && sqlite3_column_int64(outputs.get(), 0) == rowId) {
            wallet::OurOutput output;
            output.n = quint32(sqlite3_column_int64(outputs.get(), 1));
            output.scripthash = columnScripthash(outputs.get(), 2);
            output.value = sqlite3_column_int64(outputs.get(), 3);
            tx.outputs.append(std::move(output));
            outputStep = sqlite3_step(outputs.get());
        }
        // Before anything else runs on the connection: the walk of the rows would replace the error.
        if (outputStep != SQLITE_ROW && outputStep != SQLITE_DONE)
            return failed();
        out.append(std::move(tx));
        if (out.size() >= chunk) {
            sink(std::exchange(out, {}));
            out.reserve(chunk);
        }
    }
    if (step != SQLITE_DONE)
        return failed();
    if (!out.isEmpty())
        sink(std::move(out));
    return true;
}

bool LedgerDb::readEnded(int step, QString* why)
{
    if (step == SQLITE_DONE)
        return true;
    if (why)
        *why = lastError();
    readDamaged_ = damaged();
    readFailed_ = true;
    return false;
}

QVector<wallet::AddressHistory> LedgerDb::loadAddressHistory(wallet::Network network, QString* why)
{
    readFailed_ = false;
    QVector<wallet::AddressHistory> out;
    const auto id = nameIds_.constFind(wallet::networkInfo(network).id);
    if (!db_ || id == nameIds_.cend())
        return out;
    Statement list;
    if (!prepare("SELECT scripthash, wallet, address, status FROM address_history WHERE network = ?", list.slot(), why)) {
        readEnded(SQLITE_ERROR, why);
        return out;
    }
    sqlite3_bind_int(list.get(), 1, *id);
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(list.get())) == SQLITE_ROW) {
        wallet::AddressHistory address;
        address.scripthash = columnScripthash(list.get(), 0);
        address.wallet = quint32(sqlite3_column_int64(list.get(), 1));
        address.address = columnText(list.get(), 2);
        address.status = columnText(list.get(), 3);
        out.append(std::move(address));
    }
    readEnded(step, why);
    return out;
}

QMap<int, QByteArray> LedgerDb::loadHeaders(wallet::Network network, QString* why)
{
    readFailed_ = false;
    QMap<int, QByteArray> out;
    const auto id = nameIds_.constFind(wallet::networkInfo(network).id);
    if (!db_ || id == nameIds_.cend())
        return out;
    Statement list;
    if (!prepare("SELECT height, raw FROM header WHERE network = ? ORDER BY height", list.slot(), why)) {
        readEnded(SQLITE_ERROR, why);
        return out;
    }
    sqlite3_bind_int(list.get(), 1, *id);
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(list.get())) == SQLITE_ROW) {
        const auto* bytes = static_cast<const char*>(sqlite3_column_blob(list.get(), 1));
        out.insert(sqlite3_column_int(list.get(), 0), QByteArray(bytes, sqlite3_column_bytes(list.get(), 1)));
    }
    readEnded(step, why);
    return out;
}

QHash<chain::Hash256, wallet::TxNote> LedgerDb::loadNotes(wallet::Network network, QString* why)
{
    readFailed_ = false;
    QHash<chain::Hash256, wallet::TxNote> out;
    const auto id = nameIds_.constFind(wallet::networkInfo(network).id);
    if (!db_ || id == nameIds_.cend())
        return out;
    Statement list;
    if (!prepare("SELECT txid, label, note FROM tx_note WHERE network = ?", list.slot(), why)) {
        readEnded(SQLITE_ERROR, why);
        return out;
    }
    sqlite3_bind_int(list.get(), 1, *id);
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(list.get())) == SQLITE_ROW)
        out.insert(columnHash(list.get(), 0), {columnText(list.get(), 1), columnText(list.get(), 2)});
    readEnded(step, why);
    return out;
}

bool LedgerDb::writeNotes(int network, const QHash<chain::Hash256, wallet::TxNote>& notes, QString* why)
{
    Statement put;
    Statement remove;
    if (!prepare("INSERT OR REPLACE INTO tx_note(network, txid, label, note) VALUES(?,?,?,?)", put.slot(), why)
        || !prepare("DELETE FROM tx_note WHERE network = ? AND txid = ?", remove.slot(), why))
        return false;
    for (auto it = notes.cbegin(); it != notes.cend(); ++it) {
        sqlite3_stmt* write = it->isEmpty() ? remove.get() : put.get();
        sqlite3_bind_int(write, 1, network);
        bindHash(write, 2, it.key());
        if (!it->isEmpty()) {
            bindText(write, 3, it->label);
            bindText(write, 4, it->note);
        }
        const int done = sqlite3_step(write);
        sqlite3_reset(write);
        if (done != SQLITE_DONE) {
            if (why)
                *why = lastError();
            return false;
        }
    }
    return true;
}

// These exist nowhere else, and a window has said they are kept: a power cut must not take that back.
bool LedgerDb::writeLabelsDurably(const std::function<bool(QString*)>& work, QString* why)
{
    if (db_)
        sqlite3_exec(db_, "PRAGMA synchronous = FULL", nullptr, nullptr, nullptr);
    const bool ok = writeInTransaction(work, why);
    if (db_)
        sqlite3_exec(db_, "PRAGMA synchronous = NORMAL", nullptr, nullptr, nullptr);
    return ok;
}

bool LedgerDb::saveNotes(wallet::Network network, const QHash<chain::Hash256, wallet::TxNote>& notes, QString* why)
{
    return writeLabelsDurably([this, network, &notes](QString* failure) {
        const int id = nameId(wallet::networkInfo(network).id, failure);
        return id >= 0 && writeNotes(id, notes, failure);
    }, why);
}

bool LedgerDb::saveNote(wallet::Network network, const chain::Hash256& txid, const wallet::TxNote& note, QString* why)
{
    QHash<chain::Hash256, wallet::TxNote> one;
    one.insert(txid, note);
    return saveNotes(network, one, why);
}

bool LedgerDb::writeLabels(const QHash<QString, QString>& labels, QString* why)
{
    Statement put;
    Statement remove;
    if (!prepare("INSERT OR REPLACE INTO address_label(address, label) VALUES(?,?)", put.slot(), why)
        || !prepare("DELETE FROM address_label WHERE address = ?", remove.slot(), why))
        return false;
    for (auto it = labels.cbegin(); it != labels.cend(); ++it) {
        sqlite3_stmt* write = it->isEmpty() ? remove.get() : put.get();
        bindText(write, 1, it.key());
        if (!it->isEmpty())
            bindText(write, 2, *it);
        const int done = sqlite3_step(write);
        sqlite3_reset(write);
        if (done != SQLITE_DONE) {
            if (why)
                *why = lastError();
            return false;
        }
    }
    return true;
}

bool LedgerDb::saveAddressLabels(const QHash<QString, QString>& labels, QString* why)
{
    return writeLabelsDurably([this, &labels](QString* failure) { return writeLabels(labels, failure); }, why);
}

bool LedgerDb::saveAddressLabel(const QString& address, const QString& label, QString* why)
{
    QHash<QString, QString> one;
    one.insert(address, label);
    return saveAddressLabels(one, why);
}

QHash<QString, QString> LedgerDb::loadAddressLabels(QString* why)
{
    readFailed_ = false;
    QHash<QString, QString> out;
    if (!db_)
        return out;
    Statement list;
    if (!prepare("SELECT address, label FROM address_label", list.slot(), why)) {
        readEnded(SQLITE_ERROR, why);
        return out;
    }
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(list.get())) == SQLITE_ROW)
        out.insert(columnText(list.get(), 0), columnText(list.get(), 1));
    readEnded(step, why);
    return out;
}

int SavedLabels::count() const
{
    int total = int(addresses.size());
    for (auto it = notes.cbegin(); it != notes.cend(); ++it)
        total += int(it->size());
    return total;
}

SavedLabels LedgerDb::salvageLabels(const QString& path, const QByteArray& key)
{
    SavedLabels out;
    sqlite3* raw = nullptr;
    const QByteArray file = path.toUtf8();
    if (sqlite3_open_v2(file.constData(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close_v2(raw);
        return out;
    }
    QByteArray pragma = "PRAGMA key = \"x'" + key.toHex() + "'\"";
    const int keyed = sqlite3_exec(raw, pragma.constData(), nullptr, nullptr, nullptr);
    pragma.fill('\0');
    // A file from before there were any has no such tables, and nothing to give up.
    int version = -1;
    if (keyed == SQLITE_OK) {
        sqlite3_stmt* asked = nullptr;
        if (sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &asked, nullptr) == SQLITE_OK && sqlite3_step(asked) == SQLITE_ROW)
            version = sqlite3_column_int(asked, 0);
        sqlite3_finalize(asked);
    }
    if (version >= 0 && version < 2) {
        out.read = true;
        out.whole = true;
    } else if (version >= 0) {
        // A page that fails ends a walk there. The rest of the pages are as good as they were, so
        // what the walk did not reach is walked to from the other end, up to the same page.
        const auto walk = [raw](const char* sql, const std::function<void(sqlite3_stmt*)>& row) {
            sqlite3_stmt* rows = nullptr;
            if (sqlite3_prepare_v2(raw, sql, -1, &rows, nullptr) != SQLITE_OK) {
                sqlite3_finalize(rows);
                return false;
            }
            int step = SQLITE_DONE;
            while ((step = sqlite3_step(rows)) == SQLITE_ROW)
                row(rows);
            sqlite3_finalize(rows);
            return step == SQLITE_DONE;
        };
        const auto note = [&out](sqlite3_stmt* row) {
            out.notes[columnText(row, 0)].insert(columnHash(row, 1), {columnText(row, 2), columnText(row, 3)});
        };
        const auto label = [&out](sqlite3_stmt* row) { out.addresses.insert(columnText(row, 0), columnText(row, 1)); };
        const char* notesUp = "SELECT n.text, t.txid, t.label, t.note FROM tx_note t JOIN name n ON n.id = t.network ORDER BY t.network, t.txid";
        const char* notesDown = "SELECT n.text, t.txid, t.label, t.note FROM tx_note t JOIN name n ON n.id = t.network ORDER BY t.network DESC, t.txid DESC";
        const char* labelsUp = "SELECT address, label FROM address_label ORDER BY address";
        const char* labelsDown = "SELECT address, label FROM address_label ORDER BY address DESC";
        out.read = true;
        out.whole = true;
        if (!walk(notesUp, note)) {
            out.whole = false;
            walk(notesDown, note);
        }
        if (!walk(labelsUp, label)) {
            out.whole = false;
            walk(labelsDown, label);
        }
    }
    sqlite3_close_v2(raw);
    return out;
}

bool LedgerDb::restoreLabels(const SavedLabels& labels, QString* why)
{
    return writeLabelsDurably([this, &labels](QString* failure) {
        const auto ran = [this, failure](sqlite3_stmt* write) {
            const int done = sqlite3_step(write);
            sqlite3_reset(write);
            if (done == SQLITE_DONE)
                return true;
            if (failure)
                *failure = lastError();
            return false;
        };
        Statement putNote;
        Statement putLabel;
        if (!prepare("INSERT OR IGNORE INTO tx_note(network, txid, label, note) VALUES(?,?,?,?)", putNote.slot(), failure)
            || !prepare("INSERT OR IGNORE INTO address_label(address, label) VALUES(?,?)", putLabel.slot(), failure))
            return false;
        for (auto network = labels.notes.cbegin(); network != labels.notes.cend(); ++network) {
            const int id = nameId(network.key(), failure);
            if (id < 0)
                return false;
            for (auto it = network->cbegin(); it != network->cend(); ++it) {
                sqlite3_bind_int(putNote.get(), 1, id);
                bindHash(putNote.get(), 2, it.key());
                bindText(putNote.get(), 3, it->label);
                bindText(putNote.get(), 4, it->note);
                if (!ran(putNote.get()))
                    return false;
            }
        }
        for (auto it = labels.addresses.cbegin(); it != labels.addresses.cend(); ++it) {
            bindText(putLabel.get(), 1, it.key());
            bindText(putLabel.get(), 2, *it);
            if (!ran(putLabel.get()))
                return false;
        }
        return true;
    }, why);
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
