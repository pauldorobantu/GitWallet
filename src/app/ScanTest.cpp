#include "chain/Address.h"
#include "net/Budget.h"
#include "net/ElectrumClient.h"
#include "net/EthRpc.h"
#include "store/Ledger.h"
#include "store/LedgerDb.h"
#include "wallet/EvmScanner.h"
#include "wallet/Registry.h"
#include "wallet/UtxoScanner.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <sqlcipher/sqlite3.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <memory>
#include <random>

// The scanners and their clients against servers on this PC that answer as a test tells
// them: no internet, and the answers no public server gives on request. A refusal, a lie,
// a chain of another name.
namespace app {

namespace {

using Check = std::function<void(bool, const QString&)>;

bool waitFor(const std::function<bool()>& done, int ms)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

void pause(int ms)
{
    waitFor([] { return false; }, ms);
}

QString txHash(int n)
{
    return QStringLiteral("%1").arg(n, 64, 16, QLatin1Char('0'));
}

// Bitcoin's first block: what a server of that chain answers to blockchain.block.header.
const char kGenesisHeader[] =
    "0100000000000000000000000000000000000000000000000000000000000000000000003ba3edfd7a7b12b27ac72c3e67768f617fc81bc388"
    "8a51323a9fb8aa4b1e5e4a29ab5f49ffff001d1dac2b7c";

// An Electrum server, as far as a wallet can tell.
class FakeElectrum : public QObject
{
public:
    FakeElectrum()
    {
        server_.listen(QHostAddress::LocalHost);
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* peer = server_.nextPendingConnection()) {
                ++connections;
                connect(peer, &QTcpSocket::disconnected, this, [this, peer] {
                    subscribed_.remove(peer);
                    buffers_.remove(peer);
                    peer->deleteLater();
                });
                connect(peer, &QTcpSocket::readyRead, this, [this, peer] { read(peer); });
            }
        });
    }

    QString line() const { return QStringLiteral("127.0.0.1:%1").arg(server_.serverPort()); }

    // One more transaction on the address.
    void fund(const QString& scripthash, qint64 sats)
    {
        Entry& entry = entries_[scripthash];
        entry.history.append(QJsonObject{{QStringLiteral("tx_hash"), txHash(++nextTx_)}, {QStringLiteral("height"), 800000}});
        entry.confirmed += sats;
    }
    void longHistory(const QString& scripthash, int transactions)
    {
        Entry& entry = entries_[scripthash];
        for (int i = 0; i < transactions; ++i)
            entry.history.append(QJsonObject{{QStringLiteral("tx_hash"), txHash(++nextTx_)}, {QStringLiteral("height"), 800000}});
    }
    QJsonValue status(const QString& scripthash) const
    {
        QString text;
        return wallet::UtxoScanner::statusOfHistory(entries_.value(scripthash).history, &text) == 1 ? QJsonValue(text) : QJsonValue();
    }
    // What a server says when something moved. `toAll`: whoever asked for it or not.
    void notify(const QString& scripthash, bool toAll = false)
    {
        const QJsonObject message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("method"), QStringLiteral("blockchain.scripthash.subscribe")},
                                  {QStringLiteral("params"), QJsonArray{scripthash, status(scripthash)}}};
        for (auto it = subscribed_.cbegin(); it != subscribed_.cend(); ++it) {
            if (toAll || it->contains(scripthash))
                send(it.key(), message);
        }
    }
    bool follows(const QString& scripthash) const
    {
        return std::any_of(subscribed_.cbegin(), subscribed_.cend(), [&scripthash](const QSet<QString>& set) { return set.contains(scripthash); });
    }

    // Every connection cut, and the next is answered as before.
    void dropConnections()
    {
        const QList<QTcpSocket*> peers = server_.findChildren<QTcpSocket*>();
        for (QTcpSocket* peer : peers)
            peer->abort();
    }

    // Gone: every connection cut, and nobody answers again.
    void closeAll()
    {
        const QList<QTcpSocket*> peers = server_.findChildren<QTcpSocket*>();
        for (QTcpSocket* peer : peers)
            peer->abort();
        server_.close();
    }

    QSet<QString> refuseSubscribeOf;
    QSet<QString> refuseReadOf;
    QSet<QString> refuseBalanceOf;
    bool refuseSubscribes = false;
    bool refuseReads = false;
    bool refuseBalances = false;
    bool hugeBalanceReply = false;
    int historyDelayMs = 0;
    int refusedReads = 0;
    QString header = QString::fromLatin1(kGenesisHeader);
    QHash<QString, int> calls;
    int connections = 0;

private:
    struct Entry {
        QJsonArray history;
        qint64 confirmed = 0;
    };

    void send(QTcpSocket* peer, const QJsonObject& message)
    {
        peer->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    }
    void answer(QTcpSocket* peer, const QJsonValue& id, const QJsonValue& result)
    {
        send(peer, QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {QStringLiteral("result"), result}});
    }
    void refuse(QTcpSocket* peer, const QJsonValue& id)
    {
        send(peer, QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                               {QStringLiteral("id"), id},
                               {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), 1}, {QStringLiteral("message"), QStringLiteral("refused")}}}});
    }
    void read(QTcpSocket* peer)
    {
        QByteArray& buffer = buffers_[peer];
        buffer += peer->readAll();
        qsizetype end = -1;
        while ((end = buffer.indexOf('\n')) >= 0) {
            const QJsonObject request = QJsonDocument::fromJson(buffer.left(end)).object();
            buffer.remove(0, end + 1);
            handle(peer, request);
        }
    }
    void handle(QTcpSocket* peer, const QJsonObject& request)
    {
        const QString method = request.value(QStringLiteral("method")).toString();
        const QJsonArray params = request.value(QStringLiteral("params")).toArray();
        const QJsonValue id = request.value(QStringLiteral("id"));
        const QString scripthash = params.at(0).toString();
        ++calls[method];
        if (method == QLatin1String("server.version")) {
            answer(peer, id, QJsonArray{QStringLiteral("Fake 1.0"), QStringLiteral("1.4.2")});
        } else if (method == QLatin1String("blockchain.block.header")) {
            answer(peer, id, header);
        } else if (method == QLatin1String("blockchain.headers.subscribe")) {
            answer(peer, id, QJsonObject{{QStringLiteral("height"), 800000}, {QStringLiteral("hex"), QStringLiteral("00")}});
        } else if (method == QLatin1String("server.ping")) {
            answer(peer, id, QJsonValue());
        } else if (method == QLatin1String("blockchain.scripthash.subscribe")) {
            if (refuseSubscribes || refuseSubscribeOf.contains(scripthash)) {
                refuse(peer, id);
            } else {
                subscribed_[peer].insert(scripthash);
                answer(peer, id, status(scripthash));
            }
        } else if (method == QLatin1String("blockchain.scripthash.unsubscribe")) {
            subscribed_[peer].remove(scripthash);
            answer(peer, id, true);
        } else if (method == QLatin1String("blockchain.scripthash.get_history")) {
            if (refuseReads || refuseReadOf.contains(scripthash)) {
                ++refusedReads;
                refuse(peer, id);
            } else if (historyDelayMs > 0) {
                QTimer::singleShot(historyDelayMs, peer, [this, peer, id, history = entries_.value(scripthash).history] { answer(peer, id, history); });
            } else {
                answer(peer, id, entries_.value(scripthash).history);
            }
        } else if (method == QLatin1String("blockchain.scripthash.get_balance")) {
            if (hugeBalanceReply) {
                send(peer, QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("result"), QString(2 * 1024 * 1024, QLatin1Char('x'))}});
            } else if (refuseBalances || refuseBalanceOf.contains(scripthash)) {
                refuse(peer, id);
            } else {
                answer(peer, id, QJsonObject{{QStringLiteral("confirmed"), entries_.value(scripthash).confirmed}, {QStringLiteral("unconfirmed"), 0}});
            }
        } else {
            refuse(peer, id);
        }
    }

    QHash<QTcpSocket*, QByteArray> buffers_;
    QHash<QTcpSocket*, QSet<QString>> subscribed_;
    QHash<QString, Entry> entries_;
    int nextTx_ = 0;
    QTcpServer server_; // goes first: its sockets say they are gone to what is above
};

// One scan against it, with what a controller would set up.
struct UtxoRun {
    wallet::WalletModel* model;
    wallet::UtxoScanner scanner;
    bool finished = false;
    int rounds = 0;
    QList<wallet::Problem> problems;

    UtxoRun(wallet::WalletModel* rows, const FakeElectrum& server, const wallet::AccountKey& key, int receiveGap, int changeGap)
        : UtxoRun(rows, QStringList{server.line()}, key, receiveGap, changeGap)
    {
    }
    UtxoRun(wallet::WalletModel* rows, const QStringList& servers, const wallet::AccountKey& key, int receiveGap, int changeGap)
        : model(rows), scanner(key.network, rows)
    {
        scanner.usePlainTcpForTest();
        wallet::ScanSettings settings;
        settings.utxoReceiveGap = receiveGap;
        settings.utxoChangeGap = changeGap;
        settings.servers.insert(wallet::networkInfo(key.network).id, servers);
        scanner.setAccounts({key});
        scanner.setSettings(settings);
        QObject::connect(&scanner, &wallet::ChainScanner::scanFinished, &scanner, [this] { finished = true; });
        QObject::connect(&scanner, &wallet::ChainScanner::passFinished, &scanner, [this] { ++rounds; });
        QObject::connect(&scanner, &wallet::ChainScanner::problem, &scanner,
                         [this](wallet::Problem kind, const QString&, const QString&) { problems.append(kind); });
        scanner.start();
    }
    qint64 total() const { return qint64(model->total(wallet::networkInfo(scanner.network()).coin).toDouble(0)); }
};

QString scripthashOf(const wallet::AccountKey& key, quint32 change, quint32 index)
{
    return QString::fromStdString(chain::addressFromChainNode(key.node().deriveChild(change), key.type, key.account, change, index,
                                                              wallet::networkInfo(key.network).format)
                                      .electrumScripthash);
}

wallet::AddressRow savedRow(const wallet::AccountKey& key, quint32 change, quint32 index)
{
    const chain::AddressInfo info = chain::addressFromChainNode(key.node().deriveChild(change), key.type, key.account, change, index,
                                                                wallet::networkInfo(key.network).format);
    wallet::AddressRow row;
    row.wallet = key.wallet;
    row.network = key.network;
    row.type = key.type;
    row.account = key.account;
    row.change = change;
    row.index = index;
    row.address = QString::fromStdString(info.address);
    row.scripthash = QString::fromStdString(info.electrumScripthash);
    row.checked = true;
    return row;
}

void testElectrumClient(const Check& check, const wallet::AccountKey& key)
{
    FakeElectrum server;
    net::ElectrumClient client;
    client.usePlainTcpForTest();
    net::ElectrumServer where;
    net::ElectrumServer::fromString(server.line(), &where);
    client.setServers({where});
    client.setGenesis(wallet::networkInfo(key.network).genesis);
    int lost = 0;
    QObject::connect(&client, &net::ElectrumClient::connectionLost, &client, [&lost] { ++lost; });
    client.start();
    check(waitFor([&client] { return client.isReady(); }, 10000), QStringLiteral("a client connects, and the first block is the one it expects"));
    check(server.calls.value(QStringLiteral("blockchain.block.header")) == 1, QStringLiteral("it asked"));

    const QString letGo = scripthashOf(key, 0, 0);
    bool answered = false;
    client.subscribeScripthash(letGo, [&answered](const QJsonValue&, const QString&) { answered = true; });
    check(waitFor([&answered] { return answered; }, 5000), QStringLiteral("it follows an address"));
    client.unsubscribeScripthash(letGo);
    check(waitFor([&server, &letGo] { return !server.follows(letGo); }, 5000), QStringLiteral("and lets it go"));
    server.notify(letGo, true);
    pause(300);
    check(client.isReady() && lost == 0 && server.connections == 1,
          QStringLiteral("what a server still says about an address let go is ignored, and the connection stays"));
    server.notify(scripthashOf(key, 0, 1), true);
    check(waitFor([&lost] { return lost > 0; }, 5000), QStringLiteral("one that was never asked for ends it"));
    client.stop();

    // A line too long for what is on its way.
    FakeElectrum big;
    net::ElectrumServer bigWhere;
    net::ElectrumServer::fromString(big.line(), &bigWhere);
    net::ElectrumClient bigClient;
    bigClient.usePlainTcpForTest();
    bigClient.setServers({bigWhere});
    int bigLost = 0;
    QObject::connect(&bigClient, &net::ElectrumClient::connectionLost, &bigClient, [&bigLost] { ++bigLost; });
    bigClient.start();
    waitFor([&bigClient] { return bigClient.isReady(); }, 10000);
    bool longAnswer = false;
    big.longHistory(letGo, 20000); // about 1.8 MB
    bigClient.request(QStringLiteral("blockchain.scripthash.get_history"), QJsonArray{letGo},
                      [&longAnswer](const QJsonValue& result, const QString&) { longAnswer = result.toArray().size() == 20000; });
    check(waitFor([&longAnswer] { return longAnswer; }, 20000) && bigLost == 0,
          QStringLiteral("a history longer than a megabyte comes through"));
    big.hugeBalanceReply = true;
    bigClient.request(QStringLiteral("blockchain.scripthash.get_balance"), QJsonArray{letGo}, [](const QJsonValue&, const QString&) {});
    check(waitFor([&bigLost] { return bigLost > 0; }, 10000), QStringLiteral("any other reply that long ends the connection"));
    bigClient.stop();
}

