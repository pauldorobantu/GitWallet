#include "app/StoreTest.h"

#include "chain/Amount.h"
#include "net/PriceFeed.h"
#include "store/Ledger.h"
#include "store/LedgerDb.h"
#include "wallet/Registry.h"
#include "wallet/UtxoScanner.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <sqlcipher/sqlite3.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSet>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <random>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace app {

namespace {

using Check = std::function<void(bool, const QString&)>;

// An HTTP server on this PC that answers as told: what a request's path starts with
// decides its status and body. Stands in for the price sources.
class FakeHttp : public QObject
{
public:
    struct Answer {
        int status = 200;
        QByteArray body;
    };

    FakeHttp()
    {
        server_.listen(QHostAddress::LocalHost);
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* peer = server_.nextPendingConnection()) {
                connect(peer, &QTcpSocket::disconnected, peer, &QObject::deleteLater);
                connect(peer, &QTcpSocket::readyRead, peer, [this, peer] { respond(peer); });
            }
        });
    }

    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort()); }
    void answer(const QString& pathStart, int status, const QByteArray& body) { answers_.insert(pathStart, {status, body}); }
    void clear() { answers_.clear(); }
    QStringList asked; // the paths of the requests so far

private:
    void respond(QTcpSocket* peer)
    {
        const QByteArray request = peer->readAll();
        const QByteArray path = request.split(' ').value(1);
        asked.append(QString::fromLatin1(path));
        Answer found{500, "no answer set"};
        for (auto it = answers_.cbegin(); it != answers_.cend(); ++it) {
            if (QString::fromLatin1(path).startsWith(it.key()))
                found = it.value();
        }
        const QByteArray head = "HTTP/1.1 " + QByteArray::number(found.status) + " X\r\nContent-Type: application/json\r\n"
                                "Connection: close\r\nContent-Length: " + QByteArray::number(found.body.size()) + "\r\n\r\n";
        peer->write(head + found.body);
        peer->flush();
        peer->disconnectFromHost();
    }

    QTcpServer server_;
    QHash<QString, Answer> answers_;
};

// Runs the event loop until `done` says so; false if `ms` went by first.
bool waitFor(const std::function<bool()>& done, int ms)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return done();
}

const wallet::NetworkInfo& bitcoin()
{
    return *wallet::findNetwork(QStringLiteral("bitcoin"));
}

const wallet::NetworkInfo& ethereum()
{
    return *wallet::findNetwork(QStringLiteral("ethereum"));
}

const wallet::AssetInfo& usdt()
{
    return *wallet::findAsset(QStringLiteral("ethereum/usdt"));
}

const wallet::AssetInfo& usdc()
{
    return *wallet::findAsset(QStringLiteral("ethereum/usdc"));
}

wallet::AddressRow utxoRow(quint32 wallet, quint32 index, qint64 confirmed, qint64 unconfirmed)
{
    wallet::AddressRow row;
    row.wallet = wallet;
    row.network = bitcoin().network;
    row.type = chain::ScriptType::P2WPKH;
    row.index = index;
    row.address = QStringLiteral("bc1qtestaddress%1x%2").arg(wallet).arg(index);
    row.scripthash = QStringLiteral("%1").arg(qulonglong(wallet) * 1000000 + index, 64, 16, QLatin1Char('0'));
    row.confirmed = confirmed;
    row.unconfirmed = unconfirmed;
    row.used = confirmed != 0 || unconfirmed != 0;
    row.checked = true;
    return row;
}

wallet::AddressRow evmRow(quint32 wallet, quint32 index)
{
    wallet::AddressRow row;
    row.wallet = wallet;
    row.network = ethereum().network;
    row.type = chain::ScriptType::EthAccount;
    row.index = index;
    row.address = QStringLiteral("0xtestaddress%1x%2").arg(wallet).arg(index);
    row.checked = true;
    return row;
}

chain::Amount bigAmount(const char* digits)
{
    chain::Amount out;
    chain::Amount::fromDecimal(digits, &out);
    return out;
}

void testRegistry(const Check& check)
{
    check(wallet::registryProblem().isEmpty(), QStringLiteral("the list of coins loads"));
    check(wallet::findNetwork(QStringLiteral("bitcoin")) && wallet::findNetwork(QStringLiteral("ethereum")),
          QStringLiteral("it names Bitcoin and Ethereum"));
    check(wallet::findAsset(QStringLiteral("bitcoin/btc")) && wallet::findAsset(QStringLiteral("ethereum/eth"))
              && wallet::findAsset(QStringLiteral("ethereum/usdt")) && wallet::findAsset(QStringLiteral("ethereum/usdc")),
          QStringLiteral("and the four assets by their names"));
    check(!wallet::findAsset(QStringLiteral("bitcoin/usdt")) && !wallet::findNetwork(QStringLiteral("nothing")),
          QStringLiteral("and nothing else by accident"));
    check(wallet::assetInfo(bitcoin().coin).symbol == QStringLiteral("BTC") && wallet::assetInfo(ethereum().coin).decimals == 18,
          QStringLiteral("each network knows its coin"));
    check(usdt().contract.compare(QStringLiteral("0xdAC17F958D2ee523a2206206994597C13D831ec7"), Qt::CaseInsensitive) == 0
              && usdc().contract.compare(QStringLiteral("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"), Qt::CaseInsensitive) == 0
              && usdt().decimals == 6 && usdc().decimals == 6,
          QStringLiteral("the token contracts and decimals are the ones checked on mainnet"));
    const chain::AddressFormat& own = bitcoin().format;
    const chain::AddressFormat& built = chain::defaultFormat(chain::ScriptType::P2WPKH);
    check(own.coinType == built.coinType && own.p2pkhVersion == built.p2pkhVersion && own.p2shVersion == built.p2shVersion
              && own.hrp == built.hrp,
          QStringLiteral("Bitcoin's address format in the list is the one the self-test proves"));
    check(ethereum().chainId == 1 && ethereum().multicall.compare(QStringLiteral("0xcA11bde05977b3631167028862bE2a173976CA11"),
                                                                  Qt::CaseInsensitive) == 0,
          QStringLiteral("Ethereum has its chain id and Multicall3"));
    check(wallet::capturedAccounts().size() == 4, QStringLiteral("a seed keeps its four accounts"));
    wallet::AccountKey key;
    key.type = chain::ScriptType::EthAccount;
    check(key.serves(ethereum().network) && !key.serves(bitcoin().network),
          QStringLiteral("an Evm key serves the Evm networks and no other"));
    key.type = chain::ScriptType::P2PKH;
    key.network = bitcoin().network;
    check(key.serves(bitcoin().network) && !key.serves(ethereum().network), QStringLiteral("a UTXO key serves its own network"));
}