void testUtxoLive(const Check& check, const wallet::AccountKey& key)
{
    FakeElectrum server;
    server.fund(scripthashOf(key, 0, 2), 1000);
    wallet::WalletModel model;
    UtxoRun run(&model, server, key, 5, 5);
    check(waitFor([&run] { return run.finished; }, 15000), QStringLiteral("a small wallet scans"));
    check(model.size() == 13 && run.total() == 1000, QStringLiteral("to the end of its windows, with what it holds"));
    check(run.scanner.tiers().live == 13 && run.scanner.tiers().rounds == 0 && server.calls.value(QStringLiteral("blockchain.scripthash.get_history")) == 0,
          QStringLiteral("all of it followed live, nothing read"));
    check(run.scanner.answered() && !run.scanner.refreshFailing() && run.scanner.balancesAt().secsTo(QDateTime::currentDateTime()) < 5,
          QStringLiteral("and its figures stand for now"));
    server.fund(scripthashOf(key, 0, 7), 500);
    server.notify(scripthashOf(key, 0, 7));
    check(waitFor([&run] { return run.total() == 1500; }, 5000), QStringLiteral("a payment shows by notification"));
    check(waitFor([&model] { return model.size() == 18; }, 5000), QStringLiteral("and the window moves past it"));
}

void testUtxoRounds(const Check& check, const wallet::AccountKey& key)
{
    wallet::UtxoScanner::setLivePlacesForTest(10);
    wallet::UtxoScanner::setPacesForTest(300, 200, 100);
    FakeElectrum server;
    server.fund(scripthashOf(key, 0, 0), 1000);
    wallet::WalletModel model;
    UtxoRun run(&model, server, key, 20, 5);
    check(waitFor([&run] { return run.finished; }, 20000), QStringLiteral("a wallet past its live places scans"));
    check(model.size() == 26 && run.total() == 1000 && model.checkedCount() == 26, QStringLiteral("whole, with what it holds"));
    const wallet::ScanTiers tiers = run.scanner.tiers();
    check(tiers.live <= 10 && tiers.rounds > 0 && tiers.live + tiers.rounds == 26,
          QStringLiteral("each row either followed or read, none twice: %1 + %2").arg(tiers.live).arg(tiers.rounds));
    check(server.follows(scripthashOf(key, 0, 1)) && !server.follows(scripthashOf(key, 0, 15)),
          QStringLiteral("the next unused addresses are the ones followed"));

    // A cold address, and no server to say so: the next round finds it.
    const QDateTime before = QDateTime::currentDateTime();
    server.fund(scripthashOf(key, 0, 15), 700);
    check(waitFor([&run] { return run.total() == 1700; }, 15000), QStringLiteral("what moves on a row read in rounds shows by the next round"));
    check(waitFor([&model] { return model.size() == 41; }, 15000), QStringLiteral("and moves the window"));
    check(waitFor([&run, &before] { return run.scanner.balancesAt().isValid() && run.scanner.balancesAt() >= before; }, 15000),
          QStringLiteral("a whole round dates the figures from when it began"));
    check(!run.scanner.refreshFailing(), QStringLiteral("nothing failed"));

    // The next address is paid: those after it take over.
    check(waitFor([&server, &key] { return server.follows(scripthashOf(key, 0, 16)); }, 15000), QStringLiteral("the address after the one paid is followed"));
    server.fund(scripthashOf(key, 0, 16), 300);
    server.notify(scripthashOf(key, 0, 16));
    check(waitFor([&run] { return run.total() == 2000; }, 5000), QStringLiteral("a payment to it shows by notification"));
    check(waitFor([&server, &key] { return server.follows(scripthashOf(key, 0, 17)) && !server.follows(scripthashOf(key, 0, 15 - 14)); }, 5000),
          QStringLiteral("the next ones take the places of those passed"));
    check(run.scanner.tiers().live <= 10, QStringLiteral("within the places there are"));
    run.scanner.stop();

    wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
}

void testUtxoRefusals(const Check& check, const wallet::AccountKey& key)
{
    wallet::UtxoScanner::setPacesForTest(300, 200, 100);
    {
        // One address it will not follow.
        FakeElectrum server;
        server.fund(scripthashOf(key, 0, 1), 900);
        server.refuseSubscribeOf.insert(scripthashOf(key, 0, 1));
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 5, 5);
        check(waitFor([&run] { return run.finished; }, 15000), QStringLiteral("a scan ends though a server refuses to follow an address"));
        check(run.total() == 900 && model.size() == 12 && model.checkedCount() == 12, QStringLiteral("and that address is read instead"));
        check(run.problems.contains(wallet::Problem::Failure), QStringLiteral("it says so"));
        check(run.scanner.tiers().rounds == 1, QStringLiteral("and goes on reading it"));
        run.scanner.stop();
    }
    {
        // None at all: past a few it stops asking.
        FakeElectrum server;
        server.fund(scripthashOf(key, 0, 3), 400);
        server.refuseSubscribes = true;
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 300, 5);
        check(waitFor([&run] { return run.finished; }, 20000), QStringLiteral("a scan ends though a server follows nothing"));
        check(run.total() == 400 && model.checkedCount() == model.size(), QStringLiteral("everything read"));
        check(server.calls.value(QStringLiteral("blockchain.scripthash.subscribe")) < model.size(), QStringLiteral("and it stopped asking"));
        run.scanner.stop();
    }
    {
        // Reads refused for a while.
        wallet::UtxoScanner::setLivePlacesForTest(10);
        FakeElectrum server;
        server.fund(scripthashOf(key, 0, 0), 1000);
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 20, 5);
        check(waitFor([&run] { return run.finished; }, 20000), QStringLiteral("a wallet read in rounds scans"));
        const QDateTime asOf = run.scanner.balancesAt();
        const int readsBefore = server.calls.value(QStringLiteral("blockchain.scripthash.get_history"));
        server.refuseReads = true;
        server.refuseBalances = true;
        check(waitFor([&run] { return run.scanner.refreshFailing(); }, 15000), QStringLiteral("a refused round read shows as failing"));
        check(run.scanner.balancesAt() == asOf, QStringLiteral("and the figures keep the time they are from"));
        check(run.total() == 1000, QStringLiteral("and what they said"));
        server.refuseReads = false;
        server.refuseBalances = false;
        check(waitFor([&run, &asOf] { return !run.scanner.refreshFailing() && run.scanner.balancesAt() > asOf; }, 30000),
              QStringLiteral("once the server answers again a whole round clears it"));
        check(server.calls.value(QStringLiteral("blockchain.scripthash.get_history")) > readsBefore, QStringLiteral("by reading"));
        run.scanner.stop();
        check(!run.scanner.refreshFailing(), QStringLiteral("and a scan that stopped has nothing to say"));
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
    }
    {
        // It will not read an address either, and says what it holds.
        wallet::UtxoScanner::setLivePlacesForTest(5);
        FakeElectrum server;
        server.refuseSubscribes = true;
        server.refuseReads = true;
        for (int i = 0; i < 8; ++i)
            server.fund(scripthashOf(key, 0, quint32(i)), 100);
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 5, 1);
        check(waitFor([&run] { return run.total() >= 500; }, 20000), QStringLiteral("what an address holds comes through though it can be neither followed nor read"));
        run.scanner.stop();
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
    }
    wallet::UtxoScanner::setPacesForTest(5 * 60 * 1000, 5000, 30 * 1000);
}

// What a server does to a wallet that has already answered, and leaving it.
void testUtxoServers(const Check& check, const wallet::AccountKey& key)
{
    wallet::UtxoScanner::setPacesForTest(300, 200, 100);
    const QString readCalls = QStringLiteral("blockchain.scripthash.get_history");
    wallet::UtxoScanner::setLivePlacesForTest(10);
    {
        // It stops reading, and goes on answering everything else.
        FakeElectrum server;
        server.fund(scripthashOf(key, 0, 0), 1000);
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 20, 5);
        check(waitFor([&run] { return run.finished; }, 20000), QStringLiteral("a wallet read in rounds scans"));
        const QDateTime asOf = run.scanner.balancesAt();
        server.refuseReads = true;
        check(waitFor([&server] { return server.refusedReads >= 8; }, 30000), QStringLiteral("a server stops reading"));
        pause(100);
        check(run.scanner.refreshFailing(), QStringLiteral("and that shows as failing, though every balance answers"));
        check(run.scanner.balancesAt() == asOf, QStringLiteral("and the figures keep the time they are from"));
        const int reads = server.calls.value(readCalls);
        server.refuseReads = false;
        check(waitFor([&run, &asOf] { return !run.scanner.refreshFailing() && run.scanner.balancesAt() > asOf; }, 40000),
              QStringLiteral("it reads again: a whole round clears it"));
        check(server.calls.value(readCalls) > reads, QStringLiteral("by reading"));
        run.scanner.stop();
    }
    {
        // One address it will say nothing of: the round ends without an answer for it.
        FakeElectrum server;
        const QString silent = scripthashOf(key, 0, 15);
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 20, 5);
        waitFor([&run] { return run.finished; }, 20000);
        const QDateTime asOf = run.scanner.balancesAt();
        server.refuseReadOf.insert(silent);
        server.refuseBalanceOf.insert(silent);
        const int before = run.rounds;
        check(waitFor([&run, before] { return run.rounds > before; }, 20000), QStringLiteral("a round ends though an address gives no answer"));
        check(run.scanner.refreshFailing(), QStringLiteral("and says so when it ends"));
        check(run.scanner.balancesAt() == asOf, QStringLiteral("and does not date the figures"));
        server.refuseReadOf.clear();
        server.refuseBalanceOf.clear();
        check(waitFor([&run, &asOf] { return !run.scanner.refreshFailing() && run.scanner.balancesAt() > asOf; }, 40000),
              QStringLiteral("the next whole round does"));
        run.scanner.stop();
    }
    {
        // One address with a history too long to give, and it holds what its balance says.
        FakeElectrum server;
        const QString longOne = scripthashOf(key, 0, 15);
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 20, 5);
        waitFor([&run] { return run.finished; }, 20000);
        const QDateTime asOf = run.scanner.balancesAt();
        server.refuseReadOf.insert(longOne);
        const int before = run.rounds;
        check(waitFor([&run, before] { return run.rounds > before + 1; }, 30000), QStringLiteral("rounds go on with one address refused"));
        check(!run.scanner.refreshFailing() && run.scanner.balancesAt() > asOf, QStringLiteral("and its balance makes up for it"));
        run.scanner.stop();
    }
    {
        // A round's date.
        FakeElectrum server;
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 20, 5);
        waitFor([&run] { return run.finished; }, 20000);
        const QDateTime before = run.scanner.balancesAt();
        server.historyDelayMs = 400;
        check(waitFor([&run, &before] { return run.scanner.balancesAt() != before; }, 20000), QStringLiteral("a round ends"));
        const qint64 age = run.scanner.balancesAt().msecsTo(QDateTime::currentDateTime());
        check(age >= 300, QStringLiteral("and dates the figures from when it began, not when it ended: %1 ms ago").arg(age));
        run.scanner.stop();
    }
    {
        // Refresh, with the next round a long way off.
        wallet::UtxoScanner::setPacesForTest(60000, 200, 100);
        FakeElectrum server;
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 20, 5);
        waitFor([&run] { return run.finished; }, 20000);
        const int reads = server.calls.value(readCalls);
        pause(1000);
        check(server.calls.value(readCalls) == reads, QStringLiteral("the next round waits its turn"));
        run.scanner.refreshNow();
        check(waitFor([&server, &readCalls, reads] { return server.calls.value(readCalls) > reads; }, 5000), QStringLiteral("Refresh starts it now"));
        run.scanner.stop();
        wallet::UtxoScanner::setPacesForTest(300, 200, 100);
    }
    wallet::UtxoScanner::setLivePlacesForTest(12);
    {
        // A wallet that outgrows the places while it runs.
        FakeElectrum server;
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 5, 5);
        check(waitFor([&run] { return run.finished; }, 20000) && run.scanner.tiers().rounds == 0, QStringLiteral("a wallet that fits is followed whole"));
        server.fund(scripthashOf(key, 0, 4), 100);
        server.notify(scripthashOf(key, 0, 4));
        check(waitFor([&server, &key] { return server.follows(scripthashOf(key, 0, 9)) && !server.follows(scripthashOf(key, 0, 1)); }, 15000),
              QStringLiteral("when it no longer fits, the next addresses take the places of those behind"));
        check(run.scanner.tiers().live <= 12, QStringLiteral("within the places there are"));
        run.scanner.stop();
    }
    wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
    {
        // Another server: the same history, and what it holds asked again. And what the last
        // one refused to follow is followed if this one will.
        FakeElectrum first;
        FakeElectrum second;
        const QString paid = scripthashOf(key, 0, 2);
        const QString refused = scripthashOf(key, 0, 1);
        first.fund(paid, 5);
        second.fund(paid, 1000);
        first.refuseSubscribeOf.insert(refused);
        wallet::WalletModel model;
        UtxoRun run(&model, QStringList{first.line(), second.line()}, key, 5, 5);
        check(waitFor([&run] { return run.finished; }, 20000) && run.total() == 5, QStringLiteral("a server says what an address holds"));
        check(!first.follows(refused), QStringLiteral("and will not follow another"));
        first.closeAll();
        check(waitFor([&run] { return run.total() == 1000; }, 30000), QStringLiteral("after a failover the next one is asked what it holds, though the history is the same"));
        check(waitFor([&second, &refused] { return second.follows(refused); }, 10000), QStringLiteral("and follows what the last one refused"));
        run.scanner.stop();
    }
    {
        // A server that does nothing it is asked is left.
        FakeElectrum bad;
        FakeElectrum good;
        bad.refuseSubscribes = true;
        bad.refuseReads = true;
        bad.refuseBalances = true;
        good.fund(scripthashOf(key, 0, 3), 400);
        wallet::WalletModel model;
        UtxoRun run(&model, QStringList{bad.line(), good.line()}, key, 40, 5);
        check(waitFor([&run] { return run.finished; }, 30000) && run.total() == 400, QStringLiteral("a server that refuses everything is left for the next"));
        check(good.connections == 1, QStringLiteral("once"));
        run.scanner.stop();
    }
    {
        // An address no server will say anything of, with nothing on it: the scan ends, and the row says so.
        FakeElectrum server;
        const QString emptied = scripthashOf(key, 0, 1);
        server.longHistory(emptied, 3);
        server.refuseSubscribeOf.insert(emptied);
        server.refuseReadOf.insert(emptied);
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 5, 5);
        check(waitFor([&run] { return run.finished; }, 30000), QStringLiteral("a scan ends though a server will say nothing of an emptied address"));
        check(model.checkedCount() == model.size() - 1, QStringLiteral("and its row stays unchecked: %1 of %2").arg(model.checkedCount()).arg(model.size()));
        run.scanner.stop();
    }
    {
        // It answers after all: the row has its answer, and the scan says so.
        FakeElectrum server;
        const QString quiet = scripthashOf(key, 0, 1);
        server.refuseSubscribeOf.insert(quiet);
        server.refuseReadOf.insert(quiet);
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 5, 5);
        check(waitFor([&run] { return run.finished; }, 30000) && model.checkedCount() == model.size() - 1, QStringLiteral("a row without an answer stays unchecked"));
        server.refuseReadOf.clear();
        check(waitFor([&model] { return model.checkedCount() == model.size(); }, 15000), QStringLiteral("until a read answers, also with nothing to say"));
        run.scanner.stop();
    }
    {
        // Every server refuses the same address: each is asked once, and the client stays.
        FakeElectrum first;
        FakeElectrum second;
        const QString emptied = scripthashOf(key, 0, 1);
        for (FakeElectrum* server : {&first, &second}) {
            server->refuseSubscribeOf.insert(emptied);
            server->refuseReadOf.insert(emptied);
        }
        wallet::WalletModel model;
        UtxoRun run(&model, QStringList{first.line(), second.line()}, key, 5, 5);
        check(waitFor([&run] { return run.finished; }, 30000), QStringLiteral("a scan ends though every server refuses an address"));
        pause(4000);
        check(first.connections + second.connections <= 2, QStringLiteral("and the client does not hop for it: %1 connections").arg(first.connections + second.connections));
        run.scanner.stop();
    }
    {
        // An address it will neither follow nor read, holding coins, with the retry sooner
        // than the pause, as it really is: it must not hold up every other read.
        wallet::UtxoScanner::setLivePlacesForTest(10);
        wallet::UtxoScanner::setPacesForTest(300, 100, 1000);
        FakeElectrum server;
        const QString stuck = scripthashOf(key, 0, 3);
        server.fund(stuck, 500);
        server.refuseSubscribeOf.insert(stuck);
        server.refuseReadOf.insert(stuck);
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 20, 5);
        check(waitFor([&run] { return run.finished; }, 30000) && run.total() == 500, QStringLiteral("what an address holds counts though it can be neither followed nor read"));
        const int reads = server.calls.value(readCalls);
        check(waitFor([&server, &readCalls, reads] { return server.calls.value(readCalls) > reads + 10; }, 15000), QStringLiteral("and the others go on being read"));
        check(!run.scanner.refreshFailing(), QStringLiteral("and nothing says failing for one address"));
        run.scanner.stop();
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
        wallet::UtxoScanner::setPacesForTest(300, 200, 100);
    }
    {
        // With another server to ask, that one is.
        FakeElectrum first;
        FakeElectrum second;
        const QString emptied = scripthashOf(key, 0, 1);
        first.longHistory(emptied, 3);
        second.longHistory(emptied, 3);
        first.refuseSubscribeOf.insert(emptied);
        first.refuseReadOf.insert(emptied);
        wallet::WalletModel model;
        UtxoRun run(&model, QStringList{first.line(), second.line()}, key, 5, 5);
        check(waitFor([&run] { return run.finished; }, 30000) && model.checkedCount() == model.size() && second.connections == 1,
              QStringLiteral("with another server to ask, the row has its answer there"));
        run.scanner.stop();
    }
    wallet::UtxoScanner::setPacesForTest(5 * 60 * 1000, 5000, 30 * 1000);
}

void testUtxoSaved(const Check& check, const wallet::AccountKey& key)
{
    {
        // What a server said is not taken for what another says.
        FakeElectrum server;
        const QString paid = scripthashOf(key, 0, 2);
        server.fund(paid, 1000);
        wallet::AddressRow lied = savedRow(key, 0, 2);
        lied.used = true;
        lied.confirmed = 5;
        lied.status = server.status(paid).toString();
        wallet::WalletModel model;
        model.addSaved({lied});
        UtxoRun run(&model, server, key, 5, 5);
        check(waitFor([&run] { return run.finished; }, 15000) && run.total() == 1000,
              QStringLiteral("a saved balance is read again though its history is the same"));
        run.scanner.stop();
    }
    {
        // Another chain's server: the figures stay, and it is said.
        FakeElectrum server;
        server.header = QStringLiteral("00").repeated(80);
        wallet::AddressRow held = savedRow(key, 0, 0);
        held.used = true;
        held.confirmed = 1234;
        wallet::WalletModel model;
        model.addSaved({held});
        UtxoRun run(&model, server, key, 5, 5);
        check(waitFor([&run] { return run.scanner.wrongChain(); }, 15000), QStringLiteral("a server on another chain is found out"));
        check(run.total() == 1234 && run.problems.contains(wallet::Problem::WrongChain), QStringLiteral("it clears nothing, and it is said"));
        check(!run.scanner.connecting(), QStringLiteral("and that is the reason shown, not that it is connecting"));
        check(server.calls.value(QStringLiteral("blockchain.scripthash.subscribe")) == 0, QStringLiteral("it is asked about no address"));
        run.scanner.stop();
    }
    {
        // What an earlier server left past the windows.
        FakeElectrum server;
        QVector<wallet::AddressRow> rows;
        for (quint32 index = 0; index <= 50; ++index) {
            wallet::AddressRow row = savedRow(key, 0, index);
            if (index == 40)
                row.used = true;
            if (index == 45) {
                row.used = true;
                row.confirmed = 777;
            }
            if (index == 30)
                row.checked = false;
            rows.append(row);
        }
        wallet::WalletModel model;
        model.addSaved(rows);
        int dropped = 0;
        QObject::connect(&model, &wallet::WalletModel::rowsDropped, &model, [&dropped] { ++dropped; });
        {
            UtxoRun run(&model, server, key, 5, 5);
            check(waitFor([&run] { return run.finished; }, 15000), QStringLiteral("a scan over rows an earlier server left ends"));
            check(run.total() == 0, QStringLiteral("what it said they hold is gone"));
            check(dropped == 1 && model.find(key.wallet, key.network, key.type, key.account, 0, 30) < 0 && model.find(key.wallet, key.network, key.type, key.account, 0, 40) < 0,
                  QStringLiteral("rows past the windows go"));
            check(model.checkedCount() == model.size(), QStringLiteral("and every row left has its answer"));
            run.scanner.stop();
        }
        UtxoRun again(&model, server, key, 5, 5);
        check(waitFor([&again] { return again.finished; }, 15000) && model.size() == 10, QStringLiteral("by the next start only the windows are left"));
        again.scanner.stop();
    }
    {
        // A far address that really holds coins, and the database behind it all.
        const QString path = wallet::WalletStore::dataDir() + QStringLiteral("/scan-trim.db");
        const QByteArray secret = wallet::WalletStore::newDatabaseKey();
        FakeElectrum server;
        server.fund(scripthashOf(key, 0, 45), 777);
        wallet::AddressRow far = savedRow(key, 0, 60);
        far.used = true;
        wallet::AddressRow funded = savedRow(key, 0, 45);
        funded.used = true;
        funded.confirmed = 777;
        QString why;
        {
            store::LedgerDb db;
            check(db.open(path, secret, &why) == store::LedgerDb::Open::Ready && db.saveRows({far, funded}, &why), QStringLiteral("rows are in the file: ") + why);
        }
        wallet::WalletModel model;
        int rowCount = -1;
        {
            store::Ledger ledger(&model);
            bool loaded = false;
            QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
            ledger.open(path, secret, {key.wallet});
            waitFor([&loaded] { return loaded; }, 10000);
            UtxoRun run(&model, server, key, 5, 5);
            check(waitFor([&run] { return run.finished; }, 15000), QStringLiteral("a scan with the database behind it ends"));
            check(run.total() == 777 && model.find(key.wallet, key.network, key.type, key.account, 0, 45) >= 0 && model.size() == 51 + 5,
                  QStringLiteral("what really holds coins far out stays, and moves the window: %1 rows").arg(model.size()));
            check(model.find(key.wallet, key.network, key.type, key.account, 0, 60) < 0, QStringLiteral("what lies past it goes"));
            run.scanner.stop();
            rowCount = model.size();
            ledger.close();
        }
        {
            store::LedgerDb db;
            check(db.open(path, secret, &why) == store::LedgerDb::Open::Ready && db.rowCount() == rowCount,
                  QStringLiteral("and from the file too: %1 rows").arg(db.rowCount()));
        }
        QFile::remove(path);
        QFile::remove(path + QStringLiteral("-wal"));
        QFile::remove(path + QStringLiteral("-shm"));
    }
}