void testFileFormats(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    const auto accountsJson = [&keys] {
        QJsonArray list;
        for (const wallet::AccountKey& key : keys)
            list.append(QJsonObject{{QStringLiteral("type"), QLatin1String(chain::scriptTypeId(key.type))},
                                    {QStringLiteral("account"), qint64(key.account)},
                                    {QStringLiteral("xpub"), key.xpub}});
        return list;
    };

    // As the release before the database wrote it.
    const QJsonObject oldSettings{{QStringLiteral("btcReceiveGap"), 321},
                                  {QStringLiteral("btcChangeGap"), 77},
                                  {QStringLiteral("ethGap"), 150},
                                  {QStringLiteral("ethRefreshSeconds"), 90},
                                  {QStringLiteral("electrumServers"), QJsonArray{QStringLiteral("own.example:50002")}},
                                  {QStringLiteral("ethEndpoints"), QJsonArray{QStringLiteral("https://own.example/rpc")}}};
    const QJsonObject two{{QStringLiteral("format"), QStringLiteral("gitwallet")},
                          {QStringLiteral("version"), 2},
                          {QStringLiteral("nextWalletId"), 5},
                          {QStringLiteral("wallets"),
                           QJsonArray{QJsonObject{{QStringLiteral("id"), 4},
                                                  {QStringLiteral("label"), QStringLiteral("Second format")},
                                                  {QStringLiteral("fromKeys"), false},
                                                  {QStringLiteral("accounts"), accountsJson()}}}},
                          {QStringLiteral("settings"), oldSettings}};
    check(wallet::WalletStore::writeRawForTest(QJsonDocument(two).toJson(QJsonDocument::Compact), &error),
          QStringLiteral("a file of the second format is in place"));
    wallet::WalletStore store;
    check(store.load(&error) && store.wallets.size() == 1 && store.wallets.first().id == 4,
          QStringLiteral("it loads as it was: ") + error);
    check(store.settings.utxoReceiveGap == 321 && store.settings.utxoChangeGap == 77 && store.settings.evmGap == 150
              && store.settings.evmRefreshSeconds == 90,
          QStringLiteral("its gaps keep their values under their new names"));
    check(store.settings.servers.value(QStringLiteral("bitcoin")) == QStringList{QStringLiteral("own.example:50002")}
              && store.settings.servers.value(QStringLiteral("ethereum")) == QStringList{QStringLiteral("https://own.example/rpc")},
          QStringLiteral("and its server lists go to their networks"));
    const QList<wallet::AccountKey> loaded = store.allAccounts();
    check(loaded.size() == 4 && loaded.first().network == bitcoin().network,
          QStringLiteral("a key without a network is Bitcoin's, as all were"));
    check(store.databaseKey.isEmpty(), QStringLiteral("it has no database key yet"));

    store.databaseKey = wallet::WalletStore::newDatabaseKey();
    check(store.databaseKey.size() == 32 && store.databaseKey != wallet::WalletStore::newDatabaseKey(),
          QStringLiteral("a database key is 32 random bytes"));
    check(store.save(&error), QStringLiteral("it saves in the current format: ") + error);
    wallet::WalletStore again;
    check(again.load(&error) && again.databaseKey == store.databaseKey && again.settings == store.settings
              && again.wallets.size() == 1 && again.allAccounts().size() == 4,
          QStringLiteral("and reads back the same, its database key with it: ") + error);

    // Keys a later version wrote stay in the file, unread.
    QJsonArray withUnknown = accountsJson();
    withUnknown.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("p2tr")},
                                   {QStringLiteral("network"), QStringLiteral("bitcoin")},
                                   {QStringLiteral("account"), 0},
                                   {QStringLiteral("xpub"), QStringLiteral("taproot-key-of-a-later-version")}});
    withUnknown.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("p2wpkh")},
                                   {QStringLiteral("network"), QStringLiteral("a-network-not-listed")},
                                   {QStringLiteral("account"), 0},
                                   {QStringLiteral("xpub"), QStringLiteral("key-of-another-network")}});
    QJsonObject three = two;
    three.insert(QStringLiteral("version"), 3);
    three.insert(QStringLiteral("wallets"), QJsonArray{QJsonObject{{QStringLiteral("id"), 4},
                                                                   {QStringLiteral("label"), QStringLiteral("Third format")},
                                                                   {QStringLiteral("fromKeys"), false},
                                                                   {QStringLiteral("accounts"), withUnknown}}});
    check(wallet::WalletStore::writeRawForTest(QJsonDocument(three).toJson(QJsonDocument::Compact), &error),
          QStringLiteral("a file with keys this version does not know is in place"));
    wallet::WalletStore future;
    check(future.load(&error) && future.allAccounts().size() == 4 && future.wallets.first().keptAccounts.size() == 2,
          QStringLiteral("it loads, and those keys are not scanned: ") + error);
    check(future.save(&error), QStringLiteral("it saves: ") + error);
    wallet::WalletStore kept;
    check(kept.load(&error) && kept.wallets.first().keptAccounts.size() == 2,
          QStringLiteral("and they are still in the file: ") + error);

    QJsonObject twoUnknown = two;
    twoUnknown.insert(QStringLiteral("wallets"), three.value(QStringLiteral("wallets")));
    wallet::WalletStore::writeRawForTest(QJsonDocument(twoUnknown).toJson(QJsonDocument::Compact), &error);
    wallet::WalletStore damaged;
    check(!damaged.load(&error) && error.contains(QStringLiteral("unknown type")),
          QStringLiteral("in a file from before there were any, such a key means damage"));

    QJsonObject newer = three;
    newer.insert(QStringLiteral("version"), 99);
    wallet::WalletStore::writeRawForTest(QJsonDocument(newer).toJson(QJsonDocument::Compact), &error);
    wallet::WalletStore tooNew;
    check(!tooNew.load(&error) && error.contains(QStringLiteral("newer version")), QStringLiteral("a newer format is refused and says so"));
    QFile::remove(wallet::WalletStore::filePath());
}