QByteArray word(quint64 value)
{
    QByteArray out(32, '\0');
    for (int i = 0; i < 8; ++i)
        out[31 - i] = char((value >> (8 * i)) & 0xFF);
    return out;
}

// An Ethereum-style endpoint: nonces, and Multicall3 as far as a scan asks it.
class FakeRpc : public QObject
{
public:
    FakeRpc()
    {
        server_.listen(QHostAddress::LocalHost);
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* peer = server_.nextPendingConnection()) {
                connect(peer, &QTcpSocket::disconnected, peer, &QObject::deleteLater);
                auto buffer = std::make_shared<QByteArray>();
                connect(peer, &QTcpSocket::readyRead, peer, [this, peer, buffer] {
                    *buffer += peer->readAll();
                    const qsizetype head = buffer->indexOf("\r\n\r\n");
                    if (head < 0)
                        return;
                    const qsizetype at = buffer->toLower().indexOf("content-length:");
                    const int length = at < 0 ? 0 : buffer->mid(at + 15, buffer->indexOf("\r\n", at) - at - 15).trimmed().toInt();
                    if (buffer->size() < head + 4 + length)
                        return;
                    respond(peer, buffer->mid(head + 4, length));
                    buffer->clear();
                });
            }
        });
    }

    QString url() const { return QStringLiteral("http://127.0.0.1:%1/").arg(server_.serverPort()); }

    QHash<QString, quint64> nonces; // by lowercase address
    QHash<QString, quint64> ether;
    bool nullNonces = false;     // a result that is no nonce
    bool refuseNonces = false;   // HTTP 429 for a request that asks for one
    bool failSubCalls = false;   // success=false for every balance
    quint64 chainId = 1;
    int maxBatch = 0;            // a larger batch gets an error object, as a capped endpoint answers
    int delayMs = 0;
    int requests = 0;
    int nonceCalls = 0;
    int ethCalls = 0;

private:
    void respond(QTcpSocket* peer, const QByteArray& body)
    {
        if (delayMs > 0)
            QTimer::singleShot(delayMs, peer, [this, peer, body] { respondNow(peer, body); });
        else
            respondNow(peer, body);
    }
    void respondNow(QTcpSocket* peer, const QByteArray& body)
    {
        ++requests;
        const QJsonArray calls = QJsonDocument::fromJson(body).array();
        if (maxBatch > 0 && calls.size() > maxBatch) {
            const QByteArray refusal = R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"batch too large"}})";
            peer->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(refusal.size()) + "\r\n\r\n" + refusal);
            peer->flush();
            peer->disconnectFromHost();
            return;
        }
        QJsonArray replies;
        bool asksNonce = false;
        for (const QJsonValue& value : calls) {
            const QJsonObject call = value.toObject();
            const QString method = call.value(QStringLiteral("method")).toString();
            QJsonObject reply{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), call.value(QStringLiteral("id"))}};
            if (method == QLatin1String("eth_chainId")) {
                reply.insert(QStringLiteral("result"), QStringLiteral("0x%1").arg(chainId, 0, 16));
            } else if (method == QLatin1String("eth_getTransactionCount")) {
                asksNonce = true;
                ++nonceCalls;
                const QString address = call.value(QStringLiteral("params")).toArray().at(0).toString().toLower();
                reply.insert(QStringLiteral("result"), nullNonces ? QJsonValue() : QJsonValue(QStringLiteral("0x%1").arg(nonces.value(address), 0, 16)));
            } else if (method == QLatin1String("eth_call")) {
                ++ethCalls;
                reply.insert(QStringLiteral("result"), aggregate(call.value(QStringLiteral("params")).toArray().at(0).toObject().value(QStringLiteral("data")).toString()));
            }
            replies.append(reply);
        }
        const QByteArray out = asksNonce && refuseNonces ? QByteArray("{}") : QJsonDocument(replies).toJson(QJsonDocument::Compact);
        const QByteArray status = asksNonce && refuseNonces ? "429 Slow Down" : "200 OK";
        peer->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                    + QByteArray::number(out.size()) + "\r\n\r\n" + out);
        peer->flush();
        peer->disconnectFromHost();
    }

    // aggregate3((address,bool,bytes)[]) in, (bool,bytes)[] out.
    QString aggregate(const QString& data) const
    {
        const QByteArray raw = QByteArray::fromHex(data.mid(2).toLatin1()).mid(4);
        const auto wordAt = [&raw](qsizetype at) {
            quint64 value = 0;
            for (int i = 24; i < 32; ++i)
                value = (value << 8) | quint8(raw.at(at + i));
            return qsizetype(value);
        };
        const qsizetype array = wordAt(0);
        const qsizetype count = wordAt(array);
        const qsizetype heads = array + 32;
        QByteArray tails;
        QList<qsizetype> offsets;
        for (qsizetype i = 0; i < count; ++i) {
            const qsizetype tuple = heads + wordAt(heads + 32 * i);
            const qsizetype bytes = tuple + wordAt(tuple + 64);
            const qsizetype length = wordAt(bytes);
            const QByteArray call = raw.mid(bytes + 32, length);
            const QByteArray selector = call.left(4).toHex();
            const QString address = QStringLiteral("0x") + QString::fromLatin1(call.mid(16, 20).toHex());
            quint64 value = 0;
            bool success = true;
            if (selector == "3408e470")
                value = chainId;
            else if (selector == "4d2301cc")
                value = ether.value(address);
            if (failSubCalls && selector != "3408e470")
                success = false;
            offsets.append(tails.size());
            tails += word(success ? 1 : 0) + word(64) + word(success ? 32 : 0);
            if (success)
                tails += word(value);
        }
        QByteArray out = word(32) + word(quint64(count));
        for (const qsizetype offset : offsets)
            out += word(quint64(count * 32 + offset));
        return QStringLiteral("0x") + QString::fromLatin1((out + tails).toHex());
    }

    QTcpServer server_;
};

struct EvmRun {
    wallet::WalletModel* model;
    wallet::EvmScanner scanner;
    bool finished = false;
    int passes = 0;

    struct Options {
        QDateTime balancesAt;
        QDateTime noncesAt;
        wallet::NonceSweep sweep;
        net::BudgetPool* budgets = nullptr;
        bool endpointsChanged = false;
        int refreshSeconds = 3600;
    };

    EvmRun(wallet::WalletModel* rows, const QStringList& urls, const wallet::AccountKey& key, int gap, const Options& options = {})
        : model(rows), scanner(wallet::findNetwork(QStringLiteral("ethereum"))->network, options.budgets, rows)
    {
        scanner.allowLocalHttpForTest();
        wallet::ScanSettings settings;
        settings.evmGap = gap;
        settings.evmRefreshSeconds = options.refreshSeconds;
        if (options.endpointsChanged)
            scanner.setSettings(settings);
        settings.servers.insert(QStringLiteral("ethereum"), urls);
        scanner.setAccounts({key});
        scanner.setSettings(settings);
        scanner.restoreTimes(options.balancesAt, options.noncesAt, options.sweep);
        QObject::connect(&scanner, &wallet::ChainScanner::scanFinished, &scanner, [this] { finished = true; });
        QObject::connect(&scanner, &wallet::ChainScanner::passFinished, &scanner, [this] { ++passes; });
        scanner.start();
    }
};

QString evmAddress(const wallet::AccountKey& key, quint32 index)
{
    return QString::fromStdString(chain::addressFromChainNode(key.node().deriveChild(0), key.type, key.account, 0, index,
                                                              wallet::networkInfo(wallet::findNetwork(QStringLiteral("ethereum"))->network).format)
                                      .address)
        .toLower();
}

void testEvm(const Check& check, const wallet::AccountKey& key, QString* report)
{
    const wallet::NetworkInfo& ethereum = *wallet::findNetwork(QStringLiteral("ethereum"));
    {
        FakeRpc rpc;
        rpc.nonces.insert(evmAddress(key, 2), 1);
        rpc.ether.insert(evmAddress(key, 3), 7);
        wallet::WalletModel model;
        EvmRun run(&model, {rpc.url()}, key, 5);
        check(waitFor([&run] { return run.finished; }, 15000), QStringLiteral("an Ethereum account scans"));
        check(model.size() == 9 && model.checkedCount() == 9 && model.total(ethereum.coin) == chain::Amount::fromU64(7),
              QStringLiteral("to the end of its window, with what it holds"));
        check(run.scanner.answered() && !run.scanner.refreshFailing() && run.scanner.balancesAt().isValid() && run.scanner.noncesAt().isValid(),
              QStringLiteral("and its times are set"));

        // An endpoint that said every address was used, and then the truth.
        rpc.nonces.clear();
        rpc.ether.clear();
        for (quint32 i = 0; i < 30; ++i)
            rpc.nonces.insert(evmAddress(key, i), 1);
        run.scanner.stop();
        {
            EvmRun inflated(&model, {rpc.url()}, key, 5);
            check(waitFor([&inflated] { return inflated.finished; }, 15000) && model.size() == 35, QStringLiteral("an endpoint can call addresses used"));
            inflated.scanner.stop();
        }
        rpc.nonces.clear();
        int dropped = 0;
        QObject::connect(&model, &wallet::WalletModel::rowsDropped, &model, [&dropped] { ++dropped; });
        EvmRun honest(&model, {rpc.url()}, key, 5);
        check(waitFor([&honest] { return honest.finished; }, 15000), QStringLiteral("an honest one reads them all"));
        check(dropped == 1 && model.size() == 5 && model.usedCount() == 0,
              QStringLiteral("and a nonce that fell shows what was false: %1 rows, %2 used").arg(model.size()).arg(model.usedCount()));
        honest.scanner.stop();
    }
    {
        // Addresses that held a balance and gave it up by signature: nonce 0, nothing left.
        FakeRpc rpc;
        for (quint32 i = 0; i < 30; ++i)
            rpc.ether.insert(evmAddress(key, i), 1);
        wallet::WalletModel model;
        {
            EvmRun held(&model, {rpc.url()}, key, 5);
            check(waitFor([&held] { return held.finished; }, 15000) && model.size() == 35, QStringLiteral("addresses hold something"));
            held.scanner.stop();
        }
        rpc.ether.clear();
        {
            EvmRun swept(&model, {rpc.url()}, key, 5);
            check(waitFor([&swept] { return swept.finished; }, 15000), QStringLiteral("they are swept, and read again"));
            check(model.size() == 35 && model.usedCount() == 30,
                  QStringLiteral("they stay used, so the window still reaches the next one to be paid: %1 rows, %2 used").arg(model.size()).arg(model.usedCount()));
            check(swept.scanner.noncesAt().isValid(), QStringLiteral("though every nonce was read"));
            swept.scanner.stop();
        }
        // It may have been an endpoint's word all along: another list starts over.
        FakeRpc other;
        EvmRun::Options changed;
        changed.endpointsChanged = true;
        int limits = 0;
        EvmRun fresh(&model, {other.url()}, key, 5, changed);
        QObject::connect(&fresh.scanner, &wallet::ChainScanner::problem, &fresh.scanner, [&limits](wallet::Problem kind, const QString&, const QString&) {
            limits += kind == wallet::Problem::Limit ? 1 : 0;
        });
        check(waitFor([&fresh] { return fresh.finished; }, 15000), QStringLiteral("other endpoints read them all"));
        check(model.size() == 5 && model.usedCount() == 0 && limits == 0,
              QStringLiteral("and only what they answer counts: %1 rows, %2 used").arg(model.size()).arg(model.usedCount()));
        fresh.scanner.stop();
    }
    {
        // A sweep cut short goes on where it stopped.
        FakeRpc rpc;
        wallet::WalletModel model;
        {
            EvmRun first(&model, {rpc.url()}, key, 10050);
            waitFor([&first] { return first.finished; }, 60000);
            first.scanner.stop();
        }
        const int asked = rpc.nonceCalls;
        const QDateTime old = QDateTime::currentDateTime().addSecs(-3 * 3600);
        wallet::NonceSweep left;
        {
            EvmRun::Options due;
            due.balancesAt = old;
            due.noncesAt = old;
            EvmRun cut(&model, {rpc.url()}, key, 10050, due);
            check(waitFor([&rpc, asked] { return rpc.nonceCalls > asked + 3000; }, 60000), QStringLiteral("a sweep of every nonce sets out"));
            left = cut.scanner.nonceSweep();
            check(left.from.isValid() && left.index >= 2800, QStringLiteral("and says how far it has got: %1").arg(left.index));
            cut.scanner.stop();
        }
        EvmRun::Options resumed;
        resumed.balancesAt = old;
        resumed.noncesAt = old;
        resumed.sweep = left;
        EvmRun on(&model, {rpc.url()}, key, 10050, resumed);
        check(waitFor([&on] { return on.finished; }, 60000), QStringLiteral("the next run reads on"));
        check(rpc.nonceCalls - asked <= 10050 + 1000, QStringLiteral("from there, not from the top: %1 nonces in the two").arg(rpc.nonceCalls - asked));
        check(on.scanner.noncesAt() == left.from && !on.scanner.nonceSweep().from.isValid(),
              QStringLiteral("and the whole of it counts, from when it began"));
        on.scanner.stop();
    }
    {
        // The same scanner stopped and started in the middle: settings saved, a wallet added.
        FakeRpc rpc;
        wallet::WalletModel model;
        {
            EvmRun first(&model, {rpc.url()}, key, 10050);
            waitFor([&first] { return first.finished; }, 60000);
            first.scanner.stop();
        }
        const int asked = rpc.nonceCalls;
        const QDateTime old = QDateTime::currentDateTime().addSecs(-3 * 3600);
        EvmRun::Options due;
        due.balancesAt = old;
        due.noncesAt = old;
        EvmRun run(&model, {rpc.url()}, key, 10050, due);
        check(waitFor([&rpc, asked] { return rpc.nonceCalls > asked + 3000; }, 60000), QStringLiteral("a sweep is under way"));
        run.scanner.stop();
        run.finished = false;
        run.scanner.start();
        check(waitFor([&run] { return run.finished; }, 60000), QStringLiteral("a restart of the scan reads on"));
        check(rpc.nonceCalls - asked <= 10050 + 1000, QStringLiteral("from where it was: %1 nonces").arg(rpc.nonceCalls - asked));
        run.scanner.stop();
    }
    {
        // A first scan of a large wallet cut short: what it read goes with it.
        FakeRpc rpc;
        wallet::WalletModel model;
        wallet::NonceSweep left;
        {
            EvmRun cut(&model, {rpc.url()}, key, 10050);
            check(waitFor([&rpc] { return rpc.nonceCalls > 3000; }, 60000), QStringLiteral("a first scan is under way"));
            left = cut.scanner.nonceSweep();
            check(left.from.isValid() && !cut.scanner.balancesAt().isValid(), QStringLiteral("and says how far it has got, before any whole pass"));
            cut.scanner.stop();
        }
        EvmRun::Options resumed;
        resumed.sweep = left;
        EvmRun on(&model, {rpc.url()}, key, 10050, resumed);
        check(waitFor([&on] { return on.finished; }, 60000), QStringLiteral("the next run finishes it"));
        check(rpc.nonceCalls <= 10050 + 1000, QStringLiteral("without reading it all again: %1 nonces").arg(rpc.nonceCalls));
        on.scanner.stop();
    }
    {
        // An endpoint that caps a batch: nonces at the size it takes, balances as they were.
        FakeRpc rpc;
        wallet::WalletModel model;
        {
            EvmRun first(&model, {rpc.url()}, key, 300);
            waitFor([&first] { return first.finished; }, 30000);
            first.scanner.stop();
        }
        rpc.maxBatch = 10;
        const QDateTime old = QDateTime::currentDateTime().addSecs(-3 * 3600);
        EvmRun::Options due;
        due.balancesAt = old;
        due.noncesAt = old;
        EvmRun capped(&model, {rpc.url()}, key, 300, due);
        check(waitFor([&capped] { return capped.passes > 0; }, 30000), QStringLiteral("a pass ends on an endpoint that caps batches"));
        check(!capped.scanner.refreshFailing(), QStringLiteral("and its balances count"));
        for (int pass = 1; pass < 12 && capped.scanner.noncesAt() == old; ++pass) {
            capped.scanner.refreshNow();
            waitFor([&capped, pass] { return capped.passes > pass; }, 15000);
        }
        check(capped.scanner.noncesAt() > old, QStringLiteral("and every nonce gets read in the end: %1 passes").arg(capped.passes));
        check(capped.passes == 1, QStringLiteral("in one pass, what was refused being asked again at the size it takes"));
        capped.scanner.stop();
    }
    {
        // An endpoint of another chain.
        FakeRpc other;
        FakeRpc good;
        other.chainId = 5;
        other.ether.insert(evmAddress(key, 1), 1000);
        wallet::WalletModel model;
        EvmRun run(&model, {other.url(), good.url()}, key, 5);
        check(waitFor([&run] { return run.finished; }, 30000) && model.checkedCount() == 5 && model.total(ethereum.coin).isZero(),
              QStringLiteral("an endpoint of another chain is left, and nothing it said counts"));
        run.scanner.stop();

        wallet::AddressRow held;
        held.wallet = key.wallet;
        held.network = ethereum.network;
        held.type = key.type;
        held.account = key.account;
        held.address = evmAddress(key, 0);
        held.setBalance(ethereum.coin, chain::Amount::fromU64(1234));
        held.used = true;
        held.checked = true;
        wallet::WalletModel kept;
        kept.addSaved({held});
        EvmRun alone(&kept, {other.url()}, key, 5);
        check(waitFor([&alone] { return alone.scanner.wrongChain(); }, 30000), QStringLiteral("with no other it is said"));
        check(kept.total(ethereum.coin) == chain::Amount::fromU64(1234), QStringLiteral("and what was held stays"));
        alone.scanner.stop();
    }
    {
        // A pass longer than the refresh: the next addresses go ahead of it.
        wallet::EvmScanner::setMinRefreshForTest(1);
        FakeRpc rpc;
        rpc.delayMs = 100;
        wallet::WalletModel model;
        EvmRun::Options quick;
        quick.refreshSeconds = 1;
        EvmRun run(&model, {rpc.url()}, key, 2000, quick);
        check(waitFor([&run] { return run.finished; }, 60000), QStringLiteral("a pass longer than the refresh ends"));
        const wallet::ScanTiers tiers = run.scanner.tiers();
        check(tiers.live == 20 && tiers.rounds == 1980 && tiers.roundSeconds >= 1,
              QStringLiteral("and the next unused addresses went ahead of it: %1 and %2").arg(tiers.live).arg(tiers.rounds));
        run.scanner.stop();
        wallet::EvmScanner::setMinRefreshForTest(10);
    }
    {
        // A scan that stops with a request waiting its turn.
        FakeRpc rpc;
        net::BudgetPool pool;
        pool.forHost(QStringLiteral("0.1"), 1);
        wallet::WalletModel model;
        EvmRun::Options paced;
        paced.budgets = &pool;
        EvmRun run(&model, {rpc.url()}, key, 5, paced);
        check(waitFor([&rpc] { return rpc.requests >= 1; }, 10000), QStringLiteral("what the pace allows goes"));
        run.scanner.stop();
        const int sent = rpc.requests;
        pause(7000);
        check(rpc.requests == sent, QStringLiteral("and when the scan stops, nothing more goes out"));
    }
    {
        // Past what a start reads in a few minutes, nonces go by the clock, saved across runs.
        FakeRpc rpc;
        wallet::WalletModel model;
        QDateTime balancesAt;
        QDateTime noncesAt;
        {
            EvmRun first(&model, {rpc.url()}, key, 10050);
            check(waitFor([&first] { return first.finished; }, 60000), QStringLiteral("a large account scans"));
            balancesAt = first.scanner.balancesAt();
            noncesAt = first.scanner.noncesAt();
            check(noncesAt.isValid() && rpc.nonceCalls == 10050, QStringLiteral("and the first time reads every nonce"));
            first.scanner.stop();
        }
        const int afterFirst = rpc.nonceCalls;
        {
            EvmRun::Options soon;
            soon.balancesAt = balancesAt;
            soon.noncesAt = noncesAt;
            EvmRun recent(&model, {rpc.url()}, key, 10050, soon);
            check(waitFor([&recent] { return recent.finished; }, 60000) && rpc.nonceCalls == afterFirst,
                  QStringLiteral("a start soon after reads none: those are the expensive ones"));
            check(recent.scanner.noncesAt() == noncesAt, QStringLiteral("and the time they were read stays"));
            recent.scanner.stop();
        }
        {
            const QDateTime old = QDateTime::currentDateTime().addSecs(-3 * 3600);
            EvmRun::Options due;
            due.balancesAt = old;
            due.noncesAt = old;
            EvmRun later(&model, {rpc.url()}, key, 10050, due);
            check(waitFor([&later] { return later.finished; }, 60000) && rpc.nonceCalls == afterFirst + 10050,
                  QStringLiteral("hours later a start reads them all, however short each run was"));
            check(later.scanner.noncesAt() > old, QStringLiteral("and says so"));
            later.scanner.stop();
        }
    }
    {
        // Answers that make no sense from the first endpoint: the next takes over.
        FakeRpc bad;
        FakeRpc good;
        bad.nullNonces = true;
        wallet::WalletModel model;
        EvmRun run(&model, {bad.url(), good.url()}, key, 5);
        check(waitFor([&run] { return run.finished; }, 30000) && model.checkedCount() == 5,
              QStringLiteral("an endpoint with no usable nonces is left for the next"));
        check(bad.requests <= 4 && good.requests > 0, QStringLiteral("after one try: %1 requests").arg(bad.requests));
        run.scanner.stop();
    }
    {
        FakeRpc bad;
        FakeRpc good;
        bad.failSubCalls = true;
        wallet::WalletModel model;
        EvmRun run(&model, {bad.url(), good.url()}, key, 5);
        check(waitFor([&run] { return run.finished; }, 40000) && model.checkedCount() == 5,
              QStringLiteral("so is one whose every balance call fails"));
        run.scanner.stop();
    }
    {
        // Nonces refused on rows that have answered before: the balances still count.
        FakeRpc rpc;
        rpc.ether.insert(evmAddress(key, 1), 9);
        wallet::WalletModel model;
        QDateTime noncesAt;
        {
            EvmRun first(&model, {rpc.url()}, key, 5);
            waitFor([&first] { return first.finished; }, 15000);
            noncesAt = first.scanner.noncesAt();
            first.scanner.stop();
        }
        rpc.refuseNonces = true;
        rpc.ether.insert(evmAddress(key, 1), 11);
        const QDateTime old = QDateTime::currentDateTime().addSecs(-3 * 3600);
        const QDateTime start = QDateTime::currentDateTime().addSecs(-1);
        EvmRun::Options due;
        due.balancesAt = old;
        due.noncesAt = old;
        EvmRun run(&model, {rpc.url()}, key, 5, due);
        check(waitFor([&run] { return run.passes > 0; }, 15000), QStringLiteral("a pass ends though its nonces are refused"));
        check(model.total(ethereum.coin) == chain::Amount::fromU64(11), QStringLiteral("with fresh balances"));
        check(!run.scanner.refreshFailing() && run.scanner.balancesAt() >= start, QStringLiteral("and they count as fresh"));
        check(run.scanner.noncesAt() == old, QStringLiteral("while the time of the nonces stays"));
        rpc.refuseNonces = false;
        const int asked = rpc.nonceCalls;
        run.scanner.refreshNow();
        check(waitFor([&run] { return run.passes > 1; }, 15000) && rpc.nonceCalls > asked, QStringLiteral("and the next pass asks those again"));
        run.scanner.stop();
    }
    {
        // A request waiting its turn on the client itself.
        FakeRpc rpc;
        net::BudgetPool pool;
        const QHash<QString, int> pace{{rpc.url(), 1}};
        net::EthRpc client(QStringLiteral("test"), 1, {rpc.url()}, pace, &pool);
        client.allowLocalHttpForTest();
        client.setEndpoints({rpc.url()});
        // Five calls at one a second: the next waits about five seconds.
        const QList<QPair<QString, QJsonArray>> five(5, qMakePair(QStringLiteral("eth_chainId"), QJsonArray()));
        bool first = false;
        QString waitingError;
        bool waitingDone = false;
        client.batch(five, [&first](const QList<QJsonValue>&, const QStringList&, const QStringList&) { first = true; });
        client.batch(five, [&](const QList<QJsonValue>&, const QStringList& errors, const QStringList&) { waitingError = errors.value(0); waitingDone = true; });
        check(waitFor([&first] { return first; }, 4000), QStringLiteral("what the pace allows goes"));
        client.cancelAll();
        const int sent = rpc.requests;
        check(sent == 1 && waitFor([&waitingDone] { return waitingDone; }, 2000) && !waitingError.isEmpty(), QStringLiteral("what waits ends when told to"));
        pause(6000);
        check(rpc.requests == sent, QStringLiteral("and nothing more goes out"));
    }
    {
        net::Budget budget(20);
        check(budget.take(201) == 0, QStringLiteral("a request larger than the bucket goes"));
        const int wait = budget.take(1);
        check(wait > 7500 && wait <= 8500, QStringLiteral("and the next waits off what the bucket did not hold: %1 ms").arg(wait));
        net::BudgetPool pool;
        pool.forHost(QStringLiteral("a"), 50);
        pool.forHost(QStringLiteral("a"), 1);
        pool.forHost(QStringLiteral("a"), 100).take(2);
        check(pool.forHost(QStringLiteral("a"), 100).take(1) > 0, QStringLiteral("a provider goes at the lowest pace known for it"));
    }
    Q_UNUSED(report);
}