void testAmountsAndTotals(const Check& check)
{
    chain::Amount amount = bigAmount("1000000000000000000000000");
    amount -= bigAmount("1");
    check(amount == bigAmount("999999999999999999999999"), QStringLiteral("an amount of any size subtracts"));
    amount -= bigAmount("1000000000000000000000000");
    check(amount.isZero(), QStringLiteral("and stops at zero"));

    // Random rows and random changes, on both kinds of network: the totals kept as they
    // go must be what adding every row up gives.
    QRandomGenerator random(20261102);
    wallet::WalletModel model;
    const auto someEvm = [&random](quint32 wallet, quint32 index) {
        wallet::AddressRow row = evmRow(wallet, index);
        if (random.bounded(3) == 0)
            row.setBalance(ethereum().coin, bigAmount("1234567890123456789012"));
        if (random.bounded(3) == 0)
            row.setBalance(usdt().asset, chain::Amount::fromU64(random.bounded(1000000)));
        row.setBalance(usdc().asset, chain::Amount::fromU64(random.bounded(2) * 500));
        row.checked = random.bounded(4) != 0;
        return row;
    };
    for (int round = 0; round < 4000; ++round) {
        const quint32 wallet = 1 + random.bounded(3);
        const quint32 index = random.bounded(40);
        if (random.bounded(2) == 0)
            model.upsert(someEvm(wallet, index));
        else
            model.upsert(utxoRow(wallet, index, random.bounded(5) * 1000, qint64(random.bounded(3) - 1) * 300));
    }
    bool same = true;
    for (const wallet::AssetInfo& info : wallet::assets()) {
        chain::Amount all;
        QHash<quint32, chain::Amount> own;
        for (int i = 0; i < model.size(); ++i) {
            const chain::Amount value = model.at(i).amount(info.asset);
            all += value;
            own[model.at(i).wallet] += value;
        }
        same = same && all == model.total(info.asset);
        for (auto it = own.cbegin(); it != own.cend(); ++it)
            same = same && it.value() == model.total(info.asset, it.key());
    }
    check(same, QStringLiteral("4,000 random changes: every total, overall and per wallet, is the sum of its rows"));
    int rowsBitcoin = 0;
    int uncheckedEthereum = 0;
    for (int i = 0; i < model.size(); ++i) {
        rowsBitcoin += model.at(i).network == bitcoin().network;
        uncheckedEthereum += model.at(i).network == ethereum().network && !model.at(i).checked;
    }
    check(model.rowsOn(bitcoin().network) == rowsBitcoin && model.uncheckedOn(ethereum().network) == uncheckedEthereum,
          QStringLiteral("and the counts per network are right too"));
    wallet::AddressRow emptied = evmRow(1, 0);
    emptied.setBalance(ethereum().coin, chain::Amount());
    check(emptied.balances.isEmpty() && emptied.state() == wallet::AddressState::Unused,
          QStringLiteral("a zero is not kept as a balance"));

    // The database hands over what it kept: nothing is doubled, and what is here stays.
    wallet::WalletModel saved;
    saved.upsert(utxoRow(9, 0, 700, 0));
    QVector<wallet::AddressRow> rows{utxoRow(9, 0, 111, 0), utxoRow(9, 1, 222, 0)};
    int appended = 0;
    int totalsMoved = 0;
    QObject::connect(&saved, &wallet::WalletModel::rowsAppended, &saved, [&appended](int first, int last) { appended += last - first + 1; });
    QObject::connect(&saved, &wallet::WalletModel::totalsChanged, &saved, [&totalsMoved] { ++totalsMoved; });
    saved.addSaved(rows);
    check(saved.size() == 2 && saved.total(bitcoin().coin) == chain::Amount::fromU64(922) && appended == 1 && totalsMoved == 1,
          QStringLiteral("rows from the database keep what a scanner already wrote, with one signal each"));

    // A history as an Electrum server lists it hashes to the status it would subscribe to.
    // The expected value is from Python's hashlib: sha256(b"aa...aa:123:bb...bb:0:").
    QString status;
    const QJsonArray history{QJsonObject{{QStringLiteral("tx_hash"), QStringLiteral("a").repeated(64)}, {QStringLiteral("height"), 123}},
                             QJsonObject{{QStringLiteral("tx_hash"), QStringLiteral("b").repeated(64)}, {QStringLiteral("height"), 0}}};
    check(wallet::UtxoScanner::statusOfHistory(history, &status) == 1
              && status == QStringLiteral("f6a4e530563709d43172f582814e1a1a53228f80c7ce3360ef3eb112058a2950"),
          QStringLiteral("a history hashes to the protocol's status: ") + status);
    check(wallet::UtxoScanner::statusOfHistory(QJsonArray(), &status) == 0 && status.isEmpty(),
          QStringLiteral("no history is no status"));
    check(wallet::UtxoScanner::statusOfHistory(QJsonArray{QJsonObject{{QStringLiteral("tx_hash"), QStringLiteral("short")}}}, &status) < 0
              && wallet::UtxoScanner::statusOfHistory(QJsonValue(5), &status) < 0,
          QStringLiteral("and what is not a history is refused"));
}

void testLedgerDb(const Check& check)
{
    QString why;
    const QString path = wallet::WalletStore::dataDir() + QStringLiteral("/ledger-test.db");
    const auto removeFiles = [&path] {
        QFile::remove(path);
        QFile::remove(path + QStringLiteral("-wal"));
        QFile::remove(path + QStringLiteral("-shm"));
    };
    removeFiles();
    const QByteArray key = wallet::WalletStore::newDatabaseKey();

    const wallet::AddressRow utxo = utxoRow(7, 3, 123456789, -5000);
    wallet::AddressRow evm = evmRow(7, 4);
    evm.setBalance(ethereum().coin, bigAmount("1234567890123456789012345"));
    evm.setBalance(usdt().asset, chain::Amount::fromU64(42));
    evm.nonce = 17;
    evm.used = true;
    wallet::AddressRow withStatus = utxoRow(8, 0, 1, 0);
    withStatus.status = QStringLiteral("c").repeated(64);

    {
        store::LedgerDb db;
        check(db.open(path, key, &why) == store::LedgerDb::Open::Ready, QStringLiteral("a new database opens: ") + why);
        check(db.saveRows({utxo, evm, withStatus}, &why), QStringLiteral("rows are saved: ") + why);
        check(db.rowCount() == 3, QStringLiteral("three rows"));
        check(db.savePrices({{QStringLiteral("bitcoin/btc"), 78000.5, QDateTime::fromSecsSinceEpoch(1793587557, QTimeZone::UTC), QStringLiteral("DefiLlama")}}, &why),
              QStringLiteral("a price is saved: ") + why);
        check(db.saveTimes({{QStringLiteral("bitcoin"), QDateTime::fromSecsSinceEpoch(1793587000, QTimeZone::UTC)}}, &why),
              QStringLiteral("a time is saved: ") + why);
    }
    QByteArray raw;
    for (const QString& file : {path, path + QStringLiteral("-wal")}) {
        QFile f(file);
        if (f.open(QIODevice::ReadOnly))
            raw += f.readAll();
    }
    check(raw.size() > 1000 && !raw.contains("bc1qtestaddress") && !raw.contains("0xtestaddress") && !raw.contains("SQLite format")
              && !raw.contains("DefiLlama"),
          QStringLiteral("nothing in the files can be read without the key"));

    {
        store::LedgerDb db;
        check(db.open(path, wallet::WalletStore::newDatabaseKey(), &why) == store::LedgerDb::Open::NotOurs,
              QStringLiteral("another key cannot open it, and it says so"));
    }
    {
        store::LedgerDb db;
        check(db.open(path, key, &why) == store::LedgerDb::Open::Ready, QStringLiteral("its key opens it again: ") + why);
        QVector<wallet::AddressRow> rows;
        check(db.loadRows({7, 8}, 2, [&rows](QVector<wallet::AddressRow>&& chunk) { rows += chunk; }, &why) && rows.size() == 3,
              QStringLiteral("the rows read back: ") + why);
        const auto same = [](const wallet::AddressRow& a, const wallet::AddressRow& b) {
            return a.wallet == b.wallet && a.network == b.network && a.type == b.type && a.account == b.account
                   && a.change == b.change && a.index == b.index && a.address == b.address && a.scripthash == b.scripthash
                   && a.checked == b.checked && a.used == b.used && a.confirmed == b.confirmed
                   && a.unconfirmed == b.unconfirmed && a.balances == b.balances && a.nonce == b.nonce && a.status == b.status;
        };
        check(rows.size() == 3 && same(rows.at(0), utxo) && same(rows.at(1), evm) && same(rows.at(2), withStatus),
              QStringLiteral("every field as it was written, 25-digit amounts and negative ones included"));
        const QList<store::SavedPrice> prices = db.loadPrices(&why);
        check(prices.size() == 1 && prices.first().usd == 78000.5 && prices.first().at.toSecsSinceEpoch() == 1793587557
                  && prices.first().source == QStringLiteral("DefiLlama"),
              QStringLiteral("the price reads back with its time and source"));
        const QList<store::SavedTime> times = db.loadTimes(&why);
        check(times.size() == 1 && times.first().network == QStringLiteral("bitcoin") && times.first().at.toSecsSinceEpoch() == 1793587000,
              QStringLiteral("and the scan time"));

        // Saved again: replaced, not doubled, and a balance gone is gone.
        wallet::AddressRow spent = evm;
        spent.setBalance(usdt().asset, chain::Amount());
        check(db.saveRows({spent}, &why) && db.rowCount() == 3, QStringLiteral("a row saved again replaces itself: ") + why);
        rows.clear();
        db.loadRows({7, 8}, 100, [&rows](QVector<wallet::AddressRow>&& chunk) { rows += chunk; }, &why);
        check(rows.size() == 3 && rows.at(1).balances.size() == 1 && rows.at(1).amount(usdt().asset).isZero(),
              QStringLiteral("and a balance that went leaves no trace"));

        // A wallet that is gone: its rows leave at the next read.
        rows.clear();
        check(db.loadRows({7}, 100, [&rows](QVector<wallet::AddressRow>&& chunk) { rows += chunk; }, &why) && rows.size() == 2 && db.rowCount() == 2,
              QStringLiteral("rows of a wallet that is gone do not come back, and leave the file"));
        check(db.removeWallet(7, &why) && db.rowCount() == 0, QStringLiteral("removing a wallet takes its rows"));
        check(db.saveRows({utxo, evm}, &why) && db.replaceRows({withStatus}, &why) && db.rowCount() == 1,
              QStringLiteral("replacing leaves those rows and no other"));
    }

    // A row of a network not on the list now stays in the file, off the screen.
    {
        sqlite3* raw3 = nullptr;
        sqlite3_open_v2(path.toUtf8().constData(), &raw3, SQLITE_OPEN_READWRITE, nullptr);
        const QByteArray pragma = "PRAGMA key = \"x'" + key.toHex() + "'\"";
        sqlite3_exec(raw3, pragma.constData(), nullptr, nullptr, nullptr);
        const int put = sqlite3_exec(raw3,
                                     "INSERT INTO name(text) VALUES('a-network-not-listed');"
                                     "INSERT INTO address(wallet, network, type, account, branch, idx, address, checked, used, confirmed, unconfirmed, nonce) "
                                     "SELECT 8, (SELECT id FROM name WHERE text='a-network-not-listed'), (SELECT id FROM name WHERE text='p2wpkh'), 0, 0, 99, 'other', 1, 0, 0, 0, 0;",
                                     nullptr, nullptr, nullptr);
        sqlite3_close_v2(raw3);
        store::LedgerDb db;
        db.open(path, key, &why);
        QVector<wallet::AddressRow> rows;
        const bool read = db.loadRows({8}, 100, [&rows](QVector<wallet::AddressRow>&& chunk) { rows += chunk; }, &why);
        check(put == SQLITE_OK && read && rows.size() == 1 && db.rowCount() == 2,
              QStringLiteral("a row of a network not on the list stays in the file, and off the screen"));
    }
    removeFiles();

    QFile garbage(path);
    garbage.open(QIODevice::WriteOnly);
    garbage.write(QByteArray(8192, 'x'));
    garbage.close();
    {
        store::LedgerDb db;
        check(db.open(path, key, &why) == store::LedgerDb::Open::NotOurs, QStringLiteral("a file that is no database is not taken for one"));
    }
    QFile checkFile(path);
    check(checkFile.open(QIODevice::ReadOnly) && checkFile.readAll() == QByteArray(8192, 'x'), QStringLiteral("and is left as it was"));
    checkFile.close();
    removeFiles();
}