// What was given out from Receive: inside the window whatever anything else says, and followed ahead
// of what is not.
void testGivenOutScans(const Check& check, const wallet::AccountKey& segwit, const wallet::AccountKey& account)
{
    {
        wallet::AccountKey key = segwit;
        for (quint32 index = 0; index < 30; ++index)
            key.given.add(index);
        FakeElectrum server;
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 5, 5);
        check(waitFor([&run] { return run.finished; }, 15000), QStringLiteral("a wallet that has given out 30 addresses scans"));
        check(model.size() == 35 + 5, QStringLiteral("to the gap past what was given out, with no address used (%1)").arg(model.size()));
        server.fund(scripthashOf(key, 0, 29), 400);
        server.notify(scripthashOf(key, 0, 29));
        check(waitFor([&run] { return run.total() == 400; }, 5000), QStringLiteral("a payment to the last one shows by notification"));
        pause(500);
        check(model.size() == 35 + 5, QStringLiteral("the window stays where it was: that one was inside it"));
        server.fund(scripthashOf(key, 0, 34), 100);
        server.notify(scripthashOf(key, 0, 34));
        check(waitFor([&model] { return model.size() == 40 + 5; }, 5000), QStringLiteral("and one past it moves it on (%1)").arg(model.size()));

        // Given out while it runs, with nothing used anywhere.
        run.scanner.stop();
        FakeElectrum quiet;
        wallet::WalletModel fresh;
        wallet::AccountKey none = segwit;
        UtxoRun growing(&fresh, quiet, none, 5, 5);
        check(waitFor([&growing] { return growing.finished; }, 15000), QStringLiteral("a wallet scans its window"));
        const int before = fresh.size();
        none.given.add(quint32(before + 19));
        growing.scanner.noteGiven(none, quint32(before + 19));
        // Its gap past what was given out, on the receive chain, beside the change chain's.
        const int top = before + 30;
        const bool followed = waitFor([&fresh, top] { return fresh.size() == top; }, 8000);
        check(followed, QStringLiteral("given out far past it, the window follows at once (%1 from %2)").arg(fresh.size()).arg(before));
        none.given.add(2);
        growing.scanner.noteGiven(none, 2);
        pause(200);
        check(fresh.size() == top, QStringLiteral("and one given out below that does not take it back"));
    }
    // With too few places for all: what was given out goes ahead, wherever it lies.
    {
        wallet::UtxoScanner::setLivePlacesForTest(40);
        wallet::UtxoScanner::setPacesForTest(300, 200, 100);
        wallet::AccountKey key = segwit;
        key.given.add(70); // far past what the next unused ones reach, with a gap of 100
        FakeElectrum server;
        server.fund(scripthashOf(key, 0, 90), 800); // something used past it
        wallet::WalletModel model;
        {
            UtxoRun run(&model, server, key, 100, 5);
            check(waitFor([&run] { return run.finished; }, 30000), QStringLiteral("past its live places it scans"));
            check(run.scanner.tiers().rounds > 0, QStringLiteral("not all of it followed live"));
            check(server.follows(scripthashOf(key, 0, 70)), QStringLiteral("what was given out is followed, though a later one was paid"));
            check(!server.follows(scripthashOf(key, 0, 50)), QStringLiteral("what lies before it and was never given out is not"));

            // Given out while it runs, below what was used.
            check(run.scanner.tiers().live < 40, QStringLiteral("with a place left"));
            key.given.add(50);
            run.scanner.noteGiven(key, 50);
            check(waitFor([&server, &key] { return server.follows(scripthashOf(key, 0, 50)); }, 10000), QStringLiteral("given out now, it goes live at once"));

            // Paid, it has done what it was followed for.
            server.fund(scripthashOf(key, 0, 50), 300);
            server.notify(scripthashOf(key, 0, 50));
            check(waitFor([&run] { return run.total() == 1100; }, 5000), QStringLiteral("and its payment shows by notification"));
            run.scanner.stop();
        }
        {
            // From what was saved, the same.
            FakeElectrum again;
            again.fund(scripthashOf(key, 0, 90), 800);
            again.fund(scripthashOf(key, 0, 50), 300);
            UtxoRun second(&model, again, key, 100, 5);
            check(waitFor([&second] { return second.finished; }, 30000), QStringLiteral("a new start scans from what was saved"));
            check(again.follows(scripthashOf(key, 0, 70)), QStringLiteral("and what is still waiting for its payment is followed"));
            second.scanner.stop();
        }
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
    }
    // With the next round far off and every place for coins taken: only going live watches it.
    {
        wallet::UtxoScanner::setLivePlacesForTest(40);
        wallet::UtxoScanner::setPacesForTest(600000, 200, 10);
        wallet::AccountKey key = segwit;
        key.given.add(45); // past its places from the very start, so rows take places as they hold coins
        FakeElectrum server;
        for (quint32 index = 0; index <= 76; index += 4)
            server.fund(scripthashOf(key, 0, index), 100);
        wallet::WalletModel model;
        {
            UtxoRun first(&model, server, key, 5, 5);
            check(waitFor([&first] { return first.finished; }, 30000), QStringLiteral("twenty that hold coins scan"));
            first.scanner.stop();
        }
        // From what was saved, those rows are no next addresses, and they take the places for coins.
        FakeElectrum again;
        for (quint32 index = 0; index <= 76; index += 4)
            again.fund(scripthashOf(key, 0, index), 100);
        UtxoRun run(&model, again, key, 5, 5);
        check(waitFor([&run] { return run.finished; }, 30000), QStringLiteral("and start again from what was saved"));
        const wallet::ScanTiers tiers = run.scanner.tiers();
        check(tiers.live == 21 && tiers.rounds > 0, QStringLiteral("ten of them live, with places left: %1 live, %2 in rounds").arg(tiers.live).arg(tiers.rounds));
        check(!again.follows(scripthashOf(key, 0, 50)), QStringLiteral("an unused one below the last used is not followed"));
        key.given.add(50);
        run.scanner.noteGiven(key, 50);
        check(waitFor([&again, &key] { return again.follows(scripthashOf(key, 0, 50)); }, 3000), QStringLiteral("until it is given out: then it goes live at once, past the places for coins"));
        // A new connection keeps it so.
        again.dropConnections();
        check(waitFor([&again, &key] { return !again.follows(scripthashOf(key, 0, 50)); }, 3000), QStringLiteral("the connection goes"));
        check(waitFor([&again, &key] { return again.follows(scripthashOf(key, 0, 50)); }, 30000), QStringLiteral("and with the next it is followed again"));
        run.scanner.stop();
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
    }
    // Every place spoken for, starting from what was saved: what was given out comes first.
    {
        wallet::UtxoScanner::setLivePlacesForTest(40);
        wallet::UtxoScanner::setPacesForTest(300, 200, 100);
        wallet::AccountKey key = segwit;
        key.given.add(52); // below the last used, and not paid
        const auto fundAll = [&key](FakeElectrum& server) {
            for (quint32 index = 0; index <= 55; index += 5)
                server.fund(scripthashOf(key, 0, index), 500);
            server.fund(scripthashOf(key, 1, 0), 100); // so that the change chain's next are known from the start too
        };
        FakeElectrum server;
        fundAll(server);
        wallet::WalletModel model;
        {
            UtxoRun first(&model, server, key, 100, 5);
            check(waitFor([&first] { return first.finished; }, 30000), QStringLiteral("a window longer than its places scans"));
            first.scanner.stop();
        }
        FakeElectrum again;
        fundAll(again);
        UtxoRun second(&model, again, key, 100, 5);
        check(waitFor([&second] { return second.finished; }, 30000), QStringLiteral("and starts again from what was saved"));
        check(second.scanner.tiers().live == 40, QStringLiteral("with every place taken (%1)").arg(second.scanner.tiers().live));
        check(again.follows(scripthashOf(key, 0, 52)), QStringLiteral("and the one given out among them"));
        second.scanner.stop();
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
    }
    // Rows that got a place while everything fit, or as the next ones, keep to the places for coins.
    {
        wallet::UtxoScanner::setLivePlacesForTest(40);
        wallet::UtxoScanner::setPacesForTest(600000, 200, 10);
        wallet::AccountKey key = segwit;
        FakeElectrum server;
        for (quint32 index = 0; index <= 76; index += 4)
            server.fund(scripthashOf(key, 0, index), 100);
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 5, 5);
        check(waitFor([&run] { return run.finished; }, 30000), QStringLiteral("twenty that hold coins, found as they come"));
        check(run.scanner.tiers().live == 20, QStringLiteral("ten of them live and the next ones: %1").arg(run.scanner.tiers().live));
        run.scanner.stop();
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
    }
    // What was given out and has been paid holds coins like any other: it takes a place for that.
    {
        wallet::UtxoScanner::setLivePlacesForTest(40);
        wallet::UtxoScanner::setPacesForTest(600000, 200, 10);
        wallet::AccountKey key = segwit;
        for (quint32 index = 0; index < 40; ++index)
            key.given.add(index);
        const auto fundAll = [&key](FakeElectrum& server) {
            for (quint32 index = 0; index < 40; ++index)
                server.fund(scripthashOf(key, 0, index), 100);
        };
        FakeElectrum server;
        fundAll(server);
        wallet::WalletModel model;
        {
            UtxoRun first(&model, server, key, 5, 5);
            check(waitFor([&first] { return first.finished; }, 30000), QStringLiteral("forty given out and paid"));
            first.scanner.stop();
        }
        FakeElectrum again;
        fundAll(again);
        UtxoRun second(&model, again, key, 5, 5);
        check(waitFor([&second] { return second.finished; }, 30000), QStringLiteral("from what was saved"));
        check(second.scanner.tiers().live == 20, QStringLiteral("ten of them live, and the next ones: %1").arg(second.scanner.tiers().live));
        second.scanner.stop();
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
    }
    // Every place taken: one kept for coins gives way to what was given out just now.
    {
        wallet::UtxoScanner::setLivePlacesForTest(40);
        wallet::UtxoScanner::setPacesForTest(600000, 200, 10);
        wallet::AccountKey key = segwit;
        const auto fundAll = [&key](FakeElectrum& server) {
            for (quint32 index = 0; index <= 220; index += 20)
                server.fund(scripthashOf(key, 0, index), 100);
        };
        FakeElectrum server;
        fundAll(server);
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 30, 5);
        check(waitFor([&run] { return run.finished; }, 30000), QStringLiteral("a wallet that fills its places"));
        check(run.scanner.tiers().live == 40, QStringLiteral("does: %1").arg(run.scanner.tiers().live));
        check(!server.follows(scripthashOf(key, 0, 110)), QStringLiteral("and an unused one below the last used has none"));
        key.given.add(110);
        run.scanner.noteGiven(key, 110);
        check(waitFor([&server, &key] { return server.follows(scripthashOf(key, 0, 110)); }, 3000), QStringLiteral("until it is given out"));
        check(run.scanner.tiers().live == 40, QStringLiteral("and the others stay within them: %1").arg(run.scanner.tiers().live));
        run.scanner.stop();
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
    }
    // One given out far along says nothing of those before it, which nobody was given.
    {
        wallet::UtxoScanner::setLivePlacesForTest(40);
        wallet::UtxoScanner::setPacesForTest(600000, 200, 10);
        wallet::AccountKey key = segwit;
        key.given.add(70);
        FakeElectrum server;
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 100, 5);
        check(waitFor([&run] { return run.finished; }, 30000), QStringLiteral("an address given out far along, with nothing used"));
        check(run.scanner.tiers().live == 31, QStringLiteral("takes one place, beside the next ones: %1").arg(run.scanner.tiers().live));
        check(server.follows(scripthashOf(key, 0, 70)) && server.follows(scripthashOf(key, 0, 24)), QStringLiteral("it and those"));
        check(!server.follows(scripthashOf(key, 0, 50)), QStringLiteral("and none between them"));
        run.scanner.stop();
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
    }
    // Given out while its first read waits behind thousands: it does not wait for them.
    {
        wallet::UtxoScanner::setLivePlacesForTest(40);
        wallet::UtxoScanner::setPacesForTest(600000, 200, 10);
        wallet::AccountKey key = segwit;
        FakeElectrum server;
        wallet::WalletModel model;
        UtxoRun run(&model, server, key, 3000, 5);
        check(waitFor([&model, &key] { return model.find(key.wallet, key.network, key.type, 0, 0, 2500) >= 0; }, 15000), QStringLiteral("a window of thousands, read at a pace"));
        check(!server.follows(scripthashOf(key, 0, 2500)), QStringLiteral("nothing follows one that far along"));
        key.given.add(2500);
        run.scanner.noteGiven(key, 2500);
        check(waitFor([&server, &key] { return server.follows(scripthashOf(key, 0, 2500)); }, 3000), QStringLiteral("given out, it is followed at once"));
        check(!server.follows(scripthashOf(key, 0, 1000)), QStringLiteral("and what lies before it that nobody was given has no claim on a place for that"));
        run.scanner.stop();
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
    }
    {
        wallet::AccountKey key = account;
        key.given.add(7);
        FakeRpc rpc;
        wallet::WalletModel model;
        EvmRun run(&model, {rpc.url()}, key, 5);
        check(waitFor([&run] { return run.finished; }, 15000), QStringLiteral("an Ethereum account that has given out 8 addresses scans"));
        check(model.size() == 13 && model.checkedCount() == 13, QStringLiteral("to the gap past those, every row answered (%1)").arg(model.size()));
        key.given.add(11);
        run.scanner.noteGiven(key, 11);
        check(waitFor([&model] { return model.size() == 17 && model.checkedCount() == 17; }, 10000), QStringLiteral("given out while it runs, its rows come and are read without waiting for a refresh (%1)").arg(model.size()));
        run.scanner.stop();
    }
    // Given out during a long pass: the rows come when it ends, without waiting for a refresh.
    {
        wallet::AccountKey key = account;
        FakeRpc rpc;
        rpc.delayMs = 50;
        wallet::WalletModel model;
        EvmRun run(&model, {rpc.url()}, key, 1500);
        check(waitFor([&model] { return model.size() >= 1500; }, 15000), QStringLiteral("a long pass is under way"));
        check(!run.finished, QStringLiteral("and has not ended"));
        key.given.add(1600);
        run.scanner.noteGiven(key, 1600);
        check(waitFor([&model] { return model.size() == 3101; }, 60000), QStringLiteral("what was given out past it comes, with its gap: %1").arg(model.size()));
        run.scanner.stop();
    }
    // A pass longer than the refresh: what was given out goes ahead of it too, wherever it lies.
    {
        wallet::EvmScanner::setMinRefreshForTest(1);
        wallet::AccountKey key = account;
        key.given.add(500);
        FakeRpc rpc;
        rpc.delayMs = 100;
        rpc.nonces.insert(evmAddress(key, 1000), 1); // used past it
        wallet::WalletModel model;
        EvmRun::Options quick;
        quick.refreshSeconds = 1;
        EvmRun run(&model, {rpc.url()}, key, 2000, quick);
        check(waitFor([&run] { return run.finished; }, 60000), QStringLiteral("a pass longer than the refresh ends"));
        check(run.scanner.tiers().live == 21, QStringLiteral("and what was given out went ahead of it, beside the next unused ones: %1").arg(run.scanner.tiers().live));
        run.scanner.stop();
        wallet::EvmScanner::setMinRefreshForTest(10);
    }
    // An endpoint that said a lie leaves what was given out where it is.
    {
        wallet::AccountKey key = account;
        key.given.add(6);
        FakeRpc rpc;
        for (quint32 i = 0; i < 20; ++i)
            rpc.nonces.insert(evmAddress(key, i), 1);
        wallet::WalletModel model;
        {
            EvmRun inflated(&model, {rpc.url()}, key, 5);
            check(waitFor([&inflated] { return inflated.finished; }, 15000) && model.size() == 25, QStringLiteral("an endpoint can call addresses used"));
            inflated.scanner.stop();
        }
        rpc.nonces.clear();
        EvmRun honest(&model, {rpc.url()}, key, 5);
        check(waitFor([&honest] { return honest.finished; }, 15000), QStringLiteral("an honest one reads them all"));
        check(model.size() == 12, QStringLiteral("and the rows left are what was given out and its gap (%1)").arg(model.size()));
        honest.scanner.stop();
    }
}