void testLedgerAsync(const Check& check)
{
    const QString path = wallet::WalletStore::dataDir() + QStringLiteral("/ledger-async.db");
    const QByteArray key = wallet::WalletStore::newDatabaseKey();
    wallet::AddressRow evm = evmRow(3, 0);
    evm.setBalance(usdt().asset, chain::Amount::fromU64(5000000));
    evm.used = true;
    {
        wallet::WalletModel model;
        store::Ledger ledger(&model);
        bool loaded = false;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        ledger.open(path, key, {3});
        check(waitFor([&loaded] { return loaded; }, 15000) && !ledger.isLoading() && model.size() == 0,
              QStringLiteral("a new database has nothing to hand over"));
        model.upsert(utxoRow(3, 0, 1000, 0));
        model.upsert(evm);
        ledger.savePrices({{QStringLiteral("ethereum/usdt"), 0.999, QDateTime::fromSecsSinceEpoch(1793587557, QTimeZone::UTC), QStringLiteral("DefiLlama")}});
        // Closed right after the changes: what waited is written before close() returns.
    }
    {
        wallet::WalletModel model;
        store::Ledger ledger(&model);
        QList<store::SavedPrice> prices;
        bool loaded = false;
        int rewrites = 0;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger,
                         [&](const QList<store::SavedPrice>& p, const QList<store::SavedTime>&) { prices = p; loaded = true; });
        QObject::connect(&model, &wallet::WalletModel::cleared, &ledger, [&rewrites] { ++rewrites; });
        ledger.open(path, key, {3});
        check(waitFor([&loaded] { return loaded; }, 15000) && model.size() == 2,
              QStringLiteral("what a closing window saved is there at the next start"));
        check(model.total(usdt().asset) == chain::Amount::fromU64(5000000) && model.total(bitcoin().coin) == chain::Amount::fromU64(1000),
              QStringLiteral("with its totals"));
        check(prices.size() == 1 && prices.first().usd == 0.999, QStringLiteral("and its prices"));

        // A wallet removed: its rows go, whatever is on its way.
        model.upsert(utxoRow(4, 0, 77, 0));
        ledger.flush();
        ledger.forgetWallet(4);
        model.removeWallet(4);
    }
    {
        wallet::WalletModel model;
        store::Ledger ledger(&model);
        bool loaded = false;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        ledger.open(path, key, {3, 4});
        check(waitFor([&loaded] { return loaded; }, 15000) && model.size() == 2 && model.counts(4).rows == 0,
              QStringLiteral("a wallet removed does not come back"));
        model.clear();
        ledger.flush();
    }
    {
        wallet::WalletModel model;
        store::Ledger ledger(&model);
        bool loaded = false;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        ledger.open(path, key, {3});
        check(waitFor([&loaded] { return loaded; }, 15000) && model.size() == 0, QStringLiteral("rows cleared stay cleared"));
        model.upsert(utxoRow(3, 1, 5, 0));
    }
    {
        // Another key: kept aside and said so, and a new one takes over.
        wallet::WalletModel model;
        store::Ledger ledger(&model);
        bool loaded = false;
        QString notice;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        QObject::connect(&ledger, &store::Ledger::setAside, &ledger, [&notice](const QString& file) { notice = file; });
        ledger.open(path, wallet::WalletStore::newDatabaseKey(), {3});
        check(waitFor([&loaded] { return loaded; }, 15000) && model.size() == 0 && ledger.isOpen(),
              QStringLiteral("a database under another key is not read"));
        // With what says its labels have yet to come over, for when that key is back.
        QStringList aside = store::Ledger::keptAsideFiles(path);
        aside.removeIf([](const QString& name) { return name.endsWith(QLatin1String(".labels-pending")); });
        check(aside.size() == 1 && notice.endsWith(aside.value(0)), QStringLiteral("it is kept aside, and the user is told its name"));
        ledger.destroy(path);
        check(!QFile::exists(path) && !QFile::exists(path + QStringLiteral("-wal"))
                  && store::Ledger::keptAsideFiles(path).isEmpty(),
              QStringLiteral("removing everything leaves neither the database nor what was kept aside"));
    }
}

qint64 processMemoryMb()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters))
        return qint64(counters.WorkingSetSize) >> 20;
#endif
    return 0;
}

void testPrices(const Check& check)
{
    FakeHttp http;
    net::PriceFeed feed;
    feed.useTestServer(http.base());
    const wallet::AssetInfo& btc = wallet::assetInfo(bitcoin().coin);
    const wallet::AssetInfo& eth = wallet::assetInfo(ethereum().coin);
    int updates = 0;
    QStringList failures;
    QObject::connect(&feed, &net::PriceFeed::updated, &feed, [&updates] { ++updates; });
    QObject::connect(&feed, &net::PriceFeed::failed, &feed, [&failures](const QString& why) { failures << why; });
    const auto refreshed = [&feed, &updates, &failures](int before, int failed) {
        feed.refresh();
        return waitFor([&] { return updates > before || failures.size() > failed; }, 15000)
               && waitFor([&] { return !feed.isBusy(); }, 15000);
    };

    const auto llama = [](const QString& id, double price, qint64 at, double confidence = 0.99) {
        return QJsonObject{{id, QJsonObject{{QStringLiteral("price"), price}, {QStringLiteral("timestamp"), at}, {QStringLiteral("confidence"), confidence}}}};
    };
    // Recent: a price much older than this hour goes on to the next source.
    const qint64 first = QDateTime::currentSecsSinceEpoch() - 1200;
    const qint64 second = first + 600;
    const auto coins = [](std::initializer_list<QJsonObject> parts) {
        QJsonObject all;
        for (const QJsonObject& part : parts)
            for (auto it = part.constBegin(); it != part.constEnd(); ++it)
                all.insert(it.key(), it.value());
        return QJsonDocument(QJsonObject{{QStringLiteral("coins"), all}}).toJson(QJsonDocument::Compact);
    };

    // The first source answers everything.
    http.answer(QStringLiteral("/defillama"), 200,
                coins({llama(btc.defillamaId, 78000.5, first), llama(eth.defillamaId, 2291.25, first - 57),
                       llama(usdt().defillamaId, 0.999, first), llama(usdc().defillamaId, 0.9996, first)}));
    // With nothing to compare a first price to, the next source has its say, and agrees.
    http.answer(QStringLiteral("/coingecko"), 200,
                QJsonDocument(QJsonObject{{btc.coingeckoId, QJsonObject{{QStringLiteral("usd"), 78010.0}, {QStringLiteral("last_updated_at"), first - 100}}},
                                          {eth.coingeckoId, QJsonObject{{QStringLiteral("usd"), 2290.0}, {QStringLiteral("last_updated_at"), first - 100}}}})
                    .toJson(QJsonDocument::Compact));
    check(refreshed(updates, int(failures.size())) && feed.usd(btc.asset) == 78000.5 && feed.usd(usdt().asset) == 0.999,
          QStringLiteral("the first source prices every asset"));
    check(feed.sources({btc.asset, eth.asset}) == QStringLiteral("DefiLlama")
              && feed.oldest({btc.asset, eth.asset}) == QDateTime::fromSecsSinceEpoch(first - 57, QTimeZone::UTC),
          QStringLiteral("and says where from, and how old the oldest is"));
    check(failures.isEmpty() && http.asked.filter(QStringLiteral("/coingecko")).size() == 1 && http.asked.filter(QStringLiteral("/coinbase")).isEmpty(),
          QStringLiteral("a first price is confirmed by the next source, which goes no further"));
    http.asked.clear();
    check(refreshed(updates, int(failures.size())) && http.asked.size() == 1 && http.asked.first().startsWith(QStringLiteral("/defillama")),
          QStringLiteral("once confirmed, it asks nobody else"));

    // Part of it missing or doubtful: the rest comes from the next source.
    http.clear();
    http.answer(QStringLiteral("/defillama"), 200,
                coins({llama(btc.defillamaId, 78100.0, second), llama(eth.defillamaId, 1.0, second, 0.1),
                       llama(usdt().defillamaId, -5.0, second)}));
    http.answer(QStringLiteral("/coingecko"), 200,
                QJsonDocument(QJsonObject{{eth.coingeckoId, QJsonObject{{QStringLiteral("usd"), 2300.0}, {QStringLiteral("last_updated_at"), second + 100}}}})
                    .toJson(QJsonDocument::Compact));
    http.answer(QStringLiteral("/coinbase"), 200,
                QJsonDocument(QJsonObject{{QStringLiteral("data"), QJsonObject{{QStringLiteral("rates"), QJsonObject{{usdt().coinbaseSymbol, QStringLiteral("1.002")}, {usdc().coinbaseSymbol, QStringLiteral("not a number")}}}}}})
                    .toJson(QJsonDocument::Compact));
    check(refreshed(updates, int(failures.size())) && feed.usd(btc.asset) == 78100.0, QStringLiteral("what the first source answers counts"));
    check(feed.usd(eth.asset) == 2300.0 && feed.sources({eth.asset}) == QStringLiteral("CoinGecko"),
          QStringLiteral("a price it doubts goes to the next source"));
    check(std::abs(feed.usd(usdt().asset) - 1.0 / 1.002) < 1e-9 && feed.sources({usdt().asset}) == QStringLiteral("Coinbase"),
          QStringLiteral("a price it gets wrong goes on to the last, as what a rate means there"));
    check(feed.usd(usdc().asset) == 0.9996 && feed.sources({usdc().asset}) == QStringLiteral("DefiLlama"),
          QStringLiteral("what nobody answers keeps its last price, source and time"));

    // Every source down: everything stays, and says how old it is.
    http.clear();
    http.answer(QStringLiteral("/defillama"), 429, "slow down");
    http.answer(QStringLiteral("/coingecko"), 500, "down");
    http.answer(QStringLiteral("/coinbase"), 200, "{not json");
    const int before = int(failures.size());
    check(refreshed(updates, before) && failures.size() == before + 1, QStringLiteral("with every source down, the failure is reported once"));
    check(failures.last().contains(QStringLiteral("429")) && failures.last().contains(QStringLiteral("CoinGecko")),
          QStringLiteral("naming each source"));
    check(feed.usd(btc.asset) == 78100.0 && feed.oldest({btc.asset}) == QDateTime::fromSecsSinceEpoch(second, QTimeZone::UTC),
          QStringLiteral("and the prices stay, with the time they are from"));

    // What the database kept: an older price does not replace a newer one.
    net::PriceQuote older;
    older.asset = btc.asset;
    older.usd = 50000.0;
    older.at = QDateTime::fromSecsSinceEpoch(1700000000, QTimeZone::UTC);
    older.source = QStringLiteral("DefiLlama");
    feed.restore({older});
    check(feed.usd(btc.asset) == 78100.0, QStringLiteral("a saved price never replaces a newer one"));
    net::PriceFeed fresh;
    fresh.restore({older});
    net::PriceQuote bad = older;
    bad.asset = eth.asset;
    bad.usd = -1.0;
    fresh.restore({bad});
    check(fresh.usd(btc.asset) == 50000.0 && fresh.usd(eth.asset) == 0.0 && fresh.oldest({btc.asset}) == older.at,
          QStringLiteral("at the start it shows, with its time, and a bad one is dropped"));

    // A source that is behind.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    http.clear();
    http.answer(QStringLiteral("/defillama"), 200, coins({llama(btc.defillamaId, 70000.0, second - 300)}));
    http.answer(QStringLiteral("/coingecko"), 500, "down");
    http.answer(QStringLiteral("/coinbase"), 500, "down");
    refreshed(updates, int(failures.size()));
    check(feed.usd(btc.asset) == 78100.0, QStringLiteral("an older answer does not replace a newer price"));

    http.clear();
    http.answer(QStringLiteral("/defillama"), 200, coins({llama(eth.defillamaId, 2200.0, now - 5000)}));
    http.answer(QStringLiteral("/coingecko"), 200,
                QJsonDocument(QJsonObject{{eth.coingeckoId, QJsonObject{{QStringLiteral("usd"), 2310.0}, {QStringLiteral("last_updated_at"), now - 10}}}})
                    .toJson(QJsonDocument::Compact));
    http.answer(QStringLiteral("/coinbase"), 500, "down");
    refreshed(updates, int(failures.size()));
    check(feed.usd(eth.asset) == 2310.0 && feed.sources({eth.asset}) == QStringLiteral("CoinGecko"),
          QStringLiteral("an hour-old price goes to the next source"));

    net::PriceFeed lastResort;
    lastResort.useTestServer(http.base());
    int lastUpdates = 0;
    QObject::connect(&lastResort, &net::PriceFeed::updated, &lastResort, [&lastUpdates] { ++lastUpdates; });
    http.clear();
    http.answer(QStringLiteral("/defillama"), 200, coins({llama(btc.defillamaId, 69000.0, now - 5000)}));
    http.answer(QStringLiteral("/coingecko"), 500, "down");
    http.answer(QStringLiteral("/coinbase"), 500, "down");
    lastResort.refresh();
    check(waitFor([&lastResort] { return !lastResort.isBusy(); }, 15000) && lastResort.usd(btc.asset) == 69000.0
              && lastResort.oldest({btc.asset}) == QDateTime::fromSecsSinceEpoch(now - 5000, QTimeZone::UTC),
          QStringLiteral("and comes back, with its time, if no source has a newer one"));
    check(lastResort.lastRefreshFailed(), QStringLiteral("a refresh that left assets without a price says it failed"));

    net::PriceFeed strict;
    strict.useTestServer(http.base());
    http.clear();
    QJsonObject noConfidence{{QStringLiteral("price"), 80000.0}, {QStringLiteral("timestamp"), now}};
    QJsonObject wrongSymbol{{QStringLiteral("price"), 2000.0}, {QStringLiteral("timestamp"), now}, {QStringLiteral("confidence"), 0.99}, {QStringLiteral("symbol"), QStringLiteral("NOTETH")}};
    http.answer(QStringLiteral("/defillama"), 200,
                QJsonDocument(QJsonObject{{QStringLiteral("coins"), QJsonObject{{btc.defillamaId, noConfidence}, {eth.defillamaId, wrongSymbol}}}}).toJson(QJsonDocument::Compact));
    http.answer(QStringLiteral("/coingecko"), 200,
                QJsonDocument(QJsonObject{{usdt().coingeckoId, QJsonObject{{QStringLiteral("usd"), 1000.0}, {QStringLiteral("last_updated_at"), now}}},
                                          {usdc().coingeckoId, QJsonObject{{QStringLiteral("usd"), 1.0}}}})
                    .toJson(QJsonDocument::Compact));
    http.answer(QStringLiteral("/coinbase"), 500, "down");
    strict.refresh();
    waitFor([&strict] { return !strict.isBusy(); }, 15000);
    check(strict.usd(btc.asset) == 0.0, QStringLiteral("a price with no confidence said about it is not taken"));
    check(strict.usd(eth.asset) == 0.0, QStringLiteral("nor one that names another asset"));
    check(strict.usd(usdt().asset) == 0.0, QStringLiteral("nor a dollar coin far from a dollar"));
    check(strict.usd(usdc().asset) == 0.0, QStringLiteral("nor one with no time"));

    http.clear();
    http.answer(QStringLiteral("/defillama"), 200, coins({llama(btc.defillamaId, 900000.0, now + 86400)}));
    http.answer(QStringLiteral("/coingecko"), 500, "down");
    http.answer(QStringLiteral("/coinbase"), 500, "down");
    refreshed(updates, int(failures.size()));
    check(feed.usd(btc.asset) == 78100.0 && feed.oldest({btc.asset}) == QDateTime::fromSecsSinceEpoch(second, QTimeZone::UTC),
          QStringLiteral("nor ten times the price held a moment ago"));
    http.clear();
    http.answer(QStringLiteral("/defillama"), 200, coins({llama(btc.defillamaId, 78200.0, now + 86400)}));
    http.answer(QStringLiteral("/coingecko"), 500, "down");
    http.answer(QStringLiteral("/coinbase"), 500, "down");
    refreshed(updates, int(failures.size()));
    check(feed.usd(btc.asset) == 78200.0 && feed.oldest({btc.asset}) <= QDateTime::currentDateTimeUtc(),
          QStringLiteral("a clock ahead of ours does not make a price look newer than now"));

    // One source against what is known does not decide; two do.
    http.clear();
    http.answer(QStringLiteral("/defillama"), 200, coins({llama(btc.defillamaId, 7000.0, now)}));
    http.answer(QStringLiteral("/coingecko"), 500, "down");
    http.answer(QStringLiteral("/coinbase"), 500, "down");
    refreshed(updates, int(failures.size()));
    check(feed.usd(btc.asset) == 78200.0 && feed.inDoubt(btc.asset), QStringLiteral("a price far from the one held is doubted"));
    http.answer(QStringLiteral("/coingecko"), 200,
                QJsonDocument(QJsonObject{{btc.coingeckoId, QJsonObject{{QStringLiteral("usd"), 7100.0}, {QStringLiteral("last_updated_at"), now}}}})
                    .toJson(QJsonDocument::Compact));
    refreshed(updates, int(failures.size()));
    check(std::abs(feed.usd(btc.asset) - 7100.0) < 1e-9 && !feed.inDoubt(btc.asset), QStringLiteral("a second source that agrees settles it: a real crash"));

    // A first price that is wrong gives way the same way.
    net::PriceFeed wrong;
    wrong.useTestServer(http.base());
    http.clear();
    http.answer(QStringLiteral("/defillama"), 200, coins({llama(btc.defillamaId, 900000.0, now)}));
    http.answer(QStringLiteral("/coingecko"), 200,
                QJsonDocument(QJsonObject{{btc.coingeckoId, QJsonObject{{QStringLiteral("usd"), 78000.0}, {QStringLiteral("last_updated_at"), now}}}})
                    .toJson(QJsonDocument::Compact));
    http.answer(QStringLiteral("/coinbase"), 200,
                QJsonDocument(QJsonObject{{QStringLiteral("data"), QJsonObject{{QStringLiteral("rates"), QJsonObject{{btc.coinbaseSymbol, QString::number(1.0 / 78100.0, 'g', 12)}}}}}})
                    .toJson(QJsonDocument::Compact));
    wrong.refresh();
    waitFor([&wrong] { return !wrong.isBusy(); }, 15000);
    check(std::abs(wrong.usd(btc.asset) - 78100.0) < 1.0, QStringLiteral("a wrong first price gives way when two others agree: %1").arg(wrong.usd(btc.asset)));

    // A dollar coin that really left its dollar.
    net::PriceFeed depeg;
    depeg.useTestServer(http.base());
    http.clear();
    http.answer(QStringLiteral("/defillama"), 200, coins({llama(usdc().defillamaId, 0.45, now)}));
    http.answer(QStringLiteral("/coingecko"), 200,
                QJsonDocument(QJsonObject{{usdc().coingeckoId, QJsonObject{{QStringLiteral("usd"), 0.46}, {QStringLiteral("last_updated_at"), now}}}})
                    .toJson(QJsonDocument::Compact));
    http.answer(QStringLiteral("/coinbase"), 500, "down");
    depeg.refresh();
    waitFor([&depeg] { return !depeg.isBusy(); }, 15000);
    check(std::abs(depeg.usd(usdc().asset) - 0.46) < 1e-9, QStringLiteral("a dollar coin far from a dollar is believed when two sources say so"));

    // Three ways for a wrong first price to get in.
    const auto threeSources = [&http, &coins, &llama, &btc, now](double first) {
        http.clear();
        http.answer(QStringLiteral("/defillama"), 200, coins({llama(btc.defillamaId, first, now)}));
        http.answer(QStringLiteral("/coingecko"), 200,
                    QJsonDocument(QJsonObject{{btc.coingeckoId, QJsonObject{{QStringLiteral("usd"), 78000.0}, {QStringLiteral("last_updated_at"), now - 60}}}})
                        .toJson(QJsonDocument::Compact));
        http.answer(QStringLiteral("/coinbase"), 200,
                    QJsonDocument(QJsonObject{{QStringLiteral("data"), QJsonObject{{QStringLiteral("rates"), QJsonObject{{btc.coinbaseSymbol, QString::number(1.0 / 78100.0, 'g', 12)}}}}}})
                        .toJson(QJsonDocument::Compact));
    };
    {
        // Nowhere near ten times off: a later source still has to agree to confirm it.
        net::PriceFeed unshared;
        unshared.useTestServer(http.base());
        threeSources(300000.0);
        unshared.refresh();
        waitFor([&unshared] { return !unshared.isBusy(); }, 15000);
        check(std::abs(unshared.usd(btc.asset) - 78100.0) < 1.0, QStringLiteral("a first price the next sources do not share gives way: %1").arg(unshared.usd(btc.asset)));
    }
    {
        // An old price from another source is no confirmation.
        net::PriceFeed morning;
        morning.useTestServer(http.base());
        net::PriceQuote lastNight;
        lastNight.asset = btc.asset;
        lastNight.usd = 78500.0;
        lastNight.at = QDateTime::fromSecsSinceEpoch(now - 10 * 3600, QTimeZone::UTC);
        lastNight.source = QStringLiteral("Coinbase");
        morning.restore({lastNight});
        threeSources(900000.0);
        morning.refresh();
        waitFor([&morning] { return !morning.isBusy(); }, 15000);
        check(std::abs(morning.usd(btc.asset) - 78100.0) < 1.0, QStringLiteral("a price from last night confirms nothing: %1").arg(morning.usd(btc.asset)));
    }
    {
        // What was in doubt when it was saved is asked about again after a restart.
        net::PriceFeed restarted;
        restarted.useTestServer(http.base());
        net::PriceQuote saved;
        saved.asset = btc.asset;
        saved.usd = 900000.0;
        saved.at = QDateTime::fromSecsSinceEpoch(now - 600, QTimeZone::UTC);
        saved.source = QStringLiteral("DefiLlama");
        restarted.restore({saved});
        threeSources(900000.0);
        restarted.refresh();
        waitFor([&restarted] { return !restarted.isBusy(); }, 15000);
        check(std::abs(restarted.usd(btc.asset) - 78100.0) < 1.0, QStringLiteral("a wrong price saved a moment ago does not stand after a restart: %1").arg(restarted.usd(btc.asset)));
    }

    // A price saved under a clock that was ahead does not freeze what follows.
    net::PriceFeed ahead;
    ahead.useTestServer(http.base());
    net::PriceQuote future;
    future.asset = btc.asset;
    future.usd = 78000.0;
    future.at = QDateTime::fromSecsSinceEpoch(now + 86400, QTimeZone::UTC);
    future.source = QStringLiteral("Coinbase");
    ahead.restore({future});
    check(ahead.oldest({btc.asset}) <= QDateTime::currentDateTimeUtc(), QStringLiteral("a saved time ahead of the clock is taken as now"));
}