void testModelDrop(const Check& check, const wallet::AccountKey& key)
{
    wallet::WalletModel model;
    QVector<wallet::AddressRow> rows;
    for (quint32 i = 0; i < 6; ++i) {
        wallet::AddressRow row = savedRow(key, 0, i);
        row.used = i % 2 == 0;
        row.confirmed = i % 2 == 0 ? 10 : 0;
        rows.append(row);
    }
    model.addSaved(rows);
    int cleared = 0;
    int dropped = 0;
    QObject::connect(&model, &wallet::WalletModel::cleared, &model, [&cleared] { ++cleared; });
    QObject::connect(&model, &wallet::WalletModel::rowsDropped, &model, [&dropped] { ++dropped; });
    model.dropRows([](const wallet::AddressRow&) { return true; });
    check(dropped == 0, QStringLiteral("dropping no rows says nothing"));
    model.dropRows([](const wallet::AddressRow& row) { return row.index < 3; });
    const wallet::Asset coin = wallet::networkInfo(key.network).coin;
    check(dropped == 1 && cleared == 0, QStringLiteral("dropping rows is no reason to start a scan over"));
    check(model.size() == 3 && model.total(coin) == chain::Amount::fromU64(20) && model.usedCount() == 2 && model.findByScripthash(key.network, rows.at(4).scripthash) < 0
              && model.at(model.findByScripthash(key.network, rows.at(2).scripthash)).index == 2,
          QStringLiteral("what stays is counted, and found where it now is"));

    // What the database kept, over rows a scan has only derived so far.
    wallet::WalletModel blank;
    wallet::AddressRow derived = savedRow(key, 0, 0);
    derived.checked = false;
    blank.upsert(derived);
    wallet::AddressRow answered = savedRow(key, 0, 1);
    answered.confirmed = 7;
    answered.used = true;
    blank.upsert(answered);
    wallet::AddressRow savedZero = savedRow(key, 0, 0);
    savedZero.confirmed = 500;
    savedZero.used = true;
    wallet::AddressRow savedOne = savedRow(key, 0, 1);
    savedOne.confirmed = 999;
    savedOne.used = true;
    int changed = 0;
    QObject::connect(&blank, &wallet::WalletModel::rowChanged, &blank, [&changed] { ++changed; });
    blank.addSaved({savedZero, savedOne});
    check(blank.size() == 2 && blank.total(coin) == chain::Amount::fromU64(507) && changed == 1,
          QStringLiteral("a saved row fills in one only derived, and leaves one that has answered"));
}