void testScale(const Check& check, QString* report)
{
    // The most the plan counts on: 500 wallets at the default gaps.
    constexpr int kRows = 425'000;
    QString why;
    const QString path = wallet::WalletStore::dataDir() + QStringLiteral("/ledger-scale.db");
    const QByteArray key = wallet::WalletStore::newDatabaseKey();
    QElapsedTimer clock;
    const qint64 memoryBefore = processMemoryMb();

    wallet::WalletModel model;
    clock.start();
    QVector<wallet::AddressRow> rows;
    rows.reserve(kRows);
    for (int i = 0; i < kRows; ++i) {
        const quint32 wallet = 1 + i / 850;
        const quint32 index = i % 850;
        if (i % 4 == 3) {
            wallet::AddressRow row = evmRow(wallet, index);
            if (i % 400 == 3)
                row.setBalance(usdt().asset, chain::Amount::fromU64(1000000));
            rows.append(row);
        } else {
            rows.append(utxoRow(wallet, index, i % 100 == 0 ? 1000 : 0, 0));
        }
    }
    const qint64 builtMs = clock.restart();
    {
        // As a first scan's flushes may leave them: not in the order they are read.
        QVector<wallet::AddressRow> shuffled = rows;
        std::mt19937 random(11);
        std::shuffle(shuffled.begin(), shuffled.end(), random);
        clock.restart();
        store::LedgerDb db;
        check(db.open(path, key, &why) == store::LedgerDb::Open::Ready && db.saveRows(shuffled, &why), QStringLiteral("425,000 rows are written: ") + why);
    }
    const qint64 savedMs = clock.restart();
    QSet<quint32> wallets;
    for (quint32 id = 1; id <= 501; ++id)
        wallets.insert(id);
    QVector<wallet::AddressRow> readBack;
    readBack.reserve(kRows);
    {
        store::LedgerDb db;
        db.open(path, key, &why);
        check(db.loadRows(wallets, 5000, [&readBack](QVector<wallet::AddressRow>&& chunk) { readBack += chunk; }, &why) && readBack.size() == kRows,
              QStringLiteral("and read back: ") + why);
    }
    const qint64 readMs = clock.restart();
    model.addSaved(readBack);
    const qint64 addedMs = clock.restart();
    const qint64 memoryAfter = processMemoryMb();
    chain::Amount btcExpected;
    chain::Amount usdtExpected;
    for (const wallet::AddressRow& row : std::as_const(rows)) {
        btcExpected += row.amount(bitcoin().coin);
        usdtExpected += row.amount(usdt().asset);
    }
    check(model.size() == kRows && model.total(bitcoin().coin) == btcExpected && model.total(usdt().asset) == usdtExpected,
          QStringLiteral("all of them reach the model, with the right totals"));
    clock.restart();
    wallet::AddressRow changed = model.at(100);
    changed.confirmed = 555;
    for (int i = 0; i < 1000; ++i) {
        changed.confirmed += 1;
        model.upsert(changed);
    }
    const qint64 changedMs = clock.elapsed();
    check(changedMs < 2000, QStringLiteral("a thousand balances changing at this size stay quick"));
    // Generous on a slow machine, and still far from what would feel like a wait.
    check(readMs + addedMs < 60000, QStringLiteral("a start at this size reads everything in under a minute"));
    *report += QStringLiteral("      425,000 rows: built %1 ms, written %2 ms, read %3 ms, into the model %4 ms, "
                              "1,000 changes %5 ms, memory about %6 MB\n")
                   .arg(builtMs).arg(savedMs).arg(readMs).arg(addedMs).arg(changedMs).arg(memoryAfter - memoryBefore);
    QFile::remove(path);
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));
}

} // namespace

void runFoundationTest(const Check& check, QString* report, const QList<wallet::AccountKey>& someKeys)
{
    const QString home = wallet::WalletStore::dataDir();
    wallet::WalletStore::setDataDir(home + QStringLiteral("/foundation"));
    testRegistry(check);
    testFileFormats(check, someKeys);
    testAmountsAndTotals(check);
    testLedgerDb(check);
    testLedgerAsync(check);
    testPrices(check);
    testScale(check, report);
    wallet::WalletStore::setDataDir(home);
}

} // namespace app