void testDatabase(const Check& check, QString* report)
{
    const wallet::NetworkInfo& bitcoin = *wallet::findNetwork(QStringLiteral("bitcoin"));
    const QString path = wallet::WalletStore::dataDir() + QStringLiteral("/scan-test.db");
    const QByteArray key = wallet::WalletStore::newDatabaseKey();
    const auto wipe = [&path] {
        QFile::remove(path);
        QFile::remove(path + QStringLiteral("-wal"));
        QFile::remove(path + QStringLiteral("-shm"));
    };
    wipe();
    QVector<wallet::AddressRow> rows;
    for (quint32 i = 0; i < 2000; ++i) {
        wallet::AddressRow row;
        row.wallet = 1 + i % 3;
        row.network = bitcoin.network;
        row.index = i / 3;
        row.address = QStringLiteral("bc1qsomeaddressthatfillspages%1").arg(i);
        row.scripthash = QStringLiteral("%1").arg(i + 1, 64, 16, QLatin1Char('0'));
        row.checked = true;
        rows.append(row);
    }
    QVector<wallet::AddressRow> shuffled = rows;
    std::mt19937 random(7);
    std::shuffle(shuffled.begin(), shuffled.end(), random);
    QString why;
    QElapsedTimer clock;
    {
        store::LedgerDb db;
        check(db.open(path, key, &why) == store::LedgerDb::Open::Ready && db.saveRows(shuffled, &why), QStringLiteral("rows are written in any order: ") + why);
        QVector<wallet::AddressRow> read;
        check(db.loadRows({1, 2, 3}, 500, [&read](QVector<wallet::AddressRow>&& chunk) { read += chunk; }, &why) && read.size() == rows.size(),
              QStringLiteral("and read back"));
        const bool sorted = std::is_sorted(read.cbegin(), read.cend(), [](const wallet::AddressRow& a, const wallet::AddressRow& b) {
            return std::make_pair(a.wallet, a.index) < std::make_pair(b.wallet, b.index);
        });
        check(sorted, QStringLiteral("by wallet and index, whatever order they went in"));

        // What times say.
        const QDateTime at = QDateTime::fromSecsSinceEpoch(1793587557, QTimeZone::UTC);
        const QDateTime nonces = at.addSecs(-60);
        check(db.saveTimes({{QStringLiteral("ethereum"), at, nonces}, {QStringLiteral("bitcoin"), at, QDateTime()}}, &why), QStringLiteral("times are saved"));
        const QList<store::SavedTime> times = db.loadTimes(&why);
        const auto eth = std::find_if(times.cbegin(), times.cend(), [](const store::SavedTime& t) { return t.network == QLatin1String("ethereum"); });
        const auto btc = std::find_if(times.cbegin(), times.cend(), [](const store::SavedTime& t) { return t.network == QLatin1String("bitcoin"); });
        check(eth != times.cend() && eth->at == at && eth->noncesAt == nonces && btc != times.cend() && !btc->noncesAt.isValid(),
              QStringLiteral("with the time of the nonces where there is one"));
    }

    // A power cut during a checkpoint: the main file is torn, and the WAL holds it all.
    {
        const QString crash = path + QStringLiteral("-crash");
        store::LedgerDb open;
        // Rows that differ: what is already there is not written again.
        QVector<wallet::AddressRow> changed = rows.mid(0, 200);
        for (wallet::AddressRow& row : changed)
            row.status = QStringLiteral("changed");
        check(open.open(path, key, &why) == store::LedgerDb::Open::Ready && open.saveRows(changed, &why), QStringLiteral("rows wait in the WAL"));
        QFile::remove(crash);
        QFile::remove(crash + QStringLiteral("-wal"));
        check(QFile::copy(path, crash) && QFile::copy(path + QStringLiteral("-wal"), crash + QStringLiteral("-wal")), QStringLiteral("the files as a power cut leaves them"));
        QFile torn(crash);
        if (torn.open(QIODevice::Append)) {
            torn.write(QByteArray(8192, '\0'));
            torn.close();
        }
        {
            store::LedgerDb after;
            const store::LedgerDb::Open opened = after.open(crash, key, &why);
            check(opened == store::LedgerDb::Open::Ready && after.rowCount() == rows.size(),
                  QStringLiteral("a checkpoint torn by a power cut is no damage: %1, %2 rows, %3").arg(int(opened)).arg(after.rowCount()).arg(why));
        }
        QFile::remove(crash);
        QFile::remove(crash + QStringLiteral("-wal"));
        QFile::remove(crash + QStringLiteral("-shm"));
    }

    const auto rawExec = [&path, &key](const char* sql) {
        sqlite3* raw = nullptr;
        sqlite3_open_v2(path.toUtf8().constData(), &raw, SQLITE_OPEN_READWRITE, nullptr);
        const QByteArray pragma = "PRAGMA key = \"x'" + key.toHex() + "'\"";
        sqlite3_exec(raw, pragma.constData(), nullptr, nullptr, nullptr);
        const int done = sqlite3_exec(raw, sql, nullptr, nullptr, nullptr);
        sqlite3_close_v2(raw);
        return done == SQLITE_OK;
    };

    // A write that fails halfway, after adding a name.
    {
        store::LedgerDb db;
        check(db.open(path, key, &why) == store::LedgerDb::Open::Ready, QStringLiteral("the file opens: ") + why);
        check(rawExec("CREATE TRIGGER refuse BEFORE INSERT ON address WHEN new.idx = 99999 BEGIN SELECT RAISE(ABORT, 'no'); END"),
              QStringLiteral("a write can be made to fail"));
        wallet::AddressRow failing = rows.first();
        failing.type = chain::ScriptType::P2PKH; // a name the file does not have yet
        failing.index = 99999;
        check(!db.saveRows({failing}, &why), QStringLiteral("a write that fails halfway says so"));
        check(rawExec("DROP TRIGGER refuse"), QStringLiteral("and can be made to work again"));
        wallet::AddressRow good = rows.first();
        good.type = chain::ScriptType::P2PKH;
        good.index = 50000;
        check(db.saveRows({good}, &why), QStringLiteral("the name it added is not taken for one the file has: ") + why);
        QVector<wallet::AddressRow> read;
        db.loadRows({1, 2, 3}, 500, [&read](QVector<wallet::AddressRow>&& chunk) { read += chunk; }, &why);
        const bool back = std::any_of(read.cbegin(), read.cend(), [](const wallet::AddressRow& row) {
            return row.index == 50000 && row.type == chain::ScriptType::P2PKH;
        });
        check(back, QStringLiteral("and the row reads back as written"));
    }

    // A flush that fails, and goes through later.
    {
        wallet::WalletModel model;
        store::Ledger ledger(&model);
        bool loaded = false;
        QStringList notices;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        QObject::connect(&ledger, &store::Ledger::problem, &ledger, [&notices](const QString& message) { notices << message; });
        ledger.open(path, key, {1, 2, 3});
        waitFor([&loaded] { return loaded; }, 10000);
        const int before = model.size();
        check(rawExec("CREATE TRIGGER refuse BEFORE INSERT ON address WHEN new.idx = 77777 BEGIN SELECT RAISE(ABORT, 'no'); END"),
              QStringLiteral("a flush can be made to fail"));
        wallet::AddressRow row = rows.first();
        row.index = 77777;
        model.upsert(row);
        ledger.flush();
        check(waitFor([&notices] { return !notices.isEmpty(); }, 15000), QStringLiteral("a failed write is said"));
        const QDateTime held = QDateTime::fromSecsSinceEpoch(1793000000, QTimeZone::UTC);
        ledger.saveTimes({{QStringLiteral("held"), held, QDateTime(), wallet::NonceSweep()}});
        check(rawExec("DROP TRIGGER refuse"), QStringLiteral("and the disk comes back"));
        const auto inFile = [&path, &key] {
            store::LedgerDb db;
            QString quiet;
            return db.open(path, key, &quiet) == store::LedgerDb::Open::Ready ? db.rowCount() : -1;
        };
        check(waitFor([&inFile, before] { return inFile() == before + 1; }, 20000), QStringLiteral("the row reaches the file by itself, with what was held back"));
        ledger.close();
        store::LedgerDb db;
        db.open(path, key, &why);
        const QList<store::SavedTime> times = db.loadTimes(&why);
        check(std::none_of(times.cbegin(), times.cend(), [](const store::SavedTime& t) { return t.network == QLatin1String("held"); }),
              QStringLiteral("no time was saved for rows that were not there"));
    }

    // A row of a network not on the list stays through a rewrite.
    {
        sqlite3* raw = nullptr;
        sqlite3_open_v2(path.toUtf8().constData(), &raw, SQLITE_OPEN_READWRITE, nullptr);
        const QByteArray pragma = "PRAGMA key = \"x'" + key.toHex() + "'\"";
        sqlite3_exec(raw, pragma.constData(), nullptr, nullptr, nullptr);
        const int inserted = sqlite3_exec(raw,
                                          "INSERT INTO name(text) VALUES('gone-for-now');"
                                          "INSERT INTO address(wallet, network, type, account, branch, idx, address, checked, used, confirmed, unconfirmed, nonce) "
                                          "SELECT 1, id, (SELECT min(id) FROM name), 0, 0, 0, 'x', 1, 0, 0, 0, 0 FROM name WHERE text = 'gone-for-now';",
                                          nullptr, nullptr, nullptr);
        sqlite3_close_v2(raw);
        store::LedgerDb db;
        check(inserted == SQLITE_OK && db.open(path, key, &why) == store::LedgerDb::Open::Ready && db.rowCount() > rows.size(), QStringLiteral("a row of a network gone from the list is in the file"));
        check(db.replaceRows(rows.mid(0, 10), &why) && db.rowCount() == 11, QStringLiteral("and a rewrite of what is on the list leaves it there"));
        check(db.saveRows(rows, &why) && db.rowCount() == rows.size() + 1, QStringLiteral("the rows are back"));
    }

    // Damage past the first page.
    {
        clock.start();
        store::LedgerDb db;
        check(db.open(path, key, &why) == store::LedgerDb::Open::Ready, QStringLiteral("the file opens"));
        const qint64 checkedMs = clock.elapsed();
        *report += QStringLiteral("      opening with every page checked, %1 KB: %2 ms\n").arg(QFile(path).size() / 1024).arg(checkedMs);
    }
    // Its WAL is written back at the close.
    QFile file(path);
    const qint64 size = file.size();
    check(size > 4096 * 8 && file.open(QIODevice::ReadWrite), QStringLiteral("the file is long enough to damage"));
    file.seek(4096 * 5 + 100);
    const QByteArray one = file.read(1);
    file.seek(4096 * 5 + 100);
    file.write(QByteArray(1, char(one.at(0) ^ 0x55)));
    file.close();
    {
        store::LedgerDb db;
        check(db.open(path, key, &why) == store::LedgerDb::Open::NotOurs, QStringLiteral("a damaged page is found before anything is read: ") + why);
    }

    // And what the wallet makes of that.
    {
        wallet::WalletModel model;
        store::Ledger ledger(&model);
        QStringList asides;
        bool loaded = false;
        QObject::connect(&ledger, &store::Ledger::setAside, &ledger, [&asides](const QString& file) { asides << file; });
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded](const QList<store::SavedPrice>&, const QList<store::SavedTime>&) { loaded = true; });
        ledger.open(path, key, {1, 2, 3});
        check(waitFor([&loaded] { return loaded; }, 15000), QStringLiteral("a damaged database still lets the wallet start"));
        check(asides.size() == 1 && QFile::exists(asides.first()) && model.size() == 0, QStringLiteral("it is kept aside, and says so, and no figure comes from it"));
        check(QFile::exists(path), QStringLiteral("and a new one is in use"));
        wallet::AddressRow row = rows.first();
        model.upsert(row);
        ledger.flush();
        ledger.close();
        store::LedgerDb db;
        check(db.open(path, key, &why) == store::LedgerDb::Open::Ready && db.rowCount() == 1, QStringLiteral("and takes new rows"));
        db.close();
        for (const QString& file : store::Ledger::keptAsideFiles(path))
            QFile::remove(wallet::WalletStore::dataDir() + QLatin1Char('/') + file);
    }
    wipe();
}

// What a settings save or an added wallet makes the Bitcoin scanner do on the window's thread.
void testStartAtScale(const Check& check, const QList<wallet::AccountKey>& keys, QString* report)
{
    QList<wallet::AccountKey> accounts;
    for (quint32 wallet = 1; wallet <= 500; ++wallet) {
        for (wallet::AccountKey key : keys) {
            if (!chain::isUtxo(key.type))
                continue;
            key.wallet = wallet;
            accounts.append(key);
        }
    }
    wallet::WalletModel model;
    const wallet::AccountKey& first = accounts.first();
    // Every chain with rows, its first one real: what a start proves each chain by. Some
    // used, some holding coins.
    QVector<wallet::AddressRow> rows;
    rows.reserve(425000);
    const int perChain = 425000 / int(accounts.size() * 2);
    int serial = 0;
    for (const wallet::AccountKey& key : std::as_const(accounts)) {
        for (quint32 change = 0; change <= 1; ++change) {
            rows.append(savedRow(key, change, 0));
            for (int index = 1; index < perChain; ++index) {
                wallet::AddressRow row;
                row.wallet = key.wallet;
                row.network = key.network;
                row.type = key.type;
                row.change = change;
                row.index = quint32(index);
                row.address = QStringLiteral("bc1qtestaddress%1").arg(++serial);
                row.scripthash = QStringLiteral("%1").arg(qulonglong(serial), 64, 16, QLatin1Char('0'));
                row.checked = true;
                row.used = index < perChain / 2;
                row.confirmed = index == 1 ? 1000 : 0;
                rows.append(row);
            }
        }
    }
    model.addSaved(rows);
    wallet::UtxoScanner scanner(first.network, &model);
    wallet::ScanSettings settings;
    settings.servers.insert(wallet::networkInfo(first.network).id, {QStringLiteral("127.0.0.1:1")});
    scanner.usePlainTcpForTest();
    scanner.setAccounts(accounts);
    scanner.setSettings(settings);
    QElapsedTimer clock;
    clock.start();
    scanner.start();
    const qint64 ms = clock.elapsed();
    scanner.stop();
    *report += QStringLiteral("      a scan starting over 500 wallets, %1 accounts, 425,000 rows: %2 ms on the window's thread\n").arg(accounts.size()).arg(ms);
    check(ms < 5000, QStringLiteral("a scan starting at this size takes under five seconds: %1 ms").arg(ms));
}

} // namespace

void runGivenOutScans(const Check& check, const QList<wallet::AccountKey>& someKeys)
{
    wallet::AccountKey segwit;
    wallet::AccountKey account;
    for (const wallet::AccountKey& key : someKeys) {
        if (key.type == chain::ScriptType::P2WPKH)
            segwit = key;
        else if (key.type == chain::ScriptType::EthAccount)
            account = key;
    }
    segwit.wallet = 1;
    account.wallet = 1;
    testGivenOutScans(check, segwit, account);
}

void runScanTest(const Check& check, QString* report, const QList<wallet::AccountKey>& someKeys)
{
    wallet::AccountKey segwit;
    wallet::AccountKey account;
    for (const wallet::AccountKey& key : someKeys) {
        if (key.type == chain::ScriptType::P2WPKH)
            segwit = key;
        else if (key.type == chain::ScriptType::EthAccount)
            account = key;
    }
    segwit.wallet = 1;
    account.wallet = 1;
    testElectrumClient(check, segwit);
    testUtxoLive(check, segwit);
    testUtxoRounds(check, segwit);
    testUtxoRefusals(check, segwit);
    testUtxoServers(check, segwit);
    testUtxoSaved(check, segwit);
    testEvm(check, account, report);
    testGivenOutScans(check, segwit, account);
    testModelDrop(check, segwit);
    testDatabase(check, report);
    testStartAtScale(check, someKeys, report);
}

} // namespace app
