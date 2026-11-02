#include "app/WalletController.h"
#include "chain/Transaction.h"
#include "net/ElectrumClient.h"
#include "net/PriceHistory.h"
#include "store/Ledger.h"
#include "store/LedgerDb.h"
#include "ui/HistoryTableModel.h"
#include "ui/pages/HistoryPage.h"
#include "wallet/HeaderChain.h"
#include "wallet/History.h"
#include "wallet/UtxoHistory.h"
#include "wallet/UtxoScanner.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <sqlcipher/sqlite3.h>

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTableView>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <random>
#include <thread>

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

// One byte of a file, changed whatever it held: a fixed value may be what is there already.
void flipByte(const QString& path, qint64 at)
{
    QFile file(path);
    file.open(QIODevice::ReadWrite);
    file.seek(at);
    const char was = file.read(1).at(0);
    file.seek(at);
    const char flipped = char(was ^ 0x5a);
    file.write(&flipped, 1);
}

chain::Hash256 hashOf(const char* hex)
{
    chain::Hash256 out{};
    chain::hashFromHex(hex, &out);
    return out;
}

chain::Bytes bytesOf(const QString& hex)
{
    const QByteArray raw = QByteArray::fromHex(hex.toLatin1());
    return chain::Bytes(raw.cbegin(), raw.cend());
}

QString hexOf(const chain::Bytes& bytes)
{
    return QString::fromStdString(chain::toHex(bytes));
}

QString hexOf(const chain::Hash256& hash)
{
    return QString::fromStdString(chain::hashHex(hash));
}

void putU32(chain::Bytes* out, uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out->push_back(uint8_t(value >> shift));
}

void putU64(chain::Bytes* out, uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
        out->push_back(uint8_t(value >> shift));
}

// A transaction with what a test needs of it: no signatures, as nothing here checks them.
chain::Bytes rawTransaction(const QVector<chain::OutPoint>& inputs, const QVector<QPair<chain::Bytes, qint64>>& outputs, uint32_t nonce, uint32_t sequence = 0xffffffff)
{
    chain::Bytes raw;
    putU32(&raw, 2);
    raw.push_back(uint8_t(inputs.size()));
    for (const chain::OutPoint& input : inputs) {
        raw.insert(raw.end(), input.txid.rbegin(), input.txid.rend());
        putU32(&raw, input.n);
        raw.push_back(0); // no script
        putU32(&raw, sequence);
    }
    raw.push_back(uint8_t(outputs.size()));
    for (const auto& output : outputs) {
        putU64(&raw, uint64_t(output.second));
        raw.push_back(uint8_t(output.first.size()));
        raw.insert(raw.end(), output.first.begin(), output.first.end());
    }
    putU32(&raw, nonce); // the lock time: each transaction is its own
    return raw;
}

// One that a single byte cannot count: inputs and outputs by the thousand.
chain::Bytes rawOfMany(const QVector<chain::OutPoint>& inputs, const QVector<chain::Bytes>& scripts, uint32_t nonce)
{
    const auto count = [](chain::Bytes* out, size_t n) {
        out->push_back(0xfe);
        putU32(out, uint32_t(n));
    };
    chain::Bytes raw;
    putU32(&raw, 2);
    count(&raw, size_t(inputs.size()));
    for (const chain::OutPoint& input : inputs) {
        raw.insert(raw.end(), input.txid.rbegin(), input.txid.rend());
        putU32(&raw, input.n);
        raw.push_back(0);
        putU32(&raw, 0xffffffff);
    }
    count(&raw, size_t(scripts.size()));
    for (const chain::Bytes& script : scripts) {
        putU64(&raw, 1);
        raw.push_back(uint8_t(script.size()));
        raw.insert(raw.end(), script.begin(), script.end());
    }
    putU32(&raw, nonce);
    return raw;
}

// A little chain and a mempool, answering as an Electrum server does, and able to lie the way a
// server could.
class FakeChain : public QObject
{
public:
    struct Tx {
        chain::Bytes raw;
        chain::Transaction parsed;
        int height = 0; // 0: in the mempool
    };

    explicit FakeChain(int startHeight = 20)
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
        for (int i = 0; i <= startHeight; ++i)
            mine();
    }

    QString line() const { return QStringLiteral("127.0.0.1:%1").arg(server_.serverPort()); }
    int tip() const { return int(blocks_.size()) - 1; }
    const chain::BlockHeader& header(int height) const { return blocks_.at(height).header; }

    // What a build carries for this chain: a checkpoint at `height`, as coins.json has for Bitcoin.
    wallet::ChainProofs proofs(int height, int retarget = 0) const
    {
        wallet::ChainProofs out;
        out.powLimit = kEasyBits;
        out.retargetBlocks = retarget;
        out.targetSeconds = 1;
        out.checkpointHeight = height;
        out.checkpointHash = header(height).hash;
        out.checkpointBits = kEasyBits;
        out.checkpointTime = header(height).time;
        if (retarget > 0)
            out.periodStart = header(height / retarget * retarget).time;
        std::vector<chain::Hash256> leaves;
        for (int i = 0; i <= height; ++i)
            leaves.push_back(blocks_.at(i).header.hash);
        out.headersRoot = rootOf(leaves);
        return out;
    }

    chain::Hash256 pay(const QVector<chain::OutPoint>& inputs, const QVector<QPair<chain::Bytes, qint64>>& outputs, uint32_t sequence = 0xffffffff)
    {
        Tx tx;
        tx.raw = rawTransaction(inputs, outputs, ++nextNonce_, sequence);
        chain::parseTransaction(tx.raw, &tx.parsed);
        const chain::Hash256 txid = tx.parsed.txid;
        txs_.insert(txid, tx);
        mempool_.append(txid);
        return txid;
    }
    // Into a block now, from nowhere the wallets know.
    chain::Hash256 fund(const chain::Bytes& script, qint64 value)
    {
        chain::Hash256 foreign{};
        foreign[0] = uint8_t(++nextForeign_);
        return pay({{foreign, 0}}, {{script, value}});
    }

    void mine(const QVector<chain::Hash256>& only = {}, bool keepOthers = false)
    {
        Block block;
        block.txs = only.isEmpty() ? mempool_ : only;
        if (!keepOthers && only.isEmpty())
            mempool_.clear();
        for (const chain::Hash256& txid : std::as_const(block.txs)) {
            mempool_.removeAll(txid);
            txs_[txid].height = int(blocks_.size());
        }
        block.header = mined(blocks_.isEmpty() ? chain::Hash256{} : blocks_.last().header.hash, block.txs, ++nextTime_);
        blocks_.append(block);
    }

    // The last `depth` blocks give way to others, with what they held, plus one more.
    void reorganise(int depth, const QVector<chain::Hash256>& inNew)
    {
        QVector<chain::Hash256> freed;
        for (int i = 0; i < depth; ++i) {
            for (const chain::Hash256& txid : std::as_const(blocks_.last().txs)) {
                txs_[txid].height = 0;
                freed.append(txid);
            }
            blocks_.removeLast();
        }
        // A block with nothing, so what was there lands higher.
        ++nextTime_;
        Block empty;
        empty.header = mined(blocks_.last().header.hash, {}, ++nextTime_);
        blocks_.append(empty);
        mine(inNew, true);
        for (const chain::Hash256& txid : std::as_const(freed)) {
            if (!inNew.contains(txid))
                mempool_.append(txid);
        }
    }

    // The last block gives way to another at the same height, holding the same, with one more on top.
    void reorganiseInPlace()
    {
        const QVector<chain::Hash256> held = blocks_.last().txs;
        blocks_.removeLast();
        mine(held, true);
        Block empty;
        empty.header = mined(blocks_.last().header.hash, {}, ++nextTime_);
        blocks_.append(empty);
    }

    // The last block gives way to empty ones, and what it held is gone. What is pending stays so.
    void reorganiseAway()
    {
        const QVector<chain::Hash256> held = blocks_.last().txs;
        for (const chain::Hash256& txid : held)
            forget(txid);
        blocks_.removeLast();
        for (int i = 0; i < 2; ++i) {
            Block empty;
            empty.header = mined(blocks_.last().header.hash, {}, ++nextTime_);
            blocks_.append(empty);
        }
    }

    // The line goes down. Unless it is told to stay down, a client can come back.
    void dropPeers(bool stayDown = false)
    {
        if (stayDown)
            server_.close();
        for (QTcpSocket* peer : server_.findChildren<QTcpSocket*>())
            peer->abort();
    }

    // What a pending transaction does once it is gone from the mempool, replaced or not.
    void forget(const chain::Hash256& txid)
    {
        mempool_.removeAll(txid);
        txs_.remove(txid);
    }

    qint64 balanceOf(const QString& scripthash) const
    {
        qint64 balance = 0;
        for (auto it = txs_.cbegin(); it != txs_.cend(); ++it) {
            if (it->height == 0 && !mempool_.contains(it.key()))
                continue;
            const chain::Transaction& tx = it->parsed;
            for (const chain::TxOutput& output : tx.outputs) {
                if (QString::fromStdString(chain::scripthashOfScript(output.script)) == scripthash)
                    balance += qint64(output.value);
            }
            for (const chain::TxInput& input : tx.inputs) {
                const auto parent = txs_.constFind(input.previous.txid);
                if (parent == txs_.cend() || input.previous.n >= parent->parsed.outputs.size())
                    continue;
                const chain::TxOutput& spent = parent->parsed.outputs[input.previous.n];
                if (QString::fromStdString(chain::scripthashOfScript(spent.script)) == scripthash)
                    balance -= qint64(spent.value);
            }
        }
        return balance + balanceLie;
    }
    void notifyTip()
    {
        const QJsonObject message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("method"), QStringLiteral("blockchain.headers.subscribe")},
                                  {QStringLiteral("params"), QJsonArray{QJsonObject{{QStringLiteral("height"), tip()}, {QStringLiteral("hex"), hexOf(chain::serializeHeader(header(tip())))}}}}};
        for (QTcpSocket* peer : server_.findChildren<QTcpSocket*>())
            send(peer, message);
    }

    // Listed under an address, with a status to match, and touching nothing of it.
    chain::Hash256 invent(const QString& scripthash, int height)
    {
        Tx tx;
        tx.raw = rawTransaction({{hashOf("00000000000000000000000000000000000000000000000000000000000000f0"), 0}}, {{{0x51}, 1}}, ++nextNonce_);
        chain::parseTransaction(tx.raw, &tx.parsed);
        tx.height = height;
        const chain::Hash256 txid = tx.parsed.txid;
        txs_.insert(txid, tx);
        invented_[scripthash].append({txid, height});
        return txid;
    }

    // What this server is made to do.
    bool lieInRaw = false;     // a transaction that hashes to something else
    bool lieInMerkle = false;  // a branch that leads nowhere
    bool lieInBranch = false;  // a header that does not lead to the checkpoint
    bool badWork = false;      // headers past the checkpoint that follow each other, without the work
    int wrongBitsAt = -1;      // from this height on, with their work, at a difficulty the rule does not give
    bool refuseRaw = false;
    bool refuseProofs = false;
    bool refuseBranch = false; // a header below the checkpoint
    bool refuseChunks = false; // headers past it
    bool emptyChunks = false;  // none, while it says it has more
    bool listLeavesOut = false; // a list that is not what its status says
    bool hideAll = false;      // nothing of any address: no status, no list, no balance
    QSet<chain::Hash256> refuseRawOf; // these raw transactions only
    QHash<chain::Hash256, int> rawDelay; // ms before the answer about these
    int heightLie = 0;         // added to each height in a list, and in the status that goes with it
    QSet<QString> unread;      // addresses it will not read, whose balance it still gives
    qint64 balanceLie = 0;
    int replyDelayMs = 0;      // every header asked by itself, later and later
    QHash<QString, int> calls;
    int connections = 0;

private:
    static constexpr uint32_t kEasyBits = 0x207fffff;

    struct Block {
        chain::BlockHeader header;
        QVector<chain::Hash256> txs;
    };

    bool touchesHere(const Tx& tx, const QString& scripthash) const
    {
        for (const chain::TxOutput& output : tx.parsed.outputs) {
            if (QString::fromStdString(chain::scripthashOfScript(output.script)) == scripthash)
                return true;
        }
        for (const chain::TxInput& input : tx.parsed.inputs) {
            const auto parent = txs_.constFind(input.previous.txid);
            if (parent != txs_.cend() && input.previous.n < parent->parsed.outputs.size()
                && QString::fromStdString(chain::scripthashOfScript(parent->parsed.outputs[input.previous.n].script)) == scripthash)
                return true;
        }
        return false;
    }

    static chain::Hash256 rootOf(std::vector<chain::Hash256> level)
    {
        if (level.empty())
            return chain::Hash256{};
        while (level.size() > 1) {
            if (level.size() % 2 != 0)
                level.push_back(level.back());
            std::vector<chain::Hash256> next;
            for (size_t i = 0; i < level.size(); i += 2) {
                std::vector<chain::Hash256> branch{level[i + 1]};
                next.push_back(chain::merkleRootOf(level[i], branch, 0));
            }
            level = std::move(next);
        }
        return level.front();
    }
    static std::vector<chain::Hash256> branchOf(std::vector<chain::Hash256> level, size_t index)
    {
        std::vector<chain::Hash256> branch;
        while (level.size() > 1) {
            if (level.size() % 2 != 0)
                level.push_back(level.back());
            branch.push_back(level[index ^ 1]);
            std::vector<chain::Hash256> next;
            for (size_t i = 0; i < level.size(); i += 2) {
                std::vector<chain::Hash256> pair{level[i + 1]};
                next.push_back(chain::merkleRootOf(level[i], pair, 0));
            }
            level = std::move(next);
            index /= 2;
        }
        return branch;
    }

    chain::BlockHeader mined(const chain::Hash256& previous, const QVector<chain::Hash256>& txs, uint32_t time) const
    {
        std::vector<chain::Hash256> leaves(txs.cbegin(), txs.cend());
        chain::BlockHeader header;
        header.version = 2;
        header.previous = previous;
        header.merkleRoot = leaves.empty() ? chain::Hash256{} : rootOf(leaves);
        header.time = time;
        header.bits = kEasyBits;
        for (header.nonce = 0;; ++header.nonce) {
            chain::BlockHeader withHash;
            chain::parseHeader(chain::serializeHeader(header), &withHash);
            if (chain::headerHasWork(withHash, kEasyBits))
                return withHash;
        }
    }

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
        ++calls[method];
        if (method == QLatin1String("server.version")) {
            answer(peer, id, QJsonArray{QStringLiteral("Fake 1.0"), QStringLiteral("1.4.2")});
        } else if (method == QLatin1String("blockchain.block.header")) {
            const int height = params.at(0).toInt();
            // The first block of the real chain: what the wallet makes sure of before anything else.
            if (height == 0 && params.size() == 1) {
                answer(peer, id, QString::fromLatin1(kGenesisHeader));
            } else if (params.size() == 1) {
                const QString hex = hexOf(chain::serializeHeader(header(height)));
                if (replyDelayMs > 0)
                    QTimer::singleShot(replyDelayMs += 5, peer, [this, peer, id, hex] { answer(peer, id, hex); });
                else
                    answer(peer, id, hex);
            } else if (refuseBranch) {
                refuse(peer, id);
            } else {
                const int checkpoint = params.at(1).toInt();
                std::vector<chain::Hash256> leaves;
                for (int i = 0; i <= checkpoint; ++i)
                    leaves.push_back(header(i).hash);
                QJsonArray branch;
                for (const chain::Hash256& step : branchOf(leaves, size_t(height)))
                    branch.append(hexOf(step));
                if (lieInBranch)
                    branch[0] = hexOf(hashOf("0000000000000000000000000000000000000000000000000000000000000001"));
                answer(peer, id, QJsonObject{{QStringLiteral("branch"), branch},
                                             {QStringLiteral("header"), hexOf(chain::serializeHeader(header(height)))},
                                             {QStringLiteral("root"), hexOf(rootOf(leaves))}});
            }
        } else if (method == QLatin1String("blockchain.block.headers")) {
            const int from = params.at(0).toInt();
            const int count = qMin(params.at(1).toInt(), 2016);
            if (refuseChunks) {
                refuse(peer, id);
                return;
            }
            chain::Bytes all;
            int given = 0;
            chain::Hash256 previous = from > 0 && from <= blocks_.size() ? blocks_.at(from - 1).header.hash : chain::Hash256{};
            for (int height = from; !emptyChunks && height < blocks_.size() && given < count; ++height, ++given) {
                chain::BlockHeader one = blocks_.at(height).header;
                const bool wrongBits = wrongBitsAt >= 0 && height >= wrongBitsAt;
                if (badWork || wrongBits) {
                    // Each follows the last, so that only what is being tried can refuse them. The
                    // work is what cannot be faked, and the rule what work cannot buy.
                    one.previous = previous;
                    if (wrongBits)
                        one.bits = 0x207ffffe;
                    for (one.nonce = 0;; ++one.nonce) {
                        chain::parseHeader(chain::serializeHeader(one), &one);
                        if (chain::headerHasWork(one, kEasyBits) == wrongBits)
                            break;
                    }
                }
                previous = one.hash;
                const chain::Bytes raw = chain::serializeHeader(one);
                all.insert(all.end(), raw.begin(), raw.end());
            }
            answer(peer, id, QJsonObject{{QStringLiteral("count"), given}, {QStringLiteral("hex"), hexOf(all)}, {QStringLiteral("max"), 2016}});
        } else if (method == QLatin1String("blockchain.headers.subscribe")) {
            answer(peer, id, QJsonObject{{QStringLiteral("height"), tip()}, {QStringLiteral("hex"), hexOf(chain::serializeHeader(header(tip())))}});
        } else if (method == QLatin1String("server.ping")) {
            answer(peer, id, QJsonValue());
        } else if (method == QLatin1String("blockchain.scripthash.subscribe")) {
            const QString scripthash = params.at(0).toString();
            subscribed_[peer].insert(scripthash);
            if (unread.contains(scripthash))
                refuse(peer, id);
            else
                answer(peer, id, statusOfHere(scripthash));
        } else if (method == QLatin1String("blockchain.scripthash.unsubscribe")) {
            subscribed_[peer].remove(params.at(0).toString());
            answer(peer, id, true);
        } else if (method == QLatin1String("blockchain.scripthash.get_history")) {
            if (unread.contains(params.at(0).toString())) {
                refuse(peer, id);
                return;
            }
            QJsonArray list = historyHere(params.at(0).toString());
            if (listLeavesOut && !list.isEmpty())
                list.removeFirst();
            answer(peer, id, list);
        } else if (method == QLatin1String("blockchain.scripthash.get_balance")) {
            const qint64 balance = hideAll ? 0 : balanceOf(params.at(0).toString());
            answer(peer, id, QJsonObject{{QStringLiteral("confirmed"), balance}, {QStringLiteral("unconfirmed"), 0}});
        } else if (method == QLatin1String("blockchain.transaction.get")) {
            chain::Hash256 txid{};
            chain::hashFromHex(params.at(0).toString().toStdString(), &txid);
            const auto reply = [this, peer, id, txid] {
                const auto found = txs_.constFind(txid);
                if (refuseRaw || refuseRawOf.contains(txid) || found == txs_.cend()) {
                    refuse(peer, id);
                } else if (lieInRaw) {
                    answer(peer, id, hexOf(rawTransaction({{hashOf("0000000000000000000000000000000000000000000000000000000000000002"), 0}}, {{{0x51}, 1}}, 12345)));
                } else {
                    answer(peer, id, hexOf(found->raw));
                }
            };
            const int delay = rawDelay.value(txid);
            if (delay > 0)
                QTimer::singleShot(delay, peer, reply);
            else
                reply();
        } else if (method == QLatin1String("blockchain.transaction.get_merkle")) {
            chain::Hash256 txid{};
            chain::hashFromHex(params.at(0).toString().toStdString(), &txid);
            const int height = params.at(1).toInt();
            if (refuseProofs || height >= blocks_.size() || !blocks_.at(height).txs.contains(txid)) {
                refuse(peer, id);
                return;
            }
            const QVector<chain::Hash256>& txs = blocks_.at(height).txs;
            const size_t index = size_t(txs.indexOf(txid));
            QJsonArray merkle;
            for (const chain::Hash256& step : branchOf(std::vector<chain::Hash256>(txs.cbegin(), txs.cend()), index))
                merkle.append(hexOf(step));
            if (lieInMerkle)
                merkle = QJsonArray{hexOf(hashOf("0000000000000000000000000000000000000000000000000000000000000003"))};
            const QJsonObject reply{{QStringLiteral("block_height"), height}, {QStringLiteral("merkle"), merkle}, {QStringLiteral("pos"), qint64(index)}};
            answer(peer, id, reply);
        } else {
            refuse(peer, id);
        }
    }
    QJsonArray historyHere(const QString& scripthash) const
    {
        QJsonArray out;
        if (hideAll)
            return out;
        for (int height = 0; height < blocks_.size(); ++height) {
            for (const chain::Hash256& txid : blocks_.at(height).txs) {
                if (touchesHere(txs_.value(txid), scripthash))
                    out.append(QJsonObject{{QStringLiteral("tx_hash"), hexOf(txid)}, {QStringLiteral("height"), height + heightLie}});
            }
        }
        for (const QPair<chain::Hash256, int>& one : invented_.value(scripthash))
            out.append(QJsonObject{{QStringLiteral("tx_hash"), hexOf(one.first)}, {QStringLiteral("height"), one.second}});
        for (const chain::Hash256& txid : mempool_) {
            const Tx tx = txs_.value(txid);
            if (!touchesHere(tx, scripthash))
                continue;
            bool parentPending = false;
            for (const chain::TxInput& input : tx.parsed.inputs)
                parentPending = parentPending || mempool_.contains(input.previous.txid);
            out.append(QJsonObject{{QStringLiteral("tx_hash"), hexOf(txid)}, {QStringLiteral("height"), parentPending ? -1 : 0}});
        }
        return out;
    }
public:
    QJsonValue statusOfHere(const QString& scripthash) const
    {
        QString text;
        return wallet::UtxoScanner::statusOfHistory(historyHere(scripthash), &text) == 1 ? QJsonValue(text) : QJsonValue();
    }
    void notifyHere(const QString& scripthash)
    {
        const QJsonObject message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("method"), QStringLiteral("blockchain.scripthash.subscribe")},
                                  {QStringLiteral("params"), QJsonArray{scripthash, statusOfHere(scripthash)}}};
        for (auto it = subscribed_.cbegin(); it != subscribed_.cend(); ++it) {
            if (it->contains(scripthash))
                send(it.key(), message);
        }
    }

private:
    static constexpr const char* kGenesisHeader =
        "0100000000000000000000000000000000000000000000000000000000000000000000003ba3edfd7a7b12b27ac72c3e67768f617fc81bc388"
        "8a51323a9fb8aa4b1e5e4a29ab5f49ffff001d1dac2b7c";

    QVector<Block> blocks_;
    QHash<chain::Hash256, Tx> txs_;
    QVector<chain::Hash256> mempool_;
    QHash<QString, QVector<QPair<chain::Hash256, int>>> invented_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QHash<QTcpSocket*, QSet<QString>> subscribed_;
    uint32_t nextNonce_ = 0;
    int nextForeign_ = 0;
    uint32_t nextTime_ = 1700000000;
    QTcpServer server_; // goes last: its sockets say they are gone to what is above
};

struct AddressOf {
    chain::Bytes script;
    QString scripthash;
    QString address;
};

AddressOf addressOf(const wallet::AccountKey& key, quint32 change, quint32 index)
{
    const chain::AddressInfo info = chain::addressFromChainNode(key.node().deriveChild(change), key.type, key.account, change, index,
                                                                wallet::networkInfo(key.network).format);
    return {info.scriptPubKey, QString::fromStdString(info.electrumScripthash), QString::fromStdString(info.address)};
}

// One run: a scanner and a history against servers, as a Scheduler sets them up.
struct HistoryRun {
    wallet::WalletModel* model;
    wallet::HistoryBook* book;
    wallet::UtxoScanner scanner;
    wallet::UtxoHistory history;
    QList<QString> problems;

    HistoryRun(wallet::WalletModel* rows, wallet::HistoryBook* into, const wallet::ChainProofs& proofs, const QStringList& servers, const wallet::AccountKey& key, int receiveGap = 5, int changeGap = 5)
        : model(rows), book(into), scanner(key.network, rows), history(key.network, scanner.client(), rows, into)
    {
        scanner.usePlainTcpForTest();
        history.setProofsForTest(proofs);
        history.setBalanceOnItsWay([this](const QString& scripthash) { return scanner.balanceOnItsWay(scripthash); });
        wallet::ScanSettings settings;
        settings.utxoReceiveGap = receiveGap;
        settings.utxoChangeGap = changeGap;
        settings.servers.insert(wallet::networkInfo(key.network).id, servers);
        scanner.setAccounts({key});
        scanner.setSettings(settings);
        QObject::connect(&history, &wallet::UtxoHistory::problem, &history, [this](wallet::Problem, const QString& message, const QString&) { problems.append(message); });
        history.prices()->useTestServer(QStringLiteral("http://127.0.0.1:1")); // nothing listens: no price, and no real call
        scanner.start();
        history.start();
    }
    ~HistoryRun()
    {
        history.stop();
        scanner.stop();
    }
    bool settled() const
    {
        return scanner.answered() && !history.progress().working();
    }
};

void testChainUnits(const Check& check, const wallet::AccountKey& key)
{
    // Real transactions, as servers give them.
    chain::Transaction tx;
    const chain::Bytes first = bytesOf(QStringLiteral(
        "0100000001c997a5e56e104102fa209c6a852dd90660a20b2d9c352423edce25857fcd3704000000004847304402204e45e16932b8af514961a1d3a1a25fdf3f4f7732e9d624c6c61548ab5fb8cd410220181522ec8eca07de4860a4acdd12909d831cc56cbbac4622082221a8768d1d0901ffffffff0200ca9a3b00000000434104ae1a62fe09c5f51b13905f07f06b99a2f7159b2225f374cd378d71302fa28414e7aab37397f554a7df5f142c21c1b7303b8a0626f1baded5c72a704f7e6cd84cac00286bee0000000043410411db93e1dcdb8a016b49840f8c53bc1eb68a382e97b1482ecad7b148a6909a5cb2e0eaddfb84ccf9744464f82e160bfa9b8b64f9d4c03f999b8643f656b412a3ac00000000"));
    check(chain::parseTransaction(first, &tx) && hexOf(tx.txid) == QLatin1String("f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16"),
          QStringLiteral("the first payment between people hashes to its id"));
    check(tx.inputs.size() == 1 && tx.outputs.size() == 2 && tx.outputs[0].value == 1000000000 && tx.outputs[1].value == 4000000000 && tx.vsize == 275,
          QStringLiteral("with its 10 and 40 coins"));
    const chain::Bytes witness = bytesOf(QStringLiteral(
        "0200000000010140d43a99926d43eb0e619bf0b3d83b4a31f60c176beecfb9d35bf45e54d0f7420100000017160014a4b4ca48de0b3fffc15404a1acdc8dbaae226955ffffffff0100e1f5050000000017a9144a1154d50b03292b3024370901711946cb7cccc387024830450221008604ef8f6d8afa892dee0f31259b6ce02dd70c545cfcfed8148179971876c54a022076d771d6e91bed212783c9b06e0de600fab2d518fad6f15a2b191d7fbd262a3e0121039d25ab79f41f75ceaf882411fd41fa670a4c672c23ffaf0e361a969cde0692e800000000"));
    check(chain::parseTransaction(witness, &tx) && hexOf(tx.txid) == QLatin1String("c586389e5e4b3acb9d6c8be1c19ae8ab2795397633176f5a6442a261bbdefc3a"),
          QStringLiteral("one with witness data hashes without it"));
    check(tx.size == int(witness.size()) && tx.vsize < tx.size && tx.strippedSize < tx.size && tx.outputs[0].value == 100000000 && !tx.coinbase(),
          QStringLiteral("and costs less than its bytes"));
    check(QString::fromStdString(chain::addressFromScript(tx.outputs[0].script, wallet::networkInfo(key.network).format)) == QLatin1String("38Segwituno6sUoEkh57ycM6K7ej5gvJhM"),
          QStringLiteral("and what it pays reads as the address explorers show"));

    // Whatever is not one transaction is none.
    chain::Bytes cut = first;
    cut.pop_back();
    check(!chain::parseTransaction(cut, &tx), QStringLiteral("a transaction with its end cut off is refused"));
    chain::Bytes extra = first;
    extra.push_back(0);
    check(!chain::parseTransaction(extra, &tx), QStringLiteral("so is one with a byte too many"));
    check(!chain::parseTransaction(bytesOf(QStringLiteral("0100000000")), &tx), QStringLiteral("and five bytes that look like the start of one"));
    chain::Bytes noWitness = bytesOf(QStringLiteral("02000000000101"));
    noWitness.resize(noWitness.size() + 36 + 1 + 4, 0);
    noWitness.push_back(1);
    noWitness.resize(noWitness.size() + 8 + 1, 0);
    noWitness.push_back(0); // an empty witness: nothing marks the form for
    noWitness.resize(noWitness.size() + 4, 0);
    check(!chain::parseTransaction(noWitness, &tx), QStringLiteral("and one in the witness form that carries none"));
    chain::Bytes huge = bytesOf(QStringLiteral("01000000fdffff"));
    huge.resize(100, 0);
    check(!chain::parseTransaction(huge, &tx), QStringLiteral("a count larger than the bytes left is no reason to reserve room"));
    // What no block can hold is no transaction, whatever it hashes to.
    const chain::Hash256 spentTwice = hashOf("00000000000000000000000000000000000000000000000000000000000000e1");
    check(!chain::parseTransaction(rawTransaction({{spentTwice, 0}, {spentTwice, 0}}, {{{0x51}, 1}}, 1), &tx), QStringLiteral("nor does one that spends the same coin twice count"));
    check(chain::parseTransaction(rawTransaction({{spentTwice, 0}, {spentTwice, 1}}, {{{0x51}, 1}}, 1), &tx), QStringLiteral("two outputs of one transaction are another thing"));
    QVector<chain::Bytes> heavy(100000, chain::Bytes{0x51, 0x52});
    check(!chain::parseTransaction(rawOfMany({{spentTwice, 0}}, heavy, 1), &tx), QStringLiteral("and neither does one heavier than a block"));
    heavy.resize(80000);
    check(chain::parseTransaction(rawOfMany({{spentTwice, 0}}, heavy, 1), &tx) && tx.outputs.size() == 80000, QStringLiteral("though a very long one fits"));

    // A coinbase spends nothing, and a replaceable one says so.
    tx = {};
    chain::parseTransaction(rawTransaction({{chain::Hash256{}, 0xffffffff}}, {{{0x51}, 50}}, 1), &tx);
    check(tx.coinbase(), QStringLiteral("newly mined coins come from nothing"));
    chain::parseTransaction(rawTransaction({{hashOf("0000000000000000000000000000000000000000000000000000000000000009"), 0}}, {{{0x51}, 50}}, 1, 0xfffffffd), &tx);
    check(tx.signalsReplaceable() && !tx.coinbase(), QStringLiteral("and a transaction can say it may be replaced"));

    // What a script reads as, against what a key's address is: every type there is.
    const wallet::NetworkInfo& bitcoin = wallet::networkInfo(key.network);
    for (const AddressOf& one : {addressOf(key, 0, 0), addressOf(key, 1, 3)}) {
        check(QString::fromStdString(chain::addressFromScript(one.script, bitcoin.format)) == one.address, QStringLiteral("a native SegWit script reads as its address"));
        check(QString::fromStdString(chain::scripthashOfScript(one.script)) == one.scripthash, QStringLiteral("and its hash is what a server knows it by"));
    }
    const chain::Bytes legacy = bytesOf(QStringLiteral("76a914751e76e8199196d454941c45d1b3a323f1433bd688ac"));
    check(QString::fromStdString(chain::addressFromScript(legacy, bitcoin.format)) == QLatin1String("1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH"), QStringLiteral("so does a legacy script"));
    const chain::Bytes wsh = bytesOf(QStringLiteral("00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262"));
    check(QString::fromStdString(chain::addressFromScript(wsh, bitcoin.format)) == QLatin1String("bc1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qccfmv3"), QStringLiteral("a script's witness program (BIP173)"));
    const chain::Bytes taproot = bytesOf(QStringLiteral("5120a60869f0dbcf1dc659c9cecbaf8050135ea9e8cdc487053f1dc6880949dc684c"));
    check(QString::fromStdString(chain::addressFromScript(taproot, bitcoin.format)) == QLatin1String("bc1p5cyxnuxmeuwuvkwfem96lqzszd02n6xdcjrs20cac6yqjjwudpxqkedrcr"), QStringLiteral("a Taproot key (BIP86)"));
    check(chain::addressFromScript(bytesOf(QStringLiteral("6a0401020304")), bitcoin.format).empty(), QStringLiteral("and data carries no address"));

    // A real block's header and a real proof.
    chain::BlockHeader header;
    const chain::Bytes block170 = bytesOf(QStringLiteral(
        "0100000055bd840a78798ad0da853f68974f3d183e2bd1db6a842c1feecf222a00000000ff104ccb05421ab93e63f8c3ce5c2c2e9dbb37de2764b3a3175c8166562cac7d51b96a49ffff001d283e9e70"));
    check(chain::parseHeader(block170, &header) && hexOf(header.hash) == QLatin1String("00000000d1145790a8694403d4063f323d499e655c83426834d4ce2f8dd4a2ee"), QStringLiteral("a block's header hashes to its name"));
    check(chain::serializeHeader(header) == block170, QStringLiteral("and goes back to its 80 bytes"));
    check(chain::headerHasWork(header, 0x1d00ffff), QStringLiteral("it has the work its difficulty asks for"));
    chain::BlockHeader lazy = header;
    lazy.nonce ^= 1;
    chain::parseHeader(chain::serializeHeader(lazy), &lazy);
    check(!chain::headerHasWork(lazy, 0x1d00ffff), QStringLiteral("and with another nonce it has not"));
    check(!chain::headerHasWork(header, 0x1c00ffff), QStringLiteral("nor does a difficulty below what the chain allows count"));
    chain::BlockHeader signedBits = header;
    signedBits.bits = 0x1d80ffff;
    check(!chain::headerHasWork(signedBits, 0x1d00ffff), QStringLiteral("and bits with their sign set name no target"));
    check(chain::merkleRootOf(hashOf("f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16"),
                              {hashOf("b1fea52486ce0c62bb442b530a3f0132b826c74e473d1f2c220bfa78111c5082")}, 1) == header.merkleRoot,
          QStringLiteral("the proof of a real transaction leads to the root in its block"));
    check(chain::merkleRootOf(hashOf("f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16"),
                              {hashOf("b1fea52486ce0c62bb442b530a3f0132b826c74e473d1f2c220bfa78111c5082")}, 0) != header.merkleRoot,
          QStringLiteral("and from another position it does not"));
    check(chain::proofLeadsTo(hashOf("f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16"),
                              {hashOf("b1fea52486ce0c62bb442b530a3f0132b826c74e473d1f2c220bfa78111c5082")}, 1, header.merkleRoot),
          QStringLiteral("and that is what a proof comes down to"));
    // A real transaction of 64 bytes: its halves read as two hashes, and the second half as the id of
    // what no block holds, "proven" by a branch through it into the same root.
    const chain::Bytes sixtyFour = rawTransaction({{spentTwice, 0}}, {{{0x51, 0x02, 0x4e, 0x73}, 1000}}, 7);
    chain::Transaction small;
    check(sixtyFour.size() == 64 && chain::parseTransaction(sixtyFour, &small) && small.strippedSize == 64, QStringLiteral("64 bytes make a transaction, if a very small one"));
    chain::Hash256 firstHalf{};
    chain::Hash256 secondHalf{};
    std::reverse_copy(sixtyFour.begin(), sixtyFour.begin() + 32, firstHalf.begin());
    std::reverse_copy(sixtyFour.begin() + 32, sixtyFour.end(), secondHalf.begin());
    check(chain::merkleRootOf(secondHalf, {firstHalf}, 1) == small.txid, QStringLiteral("whose id two such hashes hash to, as if they were children of a node"));
    check(!chain::proofLeadsTo(secondHalf, {firstHalf}, 1, small.txid), QStringLiteral("and a branch through it proves nothing"));

    // The difficulty rule, by Bitcoin Core's own test cases, and the last period before the checkpoint.
    check(chain::nextBits(0x1d00ffff, 1261130161, 1262152739, 0x1d00ffff, 1209600) == 0x1d00d86a, QStringLiteral("the difficulty rule gives what Bitcoin Core does"));
    check(chain::nextBits(0x1d00ffff, 1231006505, 1233061996, 0x1d00ffff, 1209600) == 0x1d00ffff, QStringLiteral("it stops at the easiest there is"));
    check(chain::nextBits(0x1c05a3f4, 1279008237, 1279297671, 0x1d00ffff, 1209600) == 0x1c0168fd, QStringLiteral("a period that went too fast counts as a quarter"));
    check(chain::nextBits(0x1c387f6f, 1263163443, 1269211443, 0x1d00ffff, 1209600) == 0x1d00e1fd, QStringLiteral("one that went too slowly as four times"));
    const wallet::ChainProofs& real = bitcoin.proofs;
    check(real.nextBits != 0 && chain::nextBits(real.checkpointBits, real.periodStart, real.checkpointTime, real.powLimit, real.targetSeconds) == real.nextBits,
          QStringLiteral("and the block after the checkpoint in the list of coins has what the rule gives"));

    // Prices: each moment by the nearest answer, and none from too far.
    const QByteArray body = R"({"coins":{"coingecko:bitcoin":{"symbol":"BTC","prices":[
        {"timestamp":1499990400,"price":2228.5,"confidence":0.99},{"timestamp":1600000000,"price":10000,"confidence":0.99},
        {"timestamp":1650000000,"price":-1},{"timestamp":1650000100,"price":40000,"confidence":0.1}]}}})";
    const net::PriceHistory::Answers answers = net::PriceHistory::read(body, QStringLiteral("coingecko:bitcoin"), {1500000000, 1600000500, 1300000000, 1650000050});
    check(answers.value(1500000000).first == 2228.5 && answers.value(1500000000).second == 1499990400, QStringLiteral("a price from the moment nearest the one asked"));
    check(answers.value(1600000500).first == 10000, QStringLiteral("whichever side of it"));
    check(!answers.contains(1300000000), QStringLiteral("none from further than a day"));
    check(!answers.contains(1650000050), QStringLiteral("none that is negative, or that the source itself is not sure of"));
    check(net::PriceHistory::read("not json", QStringLiteral("coingecko:bitcoin"), {1}).isEmpty(), QStringLiteral("and nothing from what cannot be read"));
}

// What transactions did to addresses cannot depend on the order they arrive in.
void testBook(const Check& check, const wallet::AccountKey& key)
{
    const auto spendable = [] { return hashOf("00000000000000000000000000000000000000000000000000000000000000ee"); };
    const QVector<AddressOf> ours{addressOf(key, 0, 0), addressOf(key, 0, 1), addressOf(key, 0, 2), addressOf(key, 1, 0)};
    const auto isOurs = [&ours](const QString& scripthash) {
        return std::any_of(ours.cbegin(), ours.cend(), [&scripthash](const AddressOf& a) { return a.scripthash == scripthash; });
    };
    const chain::Bytes foreignScript{0x51};

    // A chain of payments: from outside to 0, 0 to 1 with change to 3, 1 and 3 to 2, 2 to outside.
    QVector<chain::Transaction> txs;
    const auto make = [&txs](const QVector<chain::OutPoint>& in, const QVector<QPair<chain::Bytes, qint64>>& out) {
        chain::Transaction tx;
        chain::parseTransaction(rawTransaction(in, out, uint32_t(txs.size() + 1)), &tx);
        txs.append(tx);
        return tx.txid;
    };
    const chain::Hash256 a = make({{hashOf("00000000000000000000000000000000000000000000000000000000000000aa"), 0}}, {{ours[0].script, 100000}, {foreignScript, 5}});
    const chain::Hash256 b = make({{a, 0}}, {{ours[1].script, 60000}, {ours[3].script, 39000}});
    const chain::Hash256 c = make({{b, 0}, {b, 1}}, {{ours[2].script, 98000}});
    const chain::Hash256 d = make({{c, 0}}, {{foreignScript, 97000}});

    wallet::HistoryBook reference(key.network);
    for (const chain::Transaction& tx : std::as_const(txs))
        reference.add(tx, 100, 1000, isOurs);
    check(reference.netOf(ours[0].scripthash) == 0 && reference.netOf(ours[1].scripthash) == 0 && reference.netOf(ours[2].scripthash) == 0
              && reference.netOf(ours[3].scripthash) == 0,
          QStringLiteral("what came in and all went out again adds up to nothing on each address"));
    check(reference.at(reference.find(b)).fee == 1000 && reference.at(reference.find(c)).fee == 1000 && reference.at(reference.find(d)).fee == 1000,
          QStringLiteral("what wholly spends the wallets' own coins knows its fee"));
    check(reference.at(reference.find(a)).fee == -1, QStringLiteral("what spends someone else's does not"));

    std::mt19937 shuffle(12345);
    bool sameEffects = true;
    bool sameFees = true;
    for (int round = 0; round < 24; ++round) {
        QVector<chain::Transaction> order = txs;
        std::shuffle(order.begin(), order.end(), shuffle);
        wallet::HistoryBook book(key.network);
        for (const chain::Transaction& tx : std::as_const(order))
            book.add(tx, 100, 1000, isOurs);
        for (const chain::Transaction& tx : std::as_const(txs)) {
            const int here = book.find(tx.txid);
            const int there = reference.find(tx.txid);
            sameFees = sameFees && book.at(here).fee == reference.at(there).fee;
            const auto spelled = [](const wallet::HistoryBook& from, int position) {
                QMap<QString, QPair<qint64, qint64>> out;
                for (const wallet::TxEffect& effect : from.effectsOf(position))
                    out.insert(from.addressAt(effect.address).scripthash, {effect.received, effect.spent});
                return out;
            };
            sameEffects = sameEffects && spelled(book, here) == spelled(reference, there);
        }
    }
    check(sameEffects, QStringLiteral("in whatever order they arrive, each did the same to each address"));
    check(sameFees, QStringLiteral("and knows the same fee"));

    // Read again it changes nothing, and an address known later picks up what it was paid.
    wallet::HistoryBook late(key.network);
    QSet<QString> known{ours[0].scripthash};
    const auto knownOnly = [&known](const QString& scripthash) { return known.contains(scripthash); };
    late.add(txs[1], 100, 1000, knownOnly);
    late.add(txs[0], 100, 1000, knownOnly);
    check(late.netOf(ours[0].scripthash) == 0 && late.netOf(ours[1].scripthash) == 0, QStringLiteral("a transaction read before its address was known leaves that address out"));
    known.insert(ours[1].scripthash);
    late.add(txs[1], 100, 1000, knownOnly);
    check(late.netOf(ours[1].scripthash) == 60000 && late.size() == 2, QStringLiteral("and picks it up when it is read again"));
    late.add(txs[1], 100, 1000, knownOnly);
    check(late.netOf(ours[1].scripthash) == 60000, QStringLiteral("and a third reading adds nothing"));

    // Replaced: the same coins, spent by something else.
    wallet::HistoryBook rbf(key.network);
    const auto spend = [&](uint32_t nonce, qint64 value) {
        chain::Transaction tx;
        chain::parseTransaction(rawTransaction({{a, 0}}, {{foreignScript, value}}, nonce), &tx);
        return tx;
    };
    rbf.add(txs[0], 100, 1000, isOurs);
    const chain::Transaction first = spend(50, 90000);
    const chain::Transaction second = spend(51, 80000);
    const int firstAt = rbf.add(first, 0, 2000, isOurs);
    check(rbf.at(firstAt).seen == wallet::TxSeen::Alive && rbf.at(firstAt).fee == 10000 && rbf.netOf(ours[0].scripthash) == 0, QStringLiteral("a pending payment counts while it stands"));
    rbf.markGone(firstAt);
    check(rbf.at(firstAt).seen == wallet::TxSeen::Dropped && rbf.netOf(ours[0].scripthash) == 100000, QStringLiteral("gone from a server's list, and nothing known in its place, it is dropped and counts no more"));
    const int secondAt = rbf.add(second, 0, 3000, isOurs);
    check(rbf.at(firstAt).seen == wallet::TxSeen::Replaced && rbf.at(firstAt).replacedBy == second.txid, QStringLiteral("what then spends the same coins is what replaced it"));
    check(rbf.at(secondAt).seen == wallet::TxSeen::Alive && rbf.netOf(ours[0].scripthash) == 0, QStringLiteral("and only that counts"));
    rbf.setHeight(firstAt, 0);
    check(rbf.at(firstAt).seen == wallet::TxSeen::Alive && rbf.at(firstAt).replacedBy == chain::Hash256{}, QStringLiteral("listed again, it stands again"));
    rbf.setHeight(secondAt, 200);
    check(rbf.at(firstAt).seen == wallet::TxSeen::Alive, QStringLiteral("a server's word that a block holds something else takes nothing from it"));
    rbf.setProven(secondAt, true, 0);
    check(rbf.at(firstAt).seen == wallet::TxSeen::Replaced && rbf.at(firstAt).replacedBy == second.txid, QStringLiteral("and what a block is proven to hold beats what is pending"));
    rbf.setHeight(firstAt, 0);
    check(rbf.at(firstAt).seen == wallet::TxSeen::Replaced, QStringLiteral("even if a list then names it again"));
    const chain::Transaction third = spend(52, 70000);
    const int thirdAt = rbf.add(third, 0, 4000, isOurs);
    check(rbf.at(thirdAt).seen == wallet::TxSeen::Replaced && rbf.netOf(ours[0].scripthash) == 0, QStringLiteral("and a new one that spends the same coins is taken for what it is"));

    // Counted as it goes, whatever it goes through.
    {
        const auto counted = [](const wallet::HistoryBook& book) {
            int pending = 0;
            int lines = 0;
            for (int i = 0; i < book.size(); ++i) {
                pending += book.at(i).seen == wallet::TxSeen::Alive && book.at(i).height <= 0 ? 1 : 0;
                lines += book.effectsOf(i).isEmpty() ? 0 : 1;
            }
            return book.pendingCount() == pending && book.lineCount() == lines;
        };
        wallet::HistoryBook book(key.network);
        const chain::Transaction stranger = [&] {
            chain::Transaction tx;
            chain::parseTransaction(rawTransaction({{hashOf("00000000000000000000000000000000000000000000000000000000000000ab"), 0}}, {{foreignScript, 5}}, 90), &tx);
            return tx;
        }();
        bool holds = true;
        for (const chain::Transaction& tx : std::as_const(txs)) {
            book.add(tx, 0, 1000, isOurs);
            holds = holds && counted(book);
        }
        const int strangerAt = book.add(stranger, 0, 1000, isOurs);
        check(book.effectsOf(strangerAt).isEmpty() && book.lineCount() == txs.size(), QStringLiteral("what touches no address gets no line"));
        check(counted(book) && holds, QStringLiteral("what is pending and what has a line are counted right as they come"));
        book.setHeight(book.find(a), 300);
        book.setHeight(book.find(b), -1);
        check(counted(book), QStringLiteral("as they move"));
        book.markGone(book.find(c));
        check(counted(book), QStringLiteral("as they go"));
        book.setHeight(book.find(c), 0);
        check(counted(book), QStringLiteral("as they come back"));
        book.describeAddress(ours[0].scripthash, 1, ours[0].address);
        for (int i = 1; i < ours.size(); ++i)
            book.describeAddress(ours[i].scripthash, 2, ours[i].address);
        const wallet::HistoryDelta written = book.takeDelta();
        check(std::none_of(written.txs.cbegin(), written.txs.cend(), [&stranger](const wallet::TxRecord& tx) { return tx.txid == stranger.txid; }) && written.txs.size() == txs.size(),
              QStringLiteral("what touches no address is never written down"));
        book.forgetWallet(1);
        check(counted(book), QStringLiteral("and when a wallet takes some along"));
    }

    // A write that failed: what it held goes again, as far as it still stands.
    {
        wallet::HistoryBook book(key.network);
        book.add(txs[0], 100, 1000, isOurs);
        book.add(txs[1], 100, 1000, isOurs);
        book.describeAddress(ours[0].scripthash, 1, ours[0].address);
        book.describeAddress(ours[1].scripthash, 2, ours[1].address);
        book.describeAddress(ours[3].scripthash, 2, ours[3].address);
        book.setStatus(ours[1].scripthash, QStringLiteral("aa"));
        book.addHeader(700, QByteArray(80, 'h'));
        book.dropHeadersFrom(800);
        book.takeDelta();
        book.setStatus(ours[0].scripthash, QStringLiteral("bb"));
        book.addHeader(701, QByteArray(80, 'i'));
        book.dropHeadersFrom(900);
        book.forgetWallet(1);
        const wallet::HistoryDelta failed = book.takeDelta();
        check(failed.removedTxs.contains(a) && failed.removedAddresses.contains(ours[0].scripthash) && failed.dropHeadersFrom == 900 && failed.headers.contains(701),
              QStringLiteral("a write holds a removal, and headers"));
        check(book.takeDelta().empty(), QStringLiteral("and the book has let go of them"));
        book.takeBack(failed);
        const wallet::HistoryDelta again = book.takeDelta();
        check(again.removedTxs.contains(a) && again.removedAddresses.contains(ours[0].scripthash) && again.dropHeadersFrom == 900 && again.headers.contains(701),
              QStringLiteral("it failed: the removal goes again, unlike with all of it written over"));
        book.setStatus(ours[0].scripthash, QStringLiteral("cc"));
        book.takeBack(failed);
        const wallet::HistoryDelta newer = book.takeDelta();
        check(!newer.removedAddresses.contains(ours[0].scripthash), QStringLiteral("except what has come back since"));
        // A note written since a removal is newer than it.
        book.takeBack(failed);
        book.setNote(a, {QStringLiteral("from a backup"), QString()});
        const wallet::HistoryDelta withNote = book.takeDelta();
        check(withNote.removedTxs.contains(a) && !withNote.removedNotes.contains(a), QStringLiteral("and a note written since a removal stays"));
    }

    // Long ones. Not on this thread for as long as the square of its outputs.
    {
        QVector<chain::Bytes> scripts;
        QSet<QString> many;
        scripts.reserve(80000);
        for (int i = 0; i < 80000; ++i) {
            const chain::Bytes script{uint8_t(i), uint8_t(i >> 8), uint8_t(i >> 16)};
            scripts.append(script);
            many.insert(QString::fromStdString(chain::scripthashOfScript(script)));
        }
        chain::Transaction longOne;
        check(chain::parseTransaction(rawOfMany({{spendable(), 0}}, scripts, 3), &longOne), QStringLiteral("a transaction with 80,000 outputs"));
        wallet::HistoryBook book(key.network);
        QElapsedTimer clock;
        clock.start();
        const int at = book.add(longOne, 0, 1000, [&many](const QString& scripthash) { return many.contains(scripthash); });
        const qint64 first = clock.elapsed();
        clock.restart();
        book.add(longOne, 0, 1000, [&many](const QString& scripthash) { return many.contains(scripthash); });
        check(first < 4000 && clock.elapsed() < 2000, QStringLiteral("all paying the wallets' addresses, read in %1 ms and again in %2 ms").arg(first).arg(clock.elapsed()));
        check(book.effectsOf(at).size() == 80000 && book.hasEffectOn(at, *many.cbegin()), QStringLiteral("every one of them counted, once"));
    }

    // What was replaced or dropped long ago goes, unless it has a note or is spent.
    {
        wallet::HistoryBook book(key.network);
        book.add(txs[0], 100, 1000, isOurs);
        const chain::Transaction old = spend(60, 90000);
        const chain::Transaction noted = spend(61, 90000);
        const chain::Transaction recent = spend(62, 90000);
        const int oldAt = book.add(old, 0, 1000, isOurs);
        const int notedAt = book.add(noted, 0, 1000, isOurs);
        const int recentAt = book.add(recent, 0, 5000, isOurs);
        book.markGone(oldAt);
        book.markGone(notedAt);
        book.markGone(recentAt);
        book.setNote(noted.txid, {QStringLiteral("kept"), QString()});
        book.takeDelta();
        book.forgetGoneBefore(2000);
        check(book.find(old.txid) < 0, QStringLiteral("what went long ago leaves the list"));
        check(book.find(noted.txid) >= 0 && book.find(recent.txid) >= 0 && book.find(a) >= 0, QStringLiteral("what has a note stays, what went lately, and what stands"));
        check(book.takeDelta().removedTxs.contains(old.txid), QStringLiteral("and the database is told"));
        check(book.netOf(ours[0].scripthash) == 100000, QStringLiteral("and what is left adds up as it did"));
    }

    // Labels and notes, in a file.
    {
        wallet::LabelLine tx;
        tx.txid = a;
        tx.network = QStringLiteral("bitcoin");
        tx.label = QStringLiteral("Order \"17\"");
        tx.note = QStringLiteral("two\nlines");
        wallet::LabelLine address;
        address.isTransaction = false;
        address.address = ours[0].address;
        address.label = QStringLiteral("Till ș");
        QByteArray file = wallet::writeLabelLine(tx) + wallet::writeLabelLine(address);
        check(file.count('\n') == 2 && file.contains("\"type\":\"tx\"") && file.contains("\"type\":\"addr\""), QStringLiteral("labels go to a file, a line each, by the kinds other wallets know"));
        file += "not json\n\n{\"type\":\"pubkey\",\"ref\":\"02aa\",\"label\":\"a kind not used here\"}\n{\"type\":\"tx\",\"ref\":\"short\",\"label\":\"no such id\"}\n{\"type\":\"addr\",\"ref\":\"bc1q\",\"label\":\"\"}\n";
        int skipped = 0;
        const QVector<wallet::LabelLine> read = wallet::readLabelLines(file, &skipped);
        check(read.size() == 2 && skipped == 4, QStringLiteral("what cannot be read is counted and left out, and the rest comes back"));
        check(read.size() == 2 && read[0].txid == a && read[0].label == tx.label && read[0].note == tx.note && read[0].network == tx.network, QStringLiteral("a transaction's whole"));
        check(read.size() == 2 && !read[1].isTransaction && read[1].address == address.address && read[1].label == address.label, QStringLiteral("an address's whole"));
    }

    // Going by wallets.
    wallet::HistoryBook two(key.network);
    for (const chain::Transaction& tx : std::as_const(txs))
        two.add(tx, 100, 1000, isOurs);
    // Still there to spend when another wallet has gone.
    const chain::Transaction stillThere = [&] {
        chain::Transaction tx;
        chain::parseTransaction(rawTransaction({{hashOf("00000000000000000000000000000000000000000000000000000000000000ac"), 0}}, {{ours[2].script, 7000}}, 76), &tx);
        return tx;
    }();
    two.add(stillThere, 100, 1000, isOurs);
    two.setProven(two.find(a), true, 3);
    two.describeAddress(ours[0].scripthash, 1, ours[0].address);
    two.describeAddress(ours[1].scripthash, 2, ours[1].address);
    two.describeAddress(ours[2].scripthash, 2, ours[2].address);
    two.describeAddress(ours[3].scripthash, 2, ours[3].address);
    two.setStatus(ours[0].scripthash, QStringLiteral("aa"));
    two.setStatus(ours[1].scripthash, QStringLiteral("bb"));
    two.setNote(a, {QStringLiteral("only wallet one"), QString()});
    two.setNote(b, {QStringLiteral("both"), QString()});
    two.takeDelta();
    two.forgetWallet(1);
    check(two.find(a) < 0 && two.noteOf(a).isEmpty(), QStringLiteral("a removed wallet takes along what touched only it, and the notes on it"));
    check(two.find(b) >= 0 && two.noteOf(b).label == QLatin1String("both") && two.netOf(ours[1].scripthash) == 0, QStringLiteral("and leaves what also touches another"));
    check(two.statusOf(ours[0].scripthash).isEmpty() && two.statusOf(ours[1].scripthash) == QLatin1String("bb"), QStringLiteral("its addresses' statuses go too"));
    const wallet::HistoryDelta gone = two.takeDelta();
    check(gone.removedTxs.contains(a) && gone.removedAddresses.contains(ours[0].scripthash), QStringLiteral("and the database is told"));
    two.keepWallets({2});
    check(two.find(b) >= 0, QStringLiteral("the wallets still there keep theirs"));
    // What is left goes on as if nothing had gone.
    check(two.netOf(ours[2].scripthash) == 7000, QStringLiteral("what is left adds up as it did"));
    const chain::Transaction spendsKept = [&] {
        chain::Transaction tx;
        chain::parseTransaction(rawTransaction({{stillThere.txid, 0}}, {{foreignScript, 6000}}, 77), &tx);
        return tx;
    }();
    const int spendsAt = two.add(spendsKept, 100, 1000, isOurs);
    check(two.netOf(ours[2].scripthash) == 0 && two.hasEffectOn(spendsAt, ours[2].scripthash) && two.at(spendsAt).fee == 1000, QStringLiteral("and what spends it afterwards still finds what it spends"));
    const chain::Hash256 notedBefore = stillThere.txid;
    two.setNote(notedBefore, {QStringLiteral("on a wallet that is about to go missing"), QString()});
    two.takeDelta();
    two.keepWallets({9});
    check(two.size() == 0, QStringLiteral("those no longer in the file do not"));
    // Nobody removed that one: a wallet.dat from before it was added comes to the same.
    check(two.noteOf(notedBefore).label.startsWith(QLatin1String("on a wallet")), QStringLiteral("though what only this PC has stays, for when it is back"));
    const wallet::HistoryDelta afterMissing = two.takeDelta();
    check(afterMissing.removedTxs.contains(notedBefore) && !afterMissing.removedNotes.contains(notedBefore), QStringLiteral("in the file too"));

    // One this wallet.dat gave out and no longer has was removed, and that only did not get as far as the file.
    {
        wallet::HistoryBook book(key.network);
        chain::Transaction tx;
        chain::parseTransaction(rawTransaction({{hashOf("00000000000000000000000000000000000000000000000000000000000000b0"), 0}}, {{ours[0].script, 1000}}, 83), &tx);
        book.add(tx, 100, 1000, isOurs);
        book.describeAddress(ours[0].scripthash, 3, ours[0].address);
        book.setNote(tx.txid, {QStringLiteral("rent"), QString()});
        book.takeDelta();
        book.keepWallets({7}, 5);
        check(book.find(tx.txid) < 0 && book.noteOf(tx.txid).isEmpty(), QStringLiteral("a removal that did not reach the file finishes at a start, notes and all"));
        check(book.takeDelta().removedNotes.contains(tx.txid), QStringLiteral("in the file too"));
    }

    // What has no effect yet may be another wallet's, on its way.
    {
        wallet::HistoryBook book(key.network);
        book.add(txs[0], 100, 1000, isOurs);
        const chain::Transaction waiting = [&] {
            chain::Transaction tx;
            chain::parseTransaction(rawTransaction({{hashOf("00000000000000000000000000000000000000000000000000000000000000ad"), 0}}, {{foreignScript, 5}}, 78), &tx);
            return tx;
        }();
        const int waitingAt = book.add(waiting, 100, 1000, isOurs);
        check(book.effectsOf(waitingAt).isEmpty(), QStringLiteral("a transaction read before what made its coins touches nothing yet"));
        book.setNote(waiting.txid, {QStringLiteral("rent"), QString()});
        book.describeAddress(ours[0].scripthash, 1, ours[0].address);
        book.forgetWallet(1);
        check(book.find(waiting.txid) >= 0 && book.noteOf(waiting.txid).label == QLatin1String("rent"), QStringLiteral("and stays, with its note, when another wallet goes"));
    }
    // The fee of what also spent from a wallet that goes is no more known than without that wallet.
    {
        wallet::HistoryBook book(key.network);
        chain::Transaction fromOne;
        chain::Transaction fromTwo;
        chain::Transaction spendsBoth;
        chain::parseTransaction(rawTransaction({{hashOf("00000000000000000000000000000000000000000000000000000000000000ae"), 0}}, {{ours[0].script, 1000}}, 80), &fromOne);
        chain::parseTransaction(rawTransaction({{hashOf("00000000000000000000000000000000000000000000000000000000000000af"), 0}}, {{ours[1].script, 1000}}, 81), &fromTwo);
        chain::parseTransaction(rawTransaction({{fromOne.txid, 0}, {fromTwo.txid, 0}}, {{foreignScript, 1900}}, 82), &spendsBoth);
        book.add(fromOne, 100, 1000, isOurs);
        book.add(fromTwo, 100, 1000, isOurs);
        const int at = book.add(spendsBoth, 100, 1000, isOurs);
        check(book.at(at).fee == 100, QStringLiteral("what spends from two wallets knows its fee"));
        book.describeAddress(ours[0].scripthash, 1, ours[0].address);
        book.describeAddress(ours[1].scripthash, 2, ours[1].address);
        book.forgetWallet(1);
        check(book.at(book.find(spendsBoth.txid)).fee == -1, QStringLiteral("and with one of them gone, knows it no more"));
    }
}

void testFlow(const Check& check, const wallet::AccountKey& key)
{
    FakeChain server;
    const wallet::ChainProofs proofs = server.proofs(10); // heights below it by branch, past it by work
    const AddressOf first = addressOf(key, 0, 0);
    const AddressOf second = addressOf(key, 0, 1);
    const AddressOf change = addressOf(key, 1, 0);
    const chain::Bytes foreign{0x51};

    // A payment before the wallet looks, in an old block below the checkpoint: mined at height 5 of
    // a chain made for it.
    FakeChain old(4);
    const chain::Hash256 oldPay = old.fund(first.script, 1000);
    old.mine();
    for (int i = 0; i < 10; ++i)
        old.mine();
    wallet::WalletModel oldModel;
    wallet::HistoryBook oldBook(key.network);
    {
        HistoryRun run(&oldModel, &oldBook, old.proofs(10), {old.line()}, key);
        check(waitFor([&run, &oldBook] { return run.settled() && oldBook.size() == 1; }, 15000), QStringLiteral("a payment from before the wallet looked is read"));
        const int at = oldBook.find(oldPay);
        check(at >= 0 && waitFor([&oldBook, at] { return oldBook.at(at).proven; }, 10000), QStringLiteral("and proven, below the checkpoint, by a branch to it"));
        check(at >= 0 && oldBook.at(at).height == 5 && oldBook.at(at).time == old.header(5).time && oldBook.at(at).blockPos == 0, QStringLiteral("with its block's time and its place there"));
        check(old.calls.value(QStringLiteral("blockchain.block.header")) >= 2, QStringLiteral("after asking for that header"));
    }

    wallet::WalletModel model;
    wallet::HistoryBook book(key.network);
    HistoryRun run(&model, &book, proofs, {server.line()}, key);
    check(waitFor([&run] { return run.settled(); }, 15000), QStringLiteral("nothing at first"));
    check(book.size() == 0 && run.history.tip() == server.tip(), QStringLiteral("and the chain is followed past the checkpoint"));

    // Pending.
    const chain::Hash256 paid = server.pay({{hashOf("00000000000000000000000000000000000000000000000000000000000000bb"), 0}}, {{first.script, 50000}, {foreign, 7}}, 0xfffffffd);
    server.notifyHere(first.scripthash);
    check(waitFor([&book, &paid] { return book.find(paid) >= 0; }, 10000), QStringLiteral("a payment shows while it is pending"));
    const int paidAt = book.find(paid);
    check(paidAt >= 0 && book.at(paidAt).height == 0 && !book.at(paidAt).proven && book.at(paidAt).replaceable && book.at(paidAt).time > 0,
          QStringLiteral("not in a block, not proven, marked as replaceable, dated when first seen"));
    check(book.netOf(first.scripthash) == 50000, QStringLiteral("for what it pays"));

    // In a block.
    server.mine();
    server.notifyTip();
    server.notifyHere(first.scripthash);
    check(waitFor([&book, paidAt] { return book.at(paidAt).proven; }, 10000), QStringLiteral("in a block, its proof is checked"));
    check(book.at(paidAt).height == server.tip() && book.at(paidAt).time == server.header(server.tip()).time, QStringLiteral("and it goes by its block's height and time"));
    check(waitFor([&run, &server] { return run.history.tip() == server.tip(); }, 5000), QStringLiteral("and the chain has taken that block"));

    // Sent, with change.
    const chain::Hash256 sent = server.pay({{paid, 0}}, {{foreign, 30000}, {change.script, 19000}});
    server.notifyHere(first.scripthash);
    server.notifyHere(change.scripthash);
    check(waitFor([&book, &sent, &change] { return book.find(sent) >= 0 && book.hasEffectOn(book.find(sent), change.scripthash); }, 10000), QStringLiteral("what is spent shows on both addresses"));
    const int sentAt = book.find(sent);
    check(book.at(sentAt).fee == 1000 && book.at(sentAt).height == 0, QStringLiteral("with its fee, as all it spends is the wallets'"));
    check(waitFor([&book, &first, &change] { return book.netOf(first.scripthash) == 0 && book.netOf(change.scripthash) == 19000; }, 5000), QStringLiteral("and what each address has left"));

    // Replaced: the mempool's version gives way.
    const chain::Hash256 better = server.pay({{paid, 0}}, {{foreign, 29000}, {change.script, 19000}});
    server.forget(sent);
    server.notifyHere(first.scripthash);
    server.notifyHere(change.scripthash);
    check(waitFor([&book, &better, sentAt] { return book.find(better) >= 0 && book.at(sentAt).seen == wallet::TxSeen::Replaced; }, 10000), QStringLiteral("a replacement shows, and what it replaced says so"));
    check(book.at(sentAt).replacedBy == better, QStringLiteral("by which"));
    check(book.netOf(first.scripthash) == 0 && book.netOf(change.scripthash) == 19000, QStringLiteral("and is counted once"));

    // Into a block, then the block gives way.
    server.mine();
    server.notifyTip();
    server.notifyHere(first.scripthash);
    server.notifyHere(change.scripthash);
    const int betterAt = book.find(better);
    check(waitFor([&book, betterAt] { return book.at(betterAt).proven; }, 10000), QStringLiteral("the replacement gets into a block"));
    const int heightBefore = book.at(betterAt).height;
    server.reorganise(1, {better});
    server.notifyTip();
    server.notifyHere(first.scripthash);
    server.notifyHere(change.scripthash);
    check(waitFor([&book, betterAt, heightBefore] { return book.at(betterAt).height > heightBefore && book.at(betterAt).proven; }, 15000),
          QStringLiteral("that block gives way: the chain follows, and it is proven at its new height"));
    check(book.at(betterAt).time == server.header(book.at(betterAt).height).time, QStringLiteral("with its new block's time"));
    check(run.history.tip() == server.tip(), QStringLiteral("and the chain stands where the server's does"));

    // An address the wallet only finds further along.
    const AddressOf far = addressOf(key, 0, 8); // past the gap of 5 until the one before it is used
    const AddressOf near = addressOf(key, 0, 4);
    const chain::Hash256 both = server.pay({{hashOf("00000000000000000000000000000000000000000000000000000000000000cc"), 0}}, {{near.script, 100}, {far.script, 200}});
    server.mine();
    server.notifyTip();
    server.notifyHere(near.scripthash);
    check(waitFor([&book, &both, &far] { return book.find(both) >= 0 && book.netOf(far.scripthash) == 200; }, 15000),
          QStringLiteral("what also paid an address found only because of it counts for that address too"));

    // Every balance adds up.
    check(waitFor([&run] { return run.settled(); }, 15000), QStringLiteral("then all is quiet"));
    bool adds = true;
    for (int row = 0; row < model.size(); ++row) {
        const wallet::AddressRow& r = model.at(row);
        if (r.status.isEmpty() && r.confirmed == 0)
            continue;
        adds = adds && book.netOf(r.scripthash) == r.confirmed + r.unconfirmed;
    }
    check(adds, QStringLiteral("and on every address the transactions add up to the balance"));
    pause(5500);
    check(run.history.progress().differing == 0, QStringLiteral("which is what the page goes by"));
    Q_UNUSED(second);
}

void testLiars(const Check& check, const wallet::AccountKey& key)
{
    const AddressOf first = addressOf(key, 0, 0);
    const auto chainWithPayment = [&first](FakeChain* server, chain::Hash256* txid) {
        *txid = server->fund(first.script, 7000);
        server->mine();
    };

    // Two honest chains, the same blocks: the same fund transaction on the same heights.
    const auto twoServers = [&](const std::function<void(FakeChain*)>& spoil, const QString& what, const std::function<bool(const wallet::HistoryBook&, const chain::Hash256&)>& done, int cp = 10, int retarget = 0) {
        FakeChain liar;
        chain::Hash256 txid{};
        chainWithPayment(&liar, &txid);
        spoil(&liar);
        FakeChain honest;
        chain::Hash256 same{};
        chainWithPayment(&honest, &same);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, honest.proofs(cp, retarget), {liar.line(), honest.line()}, key);
        const bool settled = waitFor([&book, &txid, &done] { return done(book, txid); }, 20000);
        check(settled, what);
        check(honest.connections > 0, QStringLiteral("after moving to the next server"));
        return run.problems.size();
    };
    const auto provenThere = [](const wallet::HistoryBook& book, const chain::Hash256& txid) {
        const int at = book.find(txid);
        return at >= 0 && book.at(at).proven;
    };

    int said = twoServers([](FakeChain* s) { s->lieInRaw = true; }, QStringLiteral("a transaction that does not hash to its id goes no further"), provenThere);
    check(said > 0, QStringLiteral("and the window says a server was left"));
    said = twoServers([](FakeChain* s) { s->lieInMerkle = true; }, QStringLiteral("a proof that leads nowhere is not believed"), provenThere);
    check(said > 0, QStringLiteral("and the window says so"));
    said = twoServers([](FakeChain* s) { s->badWork = true; }, QStringLiteral("headers that follow each other, without the work, are not believed"), provenThere);
    check(said > 0, QStringLiteral("and the window says so"));
    said = twoServers([](FakeChain* s) { s->wrongBitsAt = 13; }, QStringLiteral("nor, with their work, at a difficulty that does not follow"), provenThere);
    check(said > 0, QStringLiteral("and the window says so"));
    said = twoServers([](FakeChain* s) { s->wrongBitsAt = 15; }, QStringLiteral("nor where the difficulty changes, if it is not what the rule gives"), provenThere, 10, 5);
    check(said > 0, QStringLiteral("and the window says so"));
    said = twoServers([](FakeChain* s) { s->heightLie = 100000; }, QStringLiteral("a transaction in a block a server does not have goes no further"), provenThere);
    check(said > 0, QStringLiteral("and the window says so"));
    wallet::UtxoHistory::setFirstPauseForTest(300);
    said = twoServers([](FakeChain* s) { s->listLeavesOut = true; }, QStringLiteral("a list that is not what its status says is not believed, and the next server reads it"), provenThere);
    check(said > 0, QStringLiteral("and the window says so"));
    said = twoServers([](FakeChain* s) { s->refuseBranch = true; }, QStringLiteral("a server that will not give a header below the checkpoint is left for one that does"), provenThere, 25);
    said = twoServers([](FakeChain* s) { s->refuseChunks = true; }, QStringLiteral("so is one that will not give headers past it"), provenThere);
    said = twoServers([](FakeChain* s) { s->emptyChunks = true; }, QStringLiteral("or says it has more, and gives none"), provenThere);
    said = twoServers([](FakeChain* s) { s->refuseProofs = true; }, QStringLiteral("or will not give a proof"), provenThere);
    wallet::UtxoHistory::setFirstPauseForTest(0);

    {
        // No other server to turn to: nothing is believed, and it says so.
        FakeChain liar;
        chain::Hash256 txid{};
        chainWithPayment(&liar, &txid);
        liar.lieInMerkle = true;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, liar.proofs(10), {liar.line()}, key);
        check(waitFor([&run] { return run.history.progress().unproven > 0; }, 15000), QStringLiteral("with no other server the proof stays unbelieved, and the page can say so"));
        check(book.find(txid) >= 0 && !book.at(book.find(txid)).proven && book.netOf(first.scripthash) == 7000, QStringLiteral("the transaction shows, as a server's word"));
        check(run.history.proofFailed(txid), QStringLiteral("and its line can say which it is"));
        check(waitFor([&run] { return !run.problems.isEmpty(); }, 15000) && run.problems.first().contains(QStringLiteral("no other server")), QStringLiteral("and it does not claim to have left a server it stays on"));
        // It says what the transaction would be worth: and a balance that rests on it does not add up.
        check(waitFor([&run] { return run.history.progress().differing > 0; }, 15000), QStringLiteral("a balance that rests on a proof that failed does not add up"));
    }
    {
        // A proof that holds is not taken back by a list: a server has no say over what this PC checked.
        FakeChain honest;
        chain::Hash256 txid{};
        chainWithPayment(&honest, &txid);
        FakeChain second;
        chain::Hash256 same{};
        chainWithPayment(&second, &same);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, honest.proofs(10), {honest.line(), second.line()}, key);
        const int at = [&] {
            waitFor([&book, &txid] { return book.find(txid) >= 0 && book.at(book.find(txid)).proven; }, 15000);
            return book.find(txid);
        }();
        check(at >= 0 && book.at(at).proven, QStringLiteral("a transaction is proven"));
        honest.hideAll = true;
        honest.notifyHere(first.scripthash);
        check(waitFor([&run, &second] { return second.connections > 0 || !run.problems.isEmpty(); }, 15000), QStringLiteral("a server then says it never was: it is left"));
        check(book.at(at).seen == wallet::TxSeen::Alive && book.at(at).proven && book.netOf(first.scripthash) == 7000, QStringLiteral("and the transaction stands, with its proof"));
        check(waitFor([&run] { return run.settled() && run.history.progress().refused == 0; }, 15000), QStringLiteral("and the next server has it"));
    }
    {
        // What is proven stays so, when the services start again.
        FakeChain ahead;
        chain::Hash256 txid{};
        chainWithPayment(&ahead, &txid);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, ahead.proofs(10), {ahead.line()}, key);
        check(waitFor([&book, &txid] { return book.find(txid) >= 0 && book.at(book.find(txid)).proven; }, 15000), QStringLiteral("a proof holds"));
        run.history.stop();
        run.history.start();
        check(book.at(book.find(txid)).proven && run.history.progress().proofsLeft == 0, QStringLiteral("and is not asked for again at the next start"));
    }
    {
        // A height another server made up is not the next one's to answer for.
        FakeChain liar;
        chain::Hash256 txid{};
        chainWithPayment(&liar, &txid);
        liar.heightLie = 100000;
        FakeChain second;
        chain::Hash256 same{};
        chainWithPayment(&second, &same);
        FakeChain third;
        chain::Hash256 also{};
        chainWithPayment(&third, &also);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, second.proofs(10), {liar.line(), second.line(), third.line()}, key);
        check(waitFor([&book, &same] { return book.find(same) >= 0 && book.at(book.find(same)).proven; }, 20000), QStringLiteral("a server lies about a block: the next proves it where it is"));
        pause(1500);
        check(third.connections == 0 && run.problems.size() == 1, QStringLiteral("without the honest one being blamed for that (%1 said)").arg(run.problems.size()));
    }
    {
        // What every server refuses does not use up what it takes to leave one that lies.
        const AddressOf other = addressOf(key, 0, 1);
        FakeChain one;
        FakeChain two;
        const chain::Hash256 refused = one.fund(first.script, 1000);
        two.fund(first.script, 1000);
        one.mine();
        two.mine();
        one.refuseRawOf.insert(refused);
        two.refuseRawOf.insert(refused);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        wallet::UtxoHistory::setFirstPauseForTest(200);
        HistoryRun run(&model, &book, one.proofs(10), {one.line(), two.line()}, key);
        check(waitFor([&two, &run] { return two.connections > 0 && run.history.progress().refused > 0 && !run.history.progress().working(); }, 15000), QStringLiteral("no server gives a transaction, and the next has been tried"));
        const chain::Hash256 paid = two.fund(other.script, 500);
        one.fund(other.script, 500);
        two.mine();
        one.mine();
        two.lieInMerkle = true;
        const int before = one.connections;
        two.notifyTip();
        two.notifyHere(other.scripthash);
        check(waitFor([&one, before] { return one.connections > before; }, 15000), QStringLiteral("and when the one it is on then lies, it can still be left"));
        check(waitFor([&book, &paid] { return book.find(paid) >= 0 && book.at(book.find(paid)).proven; }, 15000), QStringLiteral("for what the other can prove"));
        wallet::UtxoHistory::setFirstPauseForTest(0);
    }
    {
        // Back from refusing: a proof that came late clears what did not add up, and what was refused
        // is asked for again when a block comes, with no other server to go to.
        const AddressOf other = addressOf(key, 0, 1);
        FakeChain only;
        chain::Hash256 txid{};
        chainWithPayment(&only, &txid);
        const chain::Hash256 refused = only.fund(other.script, 500);
        only.mine();
        only.refuseProofs = true;
        only.refuseRawOf.insert(refused);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        wallet::UtxoHistory::setFirstPauseForTest(200);
        HistoryRun run(&model, &book, only.proofs(10), {only.line()}, key);
        check(waitFor([&run] { const wallet::HistoryProgress p = run.history.progress(); return p.unproven > 0 && p.differing > 0 && p.refused > 0; }, 25000), QStringLiteral("a server gives no proof and a raw transaction: nothing counts on them, and it says so"));
        only.refuseProofs = false;
        only.refuseRawOf.clear();
        only.mine();
        only.notifyTip();
        check(waitFor([&run, &book, &refused] { const wallet::HistoryProgress p = run.history.progress(); return p.unproven == 0 && p.differing == 0 && p.refused == 0 && book.find(refused) >= 0; }, 25000),
              QStringLiteral("and when it gives them, with the next block, all of that clears"));
        wallet::UtxoHistory::setFirstPauseForTest(0);
    }
    {
        // Asked for by one that says it is no longer in that block.
        FakeChain only;
        chain::Hash256 txid{};
        chainWithPayment(&only, &txid);
        only.lieInMerkle = true;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        wallet::UtxoHistory::setFirstPauseForTest(200);
        HistoryRun run(&model, &book, only.proofs(10), {only.line()}, key);
        check(waitFor([&run] { return run.history.progress().unproven > 0; }, 15000), QStringLiteral("a proof fails"));
        only.lieInMerkle = false;
        only.reorganise(1, {});
        // A list and no block: a block would ask again for what failed, whatever the list said.
        only.notifyHere(first.scripthash);
        check(waitFor([&run, &book, &txid] { const int at = book.find(txid); return at >= 0 && book.at(at).height <= 0 && run.history.progress().unproven == 0; }, 15000),
              QStringLiteral("then a list says it is pending: there is no proof to fail any more"));
        wallet::UtxoHistory::setFirstPauseForTest(0);
    }
    {
        // A server that refuses headers for many blocks at once: more than a try asks again.
        FakeChain refuser(4);
        FakeChain honest(4);
        QVector<chain::Hash256> paid;
        for (int i = 0; i < 230; ++i) {
            refuser.fund(first.script, 1000 + i);
            refuser.mine();
            paid.append(honest.fund(first.script, 1000 + i));
            honest.mine();
        }
        refuser.refuseBranch = true;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        // Long enough for more to be refused than a try asks again.
        wallet::UtxoHistory::setFirstPauseForTest(8000);
        HistoryRun run(&model, &book, honest.proofs(honest.tip()), {refuser.line(), honest.line()}, key);
        check(waitFor([&honest] { return honest.connections > 0; }, 30000), QStringLiteral("a server refusing a great many headers is still left"));
        check(refuser.calls.value(QStringLiteral("blockchain.block.header")) > 200, QStringLiteral("when it has refused more than a try asks again (%1)").arg(refuser.calls.value(QStringLiteral("blockchain.block.header"))));
        check(waitFor([&run, &book] { return run.settled() && book.size() == 230; }, 40000), QStringLiteral("for one that gives them"));
        wallet::UtxoHistory::setFirstPauseForTest(0);
    }
    {
        // A first read: what lists name is read as it goes, not after every list.
        FakeChain server;
        QVector<AddressOf> addresses;
        for (quint32 i = 0; i < 60; ++i) {
            addresses.append(addressOf(key, 0, i));
            server.fund(addresses.last().script, 1000 + int(i));
        }
        server.mine();
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, server.proofs(10), {server.line()}, key, 80);
        const auto lists = [&server] { return server.calls.value(QStringLiteral("blockchain.scripthash.get_history")); };
        const auto raws = [&server] { return server.calls.value(QStringLiteral("blockchain.transaction.get")); };
        const bool listsCame = waitFor([&lists] { return lists() >= 20; }, 15000);
        check(listsCame && raws() > 0, QStringLiteral("lines come as the lists do (%1 lists, %2 raw transactions)").arg(lists()).arg(raws()));
        check(waitFor([&run, &book] { return run.settled() && book.size() == 60; }, 20000), QStringLiteral("and all of them in the end"));
    }
    {
        // An address read in rounds, and a block that gives way: nobody is left for that.
        const int places = 1;
        wallet::UtxoScanner::setLivePlacesForTest(places);
        wallet::UtxoScanner::setPacesForTest(1000, 0, 0);
        FakeChain server;
        FakeChain second;
        const AddressOf far = addressOf(key, 0, 2);
        const chain::Hash256 txid = server.fund(far.script, 7000);
        second.fund(far.script, 7000);
        server.mine();
        second.mine();
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        wallet::UtxoHistory::setFirstPauseForTest(200);
        {
            HistoryRun run(&model, &book, server.proofs(10), {server.line(), second.line()}, key);
            check(waitFor([&book, &txid] { return book.find(txid) >= 0 && book.at(book.find(txid)).proven; }, 20000), QStringLiteral("a payment on an address read in rounds is proven"));
            const int before = book.at(book.find(txid)).height;
            server.reorganise(1, {});
            for (int i = 0; i < 3; ++i)
                server.mine();
            server.notifyTip();
            check(waitFor([&book, &txid, before] { const int at = book.find(txid); return at >= 0 && book.at(at).proven && book.at(at).height != before; }, 20000),
                  QStringLiteral("its block gives way: it is read where it now is, at once"));
            check(second.connections == 0, QStringLiteral("and no server is left for telling the truth about it"));
        }
        wallet::UtxoHistory::setFirstPauseForTest(0);
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
        wallet::UtxoScanner::setPacesForTest(5 * 60 * 1000, 5000, 30 * 1000);
    }
    {
        // Rounds go on by themselves, at a wait a coarse timer would have come back from too early.
        wallet::UtxoScanner::setLivePlacesForTest(1);
        wallet::UtxoScanner::setPacesForTest(2500, 0, 0);
        FakeChain server;
        chain::Hash256 txid{};
        chainWithPayment(&server, &txid);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        {
            HistoryRun run(&model, &book, server.proofs(10), {server.line()}, key);
            const auto reads = [&server] { return server.calls.value(QStringLiteral("blockchain.scripthash.get_history")); };
            check(waitFor([&run] { return run.settled(); }, 15000), QStringLiteral("a wallet read in rounds is read"));
            const int before = reads();
            const bool goesOn = waitFor([&reads, before] { return reads() >= before + 16; }, 14000);
            check(goesOn, QStringLiteral("and goes on being read, round after round (%1 reads)").arg(reads() - before));
        }
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
        wallet::UtxoScanner::setPacesForTest(5 * 60 * 1000, 5000, 30 * 1000);
    }
    {
        // A server that is only behind is not asked for what it has not got to, and nobody is blamed.
        // It is asked when it has, with nothing else to bring that about: an address read in rounds.
        wallet::UtxoScanner::setLivePlacesForTest(1);
        wallet::UtxoScanner::setPacesForTest(5000, 0, 0);
        wallet::UtxoHistory::setFirstPauseForTest(200);
        const AddressOf far = addressOf(key, 0, 2);
        FakeChain ahead;
        FakeChain behind;
        for (int i = 0; i < 3; ++i)
            ahead.mine();
        const chain::Hash256 txid = ahead.fund(far.script, 7000);
        ahead.mine();
        ahead.refuseProofs = true;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        {
            HistoryRun run(&model, &book, ahead.proofs(10), {ahead.line(), behind.line()}, key);
            const auto proofs = [&behind] { return behind.calls.value(QStringLiteral("blockchain.transaction.get_merkle")); };
            check(waitFor([&behind, &book, &txid] { return behind.connections > 0 && book.find(txid) >= 0; }, 15000), QStringLiteral("a server that gives no proof is left for one that is blocks behind"));
            pause(300);
            check(proofs() == 0 && run.problems.isEmpty(), QStringLiteral("that one is not asked for it, and not blamed"));
            check(run.history.proofFailed(txid), QStringLiteral("and the page does not say it is proven"));
            for (int i = 0; i < 3; ++i)
                behind.mine();
            behind.fund(far.script, 7000);
            behind.mine();
            behind.notifyTip();
            check(waitFor([&book, &txid] { return book.at(book.find(txid)).proven; }, 15000) && proofs() > 0, QStringLiteral("once it has caught up it is asked, and it is proven"));
        }
        wallet::UtxoHistory::setFirstPauseForTest(0);
        wallet::UtxoScanner::setLivePlacesForTest(wallet::UtxoScanner::kMaxLiveRows);
        wallet::UtxoScanner::setPacesForTest(5 * 60 * 1000, 5000, 30 * 1000);
    }
    {
        // A lie that cannot be left is not forgotten for having been told on another connection.
        FakeChain liar;
        chain::Hash256 txid{};
        chainWithPayment(&liar, &txid);
        liar.heightLie = 100000;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, liar.proofs(10), {liar.line()}, key);
        check(waitFor([&run, &txid] { return run.history.proofFailed(txid) && run.history.progress().unproven == 1; }, 15000), QStringLiteral("the only server lists a block it does not have: the proof did not hold"));
        run.history.stop();
        run.history.start();
        pause(500);
        check(run.history.proofFailed(txid) && run.history.progress().unproven == 1, QStringLiteral("and it still has not after a new start"));
        check(run.problems.size() == 1, QStringLiteral("with no new accusation (%1 said)").arg(run.problems.size()));
        liar.mine();
        liar.notifyTip();
        pause(500);
        check(run.history.proofFailed(txid), QStringLiteral("nor after the next block"));
    }
    {
        // Asked again with every block, what the only server refuses is no reason to stop reading.
        const AddressOf other = addressOf(key, 0, 1);
        FakeChain only;
        const chain::Hash256 refused = only.fund(first.script, 1000);
        only.mine();
        only.refuseRawOf.insert(refused);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        wallet::UtxoHistory::setFirstPauseForTest(200);
        HistoryRun run(&model, &book, only.proofs(10), {only.line()}, key);
        check(waitFor([&run] { return run.history.progress().refused > 0 && !run.history.progress().working(); }, 15000), QStringLiteral("the only server will not give a transaction"));
        const int rawsBefore = only.calls.value(QStringLiteral("blockchain.transaction.get"));
        for (int i = 0; i < 16; ++i) {
            only.mine();
            only.notifyTip();
            pause(120);
        }
        check(only.calls.value(QStringLiteral("blockchain.transaction.get")) >= rawsBefore + 12, QStringLiteral("it is asked again with each block"));
        pause(2500);
        const chain::Hash256 news = only.pay({{hashOf("00000000000000000000000000000000000000000000000000000000000000d3"), 0}}, {{other.script, 500}});
        only.notifyHere(other.scripthash);
        check(waitFor([&book, &news] { return book.find(news) >= 0; }, 2000), QStringLiteral("and a payment still shows at once"));
        wallet::UtxoHistory::setFirstPauseForTest(0);
    }
    {
        // Past the limit of raw transactions waiting, a list still has its turn, with proofs waiting too.
        wallet::UtxoHistory::setMaxTxBacklogForTest(50);
        const AddressOf other = addressOf(key, 0, 1);
        FakeChain server;
        for (int i = 0; i < 400; ++i) {
            server.fund(first.script, 1000 + i);
            if (i % 100 == 99)
                server.mine();
        }
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, server.proofs(10), {server.line()}, key);
        check(waitFor([&book] { return book.size() >= 40; }, 15000), QStringLiteral("a long history is being read"));
        const chain::Hash256 news = server.pay({{hashOf("00000000000000000000000000000000000000000000000000000000000000d2"), 0}}, {{other.script, 500}});
        server.notifyHere(other.scripthash);
        const bool shown = waitFor([&book, &news] { return book.find(news) >= 0; }, 15000);
        check(shown && book.size() < 200, QStringLiteral("a payment past the limit of what waits shows ahead of it (%1 of 401)").arg(book.size()));
        wallet::UtxoHistory::setMaxTxBacklogForTest(0);
    }
    {
        // A block gives way to one that holds the same. An address nobody will read has no list to say
        // so, and the other hears nothing, its status being the same: it is proven again all the same.
        const AddressOf second = addressOf(key, 0, 1);
        FakeChain server;
        const chain::Hash256 txid = server.pay({{hashOf("00000000000000000000000000000000000000000000000000000000000000d4"), 0}}, {{first.script, 100}, {second.script, 200}});
        server.mine();
        server.unread.insert(second.scripthash);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, server.proofs(10), {server.line()}, key);
        check(waitFor([&book, &txid] { return book.find(txid) >= 0 && book.at(book.find(txid)).proven; }, 15000), QStringLiteral("what pays an address nobody will read is proven"));
        const auto proofs = [&server] { return server.calls.value(QStringLiteral("blockchain.transaction.get_merkle")); };
        const int before = proofs();
        server.reorganiseInPlace();
        server.notifyTip();
        check(waitFor([&book, &txid, &proofs, before, &run, &server] { return run.history.tip() == server.tip() && proofs() > before && book.at(book.find(txid)).proven; }, 15000),
              QStringLiteral("its block gives way to one that holds it too: it is proven there"));
    }
    {
        // What a proof took down has that proof alone against it: with the proof gone, it is itself again.
        const AddressOf x = addressOf(key, 0, 1);
        const AddressOf y = addressOf(key, 0, 2);
        const chain::OutPoint coin{hashOf("00000000000000000000000000000000000000000000000000000000000000d5"), 0};
        FakeChain server;
        const chain::Hash256 rival = server.pay({coin}, {{x.script, 5000}});
        server.mine();
        const chain::Hash256 pending = server.pay({coin}, {{y.script, 4900}});
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, server.proofs(10), {server.line()}, key);
        check(waitFor([&book, &rival, &pending] { return book.find(rival) >= 0 && book.at(book.find(rival)).proven && book.find(pending) >= 0 && book.at(book.find(pending)).seen == wallet::TxSeen::Replaced; }, 15000),
              QStringLiteral("what a block is proven to hold takes down what is pending on the same coin"));
        server.reorganiseAway();
        server.notifyTip();
        server.notifyHere(x.scripthash);
        check(waitFor([&book, &pending] { return book.at(book.find(pending)).seen == wallet::TxSeen::Alive; }, 15000) && book.netOf(y.scripthash) == 4900,
              QStringLiteral("its block gives way, and what it took down is back, with nothing to say so but its own list"));
    }
    {
        // A list not what the status says may be ahead of a notice on its way. For a while that is all it is.
        FakeChain late;
        chain::Hash256 txid{};
        chainWithPayment(&late, &txid);
        FakeChain second;
        chain::Hash256 same{};
        chainWithPayment(&second, &same);
        late.listLeavesOut = true;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        wallet::UtxoHistory::setFirstPauseForTest(5000);
        HistoryRun run(&model, &book, second.proofs(10), {late.line(), second.line()}, key);
        const auto lists = [&late] { return late.calls.value(QStringLiteral("blockchain.scripthash.get_history")); };
        check(waitFor([&lists] { return lists() >= 3; }, 20000), QStringLiteral("a list that is not what its status says is asked for again"));
        check(second.connections == 0 && run.problems.isEmpty(), QStringLiteral("and asked twice over, nobody is blamed yet"));
        late.listLeavesOut = false;
        check(waitFor([&book, &txid] { return book.find(txid) >= 0 && book.at(book.find(txid)).proven; }, 15000) && second.connections == 0, QStringLiteral("and then it comes right, on the same server"));
        wallet::UtxoHistory::setFirstPauseForTest(0);
    }
    {
        // A block has not come, and the line went down meanwhile: that says nothing of the server.
        FakeChain only;
        chain::Hash256 txid{};
        chainWithPayment(&only, &txid);
        only.heightLie = 2;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        wallet::UtxoHistory::setFirstPauseForTest(1500);
        HistoryRun run(&model, &book, only.proofs(10), {only.line()}, key);
        check(waitFor([&book, &txid, &run] { return book.find(txid) >= 0 && run.history.progress().proofsLeft > 0; }, 2500), QStringLiteral("a transaction waits for a block its server has yet to give"));
        only.dropPeers(true);
        pause(4000);
        check(run.problems.isEmpty(), QStringLiteral("the line goes down meanwhile, and that is no lie"));
        wallet::UtxoHistory::setFirstPauseForTest(0);
    }
    {
        // A retry on its way is not all well, whatever else finishes meanwhile: or it goes round for ever.
        const AddressOf other = addressOf(key, 0, 1);
        FakeChain one;
        FakeChain two;
        const chain::Hash256 refused = one.fund(first.script, 1000);
        two.fund(first.script, 1000);
        one.mine();
        two.mine();
        two.pay({{hashOf("00000000000000000000000000000000000000000000000000000000000000d6"), 0}}, {{other.script, 500}}); // read at each move, and quickly
        for (FakeChain* server : {&one, &two}) {
            server->refuseRawOf.insert(refused);
            server->rawDelay.insert(refused, 300);
        }
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        wallet::UtxoHistory::setFirstPauseForTest(200);
        HistoryRun run(&model, &book, one.proofs(10), {one.line(), two.line()}, key);
        check(waitFor([&two] { return two.connections > 0; }, 15000), QStringLiteral("what a server refuses sends it to the next"));
        pause(5000);
        check(one.connections + two.connections <= 2, QStringLiteral("and that is where it ends, whatever else was read in between (%1 connections)").arg(one.connections + two.connections));
        wallet::UtxoHistory::setFirstPauseForTest(0);
    }
    {
        // A row goes while what its list named is on its way: nobody lied.
        FakeChain server;
        chain::Hash256 txid{};
        chainWithPayment(&server, &txid);
        server.rawDelay.insert(txid, 1000);
        FakeChain second;
        chain::Hash256 same{};
        chainWithPayment(&second, &same);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, server.proofs(10), {server.line(), second.line()}, key);
        check(waitFor([&server] { return server.calls.value(QStringLiteral("blockchain.transaction.get")) > 0; }, 15000), QStringLiteral("a list named a transaction, on its way"));
        model.dropRows([&first](const wallet::AddressRow& row) { return row.scripthash != first.scripthash; });
        pause(2000);
        check(run.problems.isEmpty() && second.connections == 0, QStringLiteral("the address it was for goes meanwhile, and the server is not blamed"));
    }
    {
        // What a list names has to touch the address. Not even a server that was left can leave it behind.
        FakeChain liar;
        chain::Hash256 txid{};
        chainWithPayment(&liar, &txid);
        QVector<chain::Hash256> invented;
        for (int i = 0; i < 6; ++i)
            invented.append(liar.invent(first.scripthash, 15));
        FakeChain honest;
        chain::Hash256 same{};
        chainWithPayment(&honest, &same);
        FakeChain third;
        chain::Hash256 also{};
        chainWithPayment(&third, &also);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        {
            HistoryRun run(&model, &book, honest.proofs(10), {liar.line(), honest.line(), third.line()}, key);
            check(waitFor([&run, &book, &same] { return run.settled() && book.find(same) >= 0 && book.at(book.find(same)).proven; }, 30000), QStringLiteral("what a list names that does not touch the address goes no further"));
            check(!run.problems.isEmpty() && honest.connections > 0, QStringLiteral("and the server that named it is left"));
            pause(1500);
            check(third.connections == 0 && honest.connections == 1, QStringLiteral("for one that goes on being asked, which nothing moves on from"));
            check(run.history.progress().unproven == 0 && run.history.progress().proofsLeft == 0, QStringLiteral("and nothing goes on waiting for a proof of it"));
        }
        check(book.lineCount() == 1, QStringLiteral("and it gets no line"));
        const wallet::HistoryDelta written = book.takeDelta();
        check(std::none_of(written.txs.cbegin(), written.txs.cend(), [&invented](const wallet::TxRecord& tx) { return invented.contains(tx.txid); }), QStringLiteral("nor is it written down"));
    }
    {
        // Those that spoke the truth are not blamed for one that did not: its answers die with its connection.
        FakeChain liar;
        QVector<AddressOf> addresses;
        for (int i = 0; i < 5; ++i)
            addresses.append(addressOf(key, 0, quint32(i)));
        FakeChain honest;
        FakeChain third;
        // In blocks of their own: each is looked into by itself, and the answers come spread out.
        for (int i = 0; i < 20; ++i) {
            liar.fund(addresses.at(i % 5).script, 1000 + i);
            honest.fund(addresses.at(i % 5).script, 1000 + i);
            third.fund(addresses.at(i % 5).script, 1000 + i);
            liar.mine();
            honest.mine();
            third.mine();
        }
        liar.lieInMerkle = true;
        liar.replyDelayMs = 10;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, honest.proofs(10), {liar.line(), honest.line(), third.line()}, key);
        check(waitFor([&run, &book] { return run.settled() && book.size() == 20; }, 30000), QStringLiteral("a server's lies about proofs do not stop the next from proving them"));
        pause(800);
        check(third.connections == 0, QStringLiteral("and the honest server that answered for it is not left too"));
        check(run.problems.size() == 1, QStringLiteral("nor named (%1 said)").arg(run.problems.size()));
    }
    {
        // What every server refuses does not keep the client going round them.
        const QVector<AddressOf> addresses{addressOf(key, 0, 0), addressOf(key, 0, 1), addressOf(key, 0, 2), addressOf(key, 0, 3)};
        FakeChain one;
        FakeChain two;
        for (int i = 0; i < 12; ++i) {
            one.fund(addresses.at(i % 4).script, 1000 + i);
            two.fund(addresses.at(i % 4).script, 1000 + i);
        }
        one.mine();
        two.mine();
        one.refuseRaw = true;
        two.refuseRaw = true;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        wallet::UtxoHistory::setFirstPauseForTest(100);
        HistoryRun run(&model, &book, one.proofs(10), {one.line(), two.line()}, key);
        check(waitFor([&two] { return two.connections > 0; }, 15000), QStringLiteral("a server that refuses is left"));
        pause(5000);
        check(one.connections + two.connections <= 2, QStringLiteral("and when the next is no better that is where it ends (%1 connections)").arg(one.connections + two.connections));
        check(waitFor([&run] { return run.history.progress().refused > 0 && !run.history.progress().working(); }, 15000), QStringLiteral("and the page says what could not be read"));
        wallet::UtxoHistory::setFirstPauseForTest(0);
    }
    {
        // With no other server to go to, a chain is asked again, and what rests on it says so.
        FakeChain only;
        chain::Hash256 txid{};
        chainWithPayment(&only, &txid);
        only.refuseChunks = true;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        wallet::UtxoHistory::setFirstPauseForTest(200);
        HistoryRun run(&model, &book, only.proofs(10), {only.line()}, key);
        check(waitFor([&run] { return run.history.progress().unproven > 0 && !run.history.progress().working(); }, 15000), QStringLiteral("no headers from the only server: the page says the proof did not hold, and stops saying it is reading"));
        only.refuseChunks = false;
        only.mine();
        only.notifyTip();
        check(waitFor([&book, &txid] { return book.find(txid) >= 0 && book.at(book.find(txid)).proven; }, 15000), QStringLiteral("and it gives them later: the chain goes on"));
        wallet::UtxoHistory::setFirstPauseForTest(0);
    }
    {
        // After a bad header from the only server, the chain gets over that too.
        FakeChain only;
        chain::Hash256 txid{};
        chainWithPayment(&only, &txid);
        only.badWork = true;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        wallet::UtxoHistory::setFirstPauseForTest(200);
        HistoryRun run(&model, &book, only.proofs(10), {only.line()}, key);
        check(waitFor([&run] { return !run.problems.isEmpty(); }, 15000), QStringLiteral("a chain that does not hold, from the only server"));
        only.badWork = false;
        only.mine();
        only.notifyTip();
        check(waitFor([&run, &only] { return run.history.tip() == only.tip(); }, 15000), QStringLiteral("does not stop the chain for good when it later does"));
        wallet::UtxoHistory::setFirstPauseForTest(0);
    }
    {
        // A 64-byte transaction of the wallets' own is one like any other: no server is to blame for it.
        FakeChain server;
        const chain::Hash256 funded = server.fund(first.script, 7000);
        server.mine();
        const chain::Hash256 small = server.pay({{funded, 0}}, {{{0x51, 0x02, 0x4e, 0x73}, 6000}});
        server.mine();
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, server.proofs(10), {server.line()}, key);
        check(waitFor([&run, &book, &small] { return run.settled() && book.find(small) >= 0 && book.at(book.find(small)).proven; }, 15000), QStringLiteral("a 64-byte transaction is read and proven like any other"));
        check(run.problems.isEmpty() && book.netOf(first.scripthash) == 0, QStringLiteral("and no server gets the blame"));
    }
    {
        // What needs no proof has a limit: otherwise a server could fill this PC with it.
        FakeChain server;
        for (int i = 0; i < 6; ++i)
            server.pay({{hashOf("00000000000000000000000000000000000000000000000000000000000000d0"), uint32_t(i)}}, {{first.script, 100 + i}});
        wallet::UtxoHistory::setMaxPendingForTest(3);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, server.proofs(10), {server.line()}, key);
        check(waitFor([&run] { return !run.problems.isEmpty(); }, 15000), QStringLiteral("more pending transactions than any wallet has are not taken in"));
        check(book.pendingCount() <= 3, QStringLiteral("no more than the limit (%1)").arg(book.pendingCount()));
        wallet::UtxoHistory::setMaxPendingForTest(0);
    }
    {
        // Stopped, it has nothing to say of how far it has got.
        FakeChain server;
        chain::Hash256 txid{};
        chainWithPayment(&server, &txid);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, server.proofs(10), {server.line()}, key);
        waitFor([&run] { return run.history.progress().working(); }, 5000);
        run.history.stop();
        check(!run.history.progress().working(), QStringLiteral("a history that was stopped is not still reading"));
    }
    {
        // New payments go ahead of what is being caught up on.
        const AddressOf other = addressOf(key, 0, 1);
        FakeChain server;
        for (int i = 0; i < 300; ++i)
            server.fund(first.script, 1000 + i);
        server.mine();
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, server.proofs(10), {server.line()}, key);
        check(waitFor([&book] { return book.size() > 10; }, 15000), QStringLiteral("a long history is being read"));
        const chain::Hash256 news = server.pay({{hashOf("00000000000000000000000000000000000000000000000000000000000000d1"), 0}}, {{other.script, 500}});
        server.notifyHere(other.scripthash);
        check(waitFor([&book, &news] { return book.find(news) >= 0; }, 15000), QStringLiteral("a payment during that shows"));
        check(book.size() < 300, QStringLiteral("before the rest of it (%1 of 301)").arg(book.size()));
        check(waitFor([&run, &book] { return run.settled() && book.size() == 301; }, 30000), QStringLiteral("and the rest comes after"));
        bool dated = true;
        for (int i = 0; i < book.size(); ++i)
            dated = dated && (book.at(i).height <= 0 || book.at(i).time > 0);
        check(dated, QStringLiteral("each with its block's time"));
    }
    {
        // A header below the checkpoint with a branch that does not lead to it.
        FakeChain liar(4);
        chain::Hash256 txid{};
        chainWithPayment(&liar, &txid);
        for (int i = 0; i < 10; ++i)
            liar.mine();
        const wallet::ChainProofs proofs = liar.proofs(10);
        liar.lieInBranch = true;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, proofs, {liar.line()}, key);
        check(waitFor([&run] { return run.history.progress().unproven > 0; }, 15000), QStringLiteral("a header that does not lead to the checkpoint is not believed"));
        check(book.find(txid) >= 0 && !book.at(book.find(txid)).proven && !run.problems.isEmpty(), QStringLiteral("and neither is what rests on it"));
    }
    {
        // Another chain's checkpoint: everything past it fails at once.
        FakeChain liar;
        chain::Hash256 txid{};
        chainWithPayment(&liar, &txid);
        FakeChain other(2);
        other.fund(first.script, 1);
        other.mine();
        for (int i = 0; i < 10; ++i)
            other.mine();
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, other.proofs(10), {liar.line()}, key);
        check(waitFor([&run] { return !run.problems.isEmpty(); }, 15000), QStringLiteral("headers that do not follow the checkpoint are another chain's"));
        check(run.history.tip() == 10, QStringLiteral("and go no further than it"));
    }
    {
        // It refuses raw transactions: a pause, then the next server.
        FakeChain refuser;
        refuser.refuseRaw = true;
        QVector<chain::Hash256> many;
        const QVector<AddressOf> addresses{addressOf(key, 0, 0), addressOf(key, 0, 1), addressOf(key, 0, 2), addressOf(key, 0, 3)};
        FakeChain honest;
        for (int i = 0; i < 12; ++i) {
            refuser.fund(addresses.at(i % 4).script, 1000 + i);
            many.append(honest.fund(addresses.at(i % 4).script, 1000 + i));
        }
        refuser.mine();
        honest.mine();
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        wallet::UtxoHistory::setFirstPauseForTest(300);
        HistoryRun run(&model, &book, honest.proofs(10), {refuser.line(), honest.line()}, key);
        check(waitFor([&run, &book, &many] { return book.size() == many.size() && run.settled(); }, 30000), QStringLiteral("a server that keeps refusing transactions is left for one that gives them"));
        wallet::UtxoHistory::setFirstPauseForTest(0);
    }
    {
        // The balance says otherwise than the transactions do.
        FakeChain server;
        chain::Hash256 txid{};
        chainWithPayment(&server, &txid);
        server.balanceLie = 1;
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        {
            HistoryRun run(&model, &book, server.proofs(10), {server.line()}, key);
            check(waitFor([&run] { return run.history.progress().differing > 0; }, 15000), QStringLiteral("an address whose transactions do not add up to its balance is pointed out"));
            check(waitFor([&run] { return !run.problems.isEmpty(); }, 15000), QStringLiteral("and looked at again it is said out loud, whatever page is open"));
        }
        // And again at the next start, when nothing moves to make anyone look.
        HistoryRun again(&model, &book, server.proofs(10), {server.line()}, key);
        check(waitFor([&again] { return again.history.progress().differing > 0; }, 15000), QStringLiteral("and at the next start, with nothing moving, it is found again"));
    }
    {
        // A server that will not read an address, but says what it holds: a history is missing.
        FakeChain server;
        chain::Hash256 txid{};
        chainWithPayment(&server, &txid);
        const AddressOf second = addressOf(key, 0, 1);
        server.fund(second.script, 500);
        server.mine();
        server.unread.insert(first.scripthash);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        HistoryRun run(&model, &book, server.proofs(10), {server.line()}, key);
        check(waitFor([&run] { return run.history.progress().differing > 0; }, 25000), QStringLiteral("an address that holds coins, with no list to show for it, does not add up"));
    }
}

// A deep reorganisation, and blocks where the difficulty changes on the way.
void testChainMoves(const Check& check, const wallet::AccountKey& key)
{
    const AddressOf first = addressOf(key, 0, 0);
    FakeChain server(24);
    const wallet::ChainProofs proofs = server.proofs(10, 5);
    const chain::Hash256 txid = server.fund(first.script, 7000);
    server.mine();
    wallet::WalletModel model;
    wallet::HistoryBook book(key.network);
    HistoryRun run(&model, &book, proofs, {server.line()}, key);
    check(waitFor([&book, &txid] { return book.find(txid) >= 0 && book.at(book.find(txid)).proven; }, 15000), QStringLiteral("the difficulty rule holds where it changes, on a chain that follows it"));
    check(run.history.tip() == server.tip() && run.problems.isEmpty(), QStringLiteral("and nobody is blamed"));
    const int at = book.find(txid);
    const int before = book.at(at).height;
    // Further down than the first look goes, and longer than what it replaces.
    server.reorganise(12, {});
    for (int i = 0; i < 14; ++i)
        server.mine();
    server.notifyTip();
    server.notifyHere(first.scripthash);
    check(waitFor([&book, at, before] { return book.at(at).height != before && book.at(at).proven; }, 20000), QStringLiteral("twelve blocks give way: the chain follows, and it is proven at its new height"));
    check(run.history.tip() == server.tip() && run.problems.isEmpty(), QStringLiteral("with nothing to blame anyone for"));
}

// What a transaction was worth, from a source that answers late, or says nothing.
void testPrices(const Check& check, const wallet::AccountKey& key)
{
    const AddressOf first = addressOf(key, 0, 0);
    const AddressOf second = addressOf(key, 0, 1);
    QTcpServer source;
    source.listen(QHostAddress::LocalHost);
    int asked = 0;
    bool silent = true;
    bool holding = false;
    QList<std::function<void()>> held;
    QObject::connect(&source, &QTcpServer::newConnection, &source, [&] {
        while (QTcpSocket* peer = source.nextPendingConnection()) {
            QObject::connect(peer, &QTcpSocket::readyRead, peer, [&, peer] {
                const QByteArray request = peer->readAll();
                if (!request.contains("\r\n\r\n"))
                    return;
                ++asked;
                // Every moment it is asked for, at one price.
                const QString query = QUrl::fromPercentEncoding(request);
                QJsonArray prices;
                if (!silent) {
                    static const QRegularExpression moment(QStringLiteral("\\b(1[0-9]{9})\\b"));
                    for (auto it = moment.globalMatch(query); it.hasNext();)
                        prices.append(QJsonObject{{QStringLiteral("timestamp"), it.next().captured(1).toLongLong()}, {QStringLiteral("price"), 50000.0}, {QStringLiteral("confidence"), 0.99}});
                }
                const QByteArray body = QJsonDocument(silent ? QJsonObject{{QStringLiteral("coins"), QJsonObject{}}}
                                                             : QJsonObject{{QStringLiteral("coins"), QJsonObject{{wallet::assetInfo(wallet::networkInfo(wallet::firstOf(wallet::Family::Utxo)->network).coin).defillamaId, QJsonObject{{QStringLiteral("prices"), prices}}}}}})
                                            .toJson(QJsonDocument::Compact);
                const auto reply = [peer, body] {
                    peer->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    peer->disconnectFromHost();
                };
                if (holding)
                    held.append(reply);
                else
                    reply();
            });
        }
    });
    // Two in one block, one proven long after the other: the same moment, and each its price.
    FakeChain server;
    const chain::Hash256 one = server.fund(first.script, 7000);
    const chain::Hash256 two = server.fund(second.script, 8000);
    server.mine();
    wallet::WalletModel model;
    wallet::HistoryBook book(key.network);
    wallet::UtxoHistory::setFirstPauseForTest(300);
    {
        HistoryRun run(&model, &book, server.proofs(10), {server.line()}, key);
        run.history.prices()->useTestServer(QStringLiteral("http://127.0.0.1:%1").arg(source.serverPort()));
        check(waitFor([&book, &one, &two] { return book.find(one) >= 0 && book.at(book.find(one)).proven && book.find(two) >= 0 && book.at(book.find(two)).proven; }, 15000), QStringLiteral("two payments in one block are proven"));
        check(waitFor([&asked] { return asked > 0; }, 15000), QStringLiteral("what they were worth is asked for"));
        pause(300);
        check(book.at(book.find(one)).usdThen == 0.0, QStringLiteral("a source that says nothing gives no price"));
        silent = false;
        check(waitFor([&book, &one, &two] { return book.at(book.find(one)).usdThen > 0.0 && book.at(book.find(two)).usdThen > 0.0; }, 20000), QStringLiteral("and that is a bad hour, not the last word: it is asked again"));
        // A moment asked for and answered, with a line of that block that had no proof yet then.
        book.setUsdThen(book.find(two), 0.0, 0);
        const chain::Hash256 later = server.fund(first.script, 9000);
        server.mine();
        server.notifyTip();
        server.notifyHere(first.scripthash);
        check(waitFor([&book, &two, &later] { return book.find(later) >= 0 && book.at(book.find(later)).usdThen > 0.0 && book.at(book.find(two)).usdThen > 0.0; }, 20000),
              QStringLiteral("each line of a block has its price, whenever it was proven"));
        // A price has nothing to do with a server: one that comes across a new connection is as good.
        holding = true;
        const chain::Hash256 across = server.fund(first.script, 9500);
        server.mine();
        const int askedBefore = asked;
        server.notifyTip();
        server.notifyHere(first.scripthash);
        check(waitFor([&asked, askedBefore] { return asked > askedBefore; }, 20000), QStringLiteral("what the next was worth is asked for"));
        const int connections = server.connections;
        server.dropPeers();
        check(waitFor([&server, connections] { return server.connections > connections; }, 20000), QStringLiteral("and the line goes down and comes back before the answer does"));
        holding = false;
        for (const std::function<void()>& reply : std::as_const(held))
            reply();
        check(waitFor([&book, &across] { return book.at(book.find(across)).usdThen > 0.0; }, 5000) && asked == askedBefore + 1, QStringLiteral("and it counts, with no need to ask again"));
    }
    wallet::UtxoHistory::setFirstPauseForTest(0);
}

// What a run has read stands at the next start: nothing is asked again.
void testLedger(const Check& check, const wallet::AccountKey& key)
{
    QTemporaryDir folder;
    const QString path = folder.filePath(QStringLiteral("ledger.db"));
    const QByteArray keyBytes(32, 'L');
    FakeChain old(4);
    const AddressOf first = addressOf(key, 0, 0);
    const chain::Hash256 oldPay = old.fund(first.script, 1000);
    old.mine();
    for (int i = 0; i < 8; ++i)
        old.mine();
    const chain::Hash256 newPay = old.fund(first.script, 2000);
    old.mine();
    const chain::Hash256 pendingPay = old.pay({{newPay, 0}}, {{first.script, 1500}});
    const wallet::ChainProofs proofs = old.proofs(10);
    const int tipThen = old.tip();
    const QSet<quint32> wallets{key.wallet};

    {
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        store::Ledger ledger(&model);
        ledger.addBook(&book);
        bool loaded = false;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        ledger.open(path, keyBytes, wallets);
        check(waitFor([&loaded] { return loaded; }, 10000), QStringLiteral("a new database hands over nothing"));
        {
            HistoryRun run(&model, &book, proofs, {old.line()}, key);
            check(waitFor([&run, &book] { return run.settled() && book.size() == 3; }, 15000), QStringLiteral("a history is read"));
            check(waitFor([&book, &newPay] { return book.at(book.find(newPay)).proven; }, 10000), QStringLiteral("and proven"));
            ledger.saveAddressLabel(first.address, QStringLiteral("Till 1"));
            ledger.saveNote(key.network, oldPay, {QStringLiteral("First order"), QStringLiteral("a note")});
            book.setNote(oldPay, {QStringLiteral("First order"), QStringLiteral("a note")});
        }
        ledger.close();
    }

    const QHash<QString, int> before = old.calls;
    {
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        store::Ledger ledger(&model);
        ledger.addBook(&book);
        QHash<QString, QString> labels;
        bool loaded = false;
        // What can be edited on a line is there before the line is.
        bool labelsFirst = false;
        bool notesFirst = false;
        QObject::connect(&ledger, &store::Ledger::labelsRead, &ledger, [&](const QHash<QString, QString>& saved) {
            labels = saved;
            labelsFirst = book.size() == 0 && model.size() == 0;
            notesFirst = !book.noteOf(oldPay).isEmpty();
        });
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        ledger.open(path, keyBytes, wallets);
        check(waitFor([&loaded] { return loaded; }, 10000), QStringLiteral("at the next start the database hands over"));
        check(labelsFirst && notesFirst, QStringLiteral("its labels and notes before any line they go on"));
        check(book.size() == 3, QStringLiteral("every transaction, before any server has said anything"));
        const int at = book.find(newPay);
        check(at >= 0 && book.at(at).proven && book.at(at).time == old.header(book.at(at).height).time && book.netOf(first.scripthash) == 2500,
              QStringLiteral("each as it stood, adding up to the same"));
        check(book.at(book.find(pendingPay)).height == 0 && !book.at(book.find(pendingPay)).proven, QStringLiteral("what was pending too"));
        check(book.noteOf(oldPay).label == QLatin1String("First order") && labels.value(first.address) == QLatin1String("Till 1"), QStringLiteral("with its labels and notes"));
        check(book.headers().size() > 1, QStringLiteral("and the headers it took to prove them"));
        {
            HistoryRun run(&model, &book, proofs, {old.line()}, key);
            check(waitFor([&run] { return run.settled(); }, 15000), QStringLiteral("the servers agree"));
            pause(300);
            const auto asked = [&](const char* method) { return old.calls.value(QLatin1String(method)) - before.value(QLatin1String(method)); };
            check(asked("blockchain.transaction.get") == 0, QStringLiteral("so no transaction is read twice"));
            check(asked("blockchain.transaction.get_merkle") == 0, QStringLiteral("no proof asked for twice"));
            check(asked("blockchain.block.headers") == 0 && run.history.tip() == tipThen, QStringLiteral("and no header"));
            check(asked("blockchain.scripthash.get_history") == 0, QStringLiteral("nor any list of transactions"));
        }
        ledger.close();
    }

    // Writes that fail: a label or a note exists nowhere else, and a status never stands without
    // the transactions it says are there.
    const auto holdFile = [&path, &keyBytes]() -> sqlite3* {
        sqlite3* raw = nullptr;
        sqlite3_open_v2(path.toUtf8().constData(), &raw, SQLITE_OPEN_READWRITE, nullptr);
        const QByteArray pragma = "PRAGMA key = \"x'" + keyBytes.toHex() + "'\"";
        sqlite3_exec(raw, pragma.constData(), nullptr, nullptr, nullptr);
        sqlite3_exec(raw, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        return raw;
    };
    {
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        store::Ledger ledger(&model);
        ledger.addBook(&book);
        bool loaded = false;
        QStringList said;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        QObject::connect(&ledger, &store::Ledger::problem, &ledger, [&said](const QString& message) { said.append(message); });
        QObject::connect(&ledger, &store::Ledger::labelsProblem, &ledger, [&said](const QString& message) { said.append(message); });
        ledger.open(path, keyBytes, wallets);
        check(waitFor([&loaded] { return loaded; }, 10000), QStringLiteral("the database opens again"));
        sqlite3* holder = holdFile();
        const chain::Hash256 noted = hashOf("00000000000000000000000000000000000000000000000000000000000000c1");
        check(ledger.saveNote(key.network, noted, {QStringLiteral("Refund"), QString()}), QStringLiteral("a note goes in while another program holds the file"));
        check(ledger.saveAddressLabel(QStringLiteral("bc1qheld"), QStringLiteral("Till 2")), QStringLiteral("and a label"));
        // Half of what stands for an address, then the rest with its status: the first must not be overtaken.
        wallet::TxRecord half;
        half.network = key.network;
        half.txid = hashOf("00000000000000000000000000000000000000000000000000000000000000c2");
        wallet::OurOutput output;
        output.n = 0;
        output.value = 10;
        output.scripthash = first.scripthash;
        half.outputs.append(output);
        book.addSaved({half});
        book.setHeight(book.find(half.txid), 5);
        ledger.flush();
        pause(200);
        book.setStatus(first.scripthash, QStringLiteral("newer"));
        ledger.flush();
        check(waitFor([&said] { return said.size() >= 2; }, 30000), QStringLiteral("none of it can be written"));
        check(said.join(QLatin1Char(' ')).contains(QStringLiteral("Labels and notes")), QStringLiteral("and the window says so of what exists nowhere else, in so many words"));
        sqlite3_exec(holder, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close_v2(holder);
        pause(6000); // it is tried again
        store::LedgerDb reader;
        QString why;
        ledger.close();
        check(reader.open(path, keyBytes, &why) == store::LedgerDb::Open::Ready, QStringLiteral("and the file can be read"));
        check(reader.loadNotes(key.network, &why).value(noted).label == QLatin1String("Refund"), QStringLiteral("the note is there"));
        check(reader.loadAddressLabels(&why).value(QStringLiteral("bc1qheld")) == QLatin1String("Till 2"), QStringLiteral("the label too"));
        QVector<wallet::TxRecord> rows;
        reader.loadHistory(key.network, 100, [&rows](QVector<wallet::TxRecord>&& chunk) { rows += chunk; }, &why);
        const QVector<wallet::AddressHistory> statuses = reader.loadAddressHistory(key.network, &why);
        const auto status = std::find_if(statuses.cbegin(), statuses.cend(), [&first](const wallet::AddressHistory& one) { return one.scripthash == first.scripthash; });
        check(status != statuses.cend() && status->status == QLatin1String("newer") && std::any_of(rows.cbegin(), rows.cend(), [&half](const wallet::TxRecord& tx) { return tx.txid == half.txid; }),
              QStringLiteral("and the status went in with the transaction it stands for, not ahead of it"));
        reader.close();
    }

    // A file with a bad page: what only this PC has comes out of it.
    {
        const QString damaged = folder.filePath(QStringLiteral("damaged.db"));
        const chain::Hash256 noted = hashOf("00000000000000000000000000000000000000000000000000000000000000c3");
        {
            store::LedgerDb db;
            QString why;
            db.open(damaged, keyBytes, &why);
            db.saveNote(key.network, noted, {QStringLiteral("The only copy"), QStringLiteral("of this")}, &why);
            db.saveAddressLabel(QStringLiteral("bc1qonly"), QStringLiteral("Till 3"), &why);
            // Rows enough that the last page of the file is none of those.
            wallet::HistoryDelta delta;
            for (int n = 0; n < 2000; ++n) {
                wallet::TxRecord tx;
                tx.network = key.network;
                tx.txid[0] = uint8_t(n);
                tx.txid[1] = uint8_t(n >> 8);
                tx.inputs.resize(8);
                delta.txs.append(tx);
            }
            db.saveHistory(key.network, delta, &why);
            db.close();
        }
        flipByte(damaged, QFile(damaged).size() - 100);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        store::Ledger ledger(&model);
        ledger.addBook(&book);
        bool loaded = false;
        int recovered = -2;
        bool whole = false;
        QHash<QString, QString> labels;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        QObject::connect(&ledger, &store::Ledger::labelsRead, &ledger, [&labels](const QHash<QString, QString>& read) { labels = read; });
        QObject::connect(&ledger, &store::Ledger::setAside, &ledger, [&](const QString&, int count, bool all) {
            recovered = count;
            whole = all;
        });
        ledger.open(damaged, keyBytes, wallets);
        check(waitFor([&loaded] { return loaded; }, 15000) && ledger.isOpen(), QStringLiteral("a file with a bad page is set aside"));
        check(recovered == 2 && whole, QStringLiteral("its labels and notes come along, and it says so (%1)").arg(recovered));
        check(book.noteOf(noted).label == QLatin1String("The only copy") && book.noteOf(noted).note == QLatin1String("of this"), QStringLiteral("the note, whole"));
        check(labels.value(QStringLiteral("bc1qonly")) == QLatin1String("Till 3"), QStringLiteral("the label"));
        ledger.close();
        store::LedgerDb reader;
        QString why;
        check(reader.open(damaged, keyBytes, &why) == store::LedgerDb::Open::Ready && reader.loadNotes(key.network, &why).contains(noted), QStringLiteral("and into the new file, for the next start"));
        reader.close();
        const store::SavedLabels another = store::LedgerDb::salvageLabels(path, QByteArray(32, 'x'));
        check(!another.read && another.count() == 0, QStringLiteral("under another key a file gives nothing, and says it could not read it"));
    }
    // A bad page among notes: what lies beyond it is read from the other end.
    {
        const QString many = folder.filePath(QStringLiteral("many-notes.db"));
        constexpr int kNotes = 6000;
        {
            store::LedgerDb db;
            QString why;
            db.open(many, keyBytes, &why);
            QHash<chain::Hash256, wallet::TxNote> notes;
            for (int n = 0; n < kNotes; ++n) {
                chain::Hash256 txid{};
                txid[0] = uint8_t(n);
                txid[1] = uint8_t(n >> 8);
                notes.insert(txid, {QStringLiteral("note %1").arg(n), QStringLiteral("so that a page holds few of these")});
            }
            db.saveNotes(key.network, notes, &why);
            db.close();
        }
        flipByte(many, QFile(many).size() / 2);
        const store::SavedLabels saved = store::LedgerDb::salvageLabels(many, keyBytes);
        check(saved.read && !saved.whole && saved.count() > kNotes * 9 / 10 && saved.count() < kNotes, QStringLiteral("one bad page costs what it holds, not what comes after it (%1 of %2)").arg(saved.count()).arg(kNotes));
        // A file from before there were any has none to lose.
        sqlite3* raw = nullptr;
        const QString oldFile = folder.filePath(QStringLiteral("no-notes.db"));
        sqlite3_open_v2(oldFile.toUtf8().constData(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        const QByteArray pragma = "PRAGMA key = \"x'" + keyBytes.toHex() + "'\"";
        sqlite3_exec(raw, pragma.constData(), nullptr, nullptr, nullptr);
        sqlite3_exec(raw, "CREATE TABLE address(id INTEGER PRIMARY KEY); PRAGMA user_version = 1;", nullptr, nullptr, nullptr);
        sqlite3_close_v2(raw);
        const store::SavedLabels none = store::LedgerDb::salvageLabels(oldFile, keyBytes);
        check(none.read && none.whole && none.count() == 0, QStringLiteral("and a file from before there were any has none to lose"));
    }
    // A file set aside on a full disk: its labels and notes come over at the next start that can.
    {
        const QString pending = folder.filePath(QStringLiteral("pending.db"));
        const QString aside = pending + QStringLiteral(".unread-20260101-000000");
        const chain::Hash256 noted = hashOf("00000000000000000000000000000000000000000000000000000000000000c4");
        {
            store::LedgerDb db;
            QString why;
            db.open(aside, keyBytes, &why);
            db.saveNote(key.network, noted, {QStringLiteral("older"), QString()}, &why);
            db.saveAddressLabel(QStringLiteral("bc1qpending"), QStringLiteral("Till 4"), &why);
            db.close();
        }
        QFile(aside + QStringLiteral(".labels-pending")).open(QIODevice::WriteOnly);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        store::Ledger ledger(&model);
        ledger.addBook(&book);
        bool loaded = false;
        QHash<QString, QString> labels;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        QObject::connect(&ledger, &store::Ledger::labelsRead, &ledger, [&labels](const QHash<QString, QString>& read) { labels = read; });
        ledger.open(pending, keyBytes, wallets);
        check(waitFor([&loaded] { return loaded; }, 15000), QStringLiteral("a start finds a file whose labels and notes have yet to come over"));
        check(book.noteOf(noted).label == QLatin1String("older") && labels.value(QStringLiteral("bc1qpending")) == QLatin1String("Till 4"), QStringLiteral("and they do"));
        check(!QFile::exists(aside + QStringLiteral(".labels-pending")), QStringLiteral("once"));
        ledger.close();
    }
    // Set aside, and the new file will not open: nothing is said to have come along, what leads back to
    // it was there before it moved, and the next start brings it over.
    {
        const QString stuck = folder.filePath(QStringLiteral("stuck.db"));
        const chain::Hash256 noted = hashOf("00000000000000000000000000000000000000000000000000000000000000c5");
        const QDir here(folder.path());
        const QStringList marks{QStringLiteral("stuck.db.unread-*.labels-pending")};
        bool markedFirst = false;
        int recovered = -2;
        bool whole = true;
        bool stayedShut = false;
        // Something has to get in between the file going aside and the new one opening.
        for (int attempt = 0; attempt < 3 && recovered != -1; ++attempt) {
            here.rmdir(QStringLiteral("stuck.db-journal"));
            for (const QString& name : here.entryList({QStringLiteral("stuck.db*")}, QDir::Files))
                QFile::remove(here.filePath(name));
            {
                store::LedgerDb db;
                QString why;
                db.open(stuck, keyBytes, &why);
                db.saveNote(key.network, noted, {QStringLiteral("Only here"), QString()}, &why);
                db.saveAddressLabel(QStringLiteral("bc1qstuck"), QStringLiteral("Till 5"), &why);
                QHash<chain::Hash256, wallet::TxNote> notes;
                for (int n = 0; n < 8000; ++n) {
                    chain::Hash256 txid{};
                    txid[0] = uint8_t(n);
                    txid[1] = uint8_t(n >> 8);
                    txid[2] = 1;
                    notes.insert(txid, {QStringLiteral("note %1").arg(n), QStringLiteral("which takes a moment to read")});
                }
                db.saveNotes(key.network, notes, &why);
                wallet::HistoryDelta delta;
                for (int n = 0; n < 2000; ++n) {
                    wallet::TxRecord tx;
                    tx.network = key.network;
                    tx.txid[0] = uint8_t(n);
                    tx.txid[1] = uint8_t(n >> 8);
                    tx.inputs.resize(8);
                    delta.txs.append(tx);
                }
                db.saveHistory(key.network, delta, &why);
                db.close();
            }
            flipByte(stuck, QFile(stuck).size() - 100);
            std::atomic<bool> stop{false};
            std::atomic<bool> marked{false};
            std::thread watcher([&] {
                while (!stop.load()) {
                    const QStringList found = here.entryList({QStringLiteral("stuck.db.unread-*")}, QDir::Files);
                    for (const QString& name : found) {
                        if (name.endsWith(QLatin1String(".labels-pending")))
                            continue;
                        marked = QFile::exists(here.filePath(name + QStringLiteral(".labels-pending")));
                        here.mkdir(QStringLiteral("stuck.db-journal"));
                        return;
                    }
                }
            });
            wallet::WalletModel model;
            wallet::HistoryBook book(key.network);
            store::Ledger ledger(&model);
            ledger.addBook(&book);
            bool loaded = false;
            QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
            QObject::connect(&ledger, &store::Ledger::setAside, &ledger, [&](const QString&, int count, bool all) {
                recovered = count;
                whole = all;
            });
            ledger.open(stuck, keyBytes, wallets);
            waitFor([&loaded] { return loaded; }, 20000);
            stop = true;
            watcher.join();
            markedFirst = marked;
            stayedShut = !ledger.isOpen();
            ledger.close();
        }
        check(markedFirst, QStringLiteral("a file set aside is marked before it moves: a power cut from there on still leaves word of it"));
        check(recovered == -1 && !whole && stayedShut, QStringLiteral("the new file will not open: nothing is said to have come along (%1)").arg(recovered));
        check(here.entryList(marks, QDir::Files).size() == 1, QStringLiteral("and the mark stays"));
        here.rmdir(QStringLiteral("stuck.db-journal"));
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        store::Ledger ledger(&model);
        ledger.addBook(&book);
        bool loaded = false;
        QHash<QString, QString> labels;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        QObject::connect(&ledger, &store::Ledger::labelsRead, &ledger, [&labels](const QHash<QString, QString>& read) { labels = read; });
        ledger.open(stuck, keyBytes, wallets);
        check(waitFor([&loaded] { return loaded; }, 15000) && ledger.isOpen(), QStringLiteral("what stood in the way goes, and the next start opens"));
        check(book.noteOf(noted).label == QLatin1String("Only here") && labels.value(QStringLiteral("bc1qstuck")) == QLatin1String("Till 5"), QStringLiteral("and brings them over"));
        check(here.entryList(marks, QDir::Files).isEmpty() && !ledger.labelsOwed(), QStringLiteral("and that ends it"));
        ledger.close();
    }
    // What a mark leads to decides what becomes of it.
    {
        const QString marked = folder.filePath(QStringLiteral("marked.db"));
        const chain::Hash256 noted = hashOf("00000000000000000000000000000000000000000000000000000000000000c6");
        const auto aside = [&](const QString& stamp, const QByteArray& under, const QString& text) {
            const QString file = marked + QStringLiteral(".unread-") + stamp;
            store::LedgerDb db;
            QString why;
            db.open(file, under, &why);
            db.saveNote(key.network, noted, {text, QString()}, &why);
            db.close();
            QFile(file + QStringLiteral(".labels-pending")).open(QIODevice::WriteOnly);
            return file;
        };
        const QString older = aside(QStringLiteral("20260101-000000"), keyBytes, QStringLiteral("older"));
        const QString newer = aside(QStringLiteral("20260102-000000"), keyBytes, QStringLiteral("newer"));
        const QString another = aside(QStringLiteral("20260103-000000"), QByteArray(32, 'x'), QStringLiteral("another key"));
        const QString nothing = marked + QStringLiteral(".unread-20260104-000000");
        QFile(nothing + QStringLiteral(".labels-pending")).open(QIODevice::WriteOnly);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        store::Ledger ledger(&model);
        ledger.addBook(&book);
        bool loaded = false;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        ledger.open(marked, keyBytes, wallets);
        check(waitFor([&loaded] { return loaded; }, 15000), QStringLiteral("a start finds several files whose labels have yet to come over"));
        check(book.noteOf(noted).label == QLatin1String("newer"), QStringLiteral("of two, the newer word stands (%1)").arg(book.noteOf(noted).label));
        check(!QFile::exists(older + QStringLiteral(".labels-pending")) && !QFile::exists(newer + QStringLiteral(".labels-pending")), QStringLiteral("and both have come over"));
        check(QFile::exists(another + QStringLiteral(".labels-pending")) && !ledger.labelsOwed(), QStringLiteral("one that cannot be read keeps its mark, it may be another time, and holds nothing up"));
        check(!QFile::exists(nothing + QStringLiteral(".labels-pending")), QStringLiteral("and a mark with no file is none"));
        ledger.close();
    }
    // The new file cannot be written just now: nothing about them changes until they have come over.
    {
        store::LedgerDb::setBusyTimeoutForTest(100);
        const QString owedFile = folder.filePath(QStringLiteral("owed.db"));
        const QString aside = owedFile + QStringLiteral(".unread-20260101-000000");
        const chain::Hash256 noted = hashOf("00000000000000000000000000000000000000000000000000000000000000c7");
        QString why;
        {
            store::LedgerDb db;
            db.open(aside, keyBytes, &why);
            db.saveNote(key.network, noted, {QStringLiteral("owed"), QString()}, &why);
            db.saveAddressLabel(QStringLiteral("bc1qowed"), QStringLiteral("Till 6"), &why);
            db.close();
            db.open(owedFile, keyBytes, &why);
            db.close();
        }
        QFile(aside + QStringLiteral(".labels-pending")).open(QIODevice::WriteOnly);
        sqlite3* holder = nullptr;
        sqlite3_open_v2(owedFile.toUtf8().constData(), &holder, SQLITE_OPEN_READWRITE, nullptr);
        const QByteArray pragma = "PRAGMA key = \"x'" + keyBytes.toHex() + "'\"";
        sqlite3_exec(holder, pragma.constData(), nullptr, nullptr, nullptr);
        sqlite3_exec(holder, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        wallet::WalletModel model;
        wallet::HistoryBook book(key.network);
        store::Ledger ledger(&model);
        ledger.addBook(&book);
        bool loaded = false;
        int heard = 0;
        QHash<QString, QString> labels;
        QObject::connect(&ledger, &store::Ledger::loaded, &ledger, [&loaded] { loaded = true; });
        QObject::connect(&ledger, &store::Ledger::labelsOwedChanged, &ledger, [&heard] { ++heard; });
        QObject::connect(&ledger, &store::Ledger::labelsRead, &ledger, [&labels](const QHash<QString, QString>& read) { labels = read; });
        ledger.open(owedFile, keyBytes, wallets);
        check(waitFor([&loaded] { return loaded; }, 15000) && ledger.isOpen() && ledger.labelsOwed() && heard == 1, QStringLiteral("labels that cannot be written just now are owed, and that is said"));
        check(QFile::exists(aside + QStringLiteral(".labels-pending")), QStringLiteral("and the mark stays"));
        ledger.bringOverOwed();
        pause(1000);
        check(ledger.labelsOwed() && heard == 1, QStringLiteral("and asked again they still are"));
        sqlite3_exec(holder, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close_v2(holder);
        ledger.bringOverOwed();
        check(waitFor([&ledger] { return !ledger.labelsOwed(); }, 5000) && heard == 2, QStringLiteral("what stood in the way goes, and they come over in the same session"));
        check(waitFor([&book, &noted] { return book.noteOf(noted).label == QLatin1String("owed"); }, 2000) && labels.value(QStringLiteral("bc1qowed")) == QLatin1String("Till 6"), QStringLiteral("to what is on screen too"));
        check(!QFile::exists(aside + QStringLiteral(".labels-pending")), QStringLiteral("and the mark goes"));
        ledger.close();
        store::LedgerDb::setBusyTimeoutForTest(0);
    }

    // What a wallet leaves behind goes with it, whatever rows the window has.
    {
        const QString labelled = folder.filePath(QStringLiteral("labelled.db"));
        store::LedgerDb db;
        QString why;
        db.open(labelled, keyBytes, &why);
        wallet::AddressRow row;
        row.network = key.network;
        row.wallet = 4;
        row.type = key.type;
        row.address = QStringLiteral("bc1qfour");
        row.scripthash = first.scripthash;
        wallet::AddressRow shared = row;
        shared.index = 1;
        shared.address = QStringLiteral("bc1qshared");
        wallet::AddressRow other = shared;
        other.wallet = 5;
        db.saveRows({row, shared, other}, &why);
        db.saveAddressLabel(QStringLiteral("bc1qfour"), QStringLiteral("goes"), &why);
        db.saveAddressLabel(QStringLiteral("bc1qshared"), QStringLiteral("stays"), &why);
        // A wallet that is only not in this wallet.dat, at a start: its rows go, not what only this PC has.
        QVector<wallet::AddressRow> read;
        check(db.loadRows({5}, 100, [&read](QVector<wallet::AddressRow>&& chunk) { read += chunk; }, &why) && read.size() == 1, QStringLiteral("a start lets go of the rows of a wallet it does not know"));
        check(db.loadAddressLabels(&why).value(QStringLiteral("bc1qfour")) == QLatin1String("goes"), QStringLiteral("but keeps its labels"));
        db.saveRows({row, shared, other}, &why);
        QStringList gone;
        check(db.removeWallet(4, &why, &gone) && gone == QStringList{QStringLiteral("bc1qfour")}, QStringLiteral("a wallet removed takes its addresses' labels along, and says which"));
        check(db.loadAddressLabels(&why).value(QStringLiteral("bc1qshared")) == QLatin1String("stays"), QStringLiteral("but not those of an address another wallet has too"));
        // One this wallet.dat gave out and no longer has was removed, and that only did not get as far as here.
        db.saveRows({row, shared, other}, &why);
        db.saveAddressLabel(QStringLiteral("bc1qfour"), QStringLiteral("goes again"), &why);
        QStringList finished;
        read.clear();
        check(db.loadRows({5}, 100, [&read](QVector<wallet::AddressRow>&& chunk) { read += chunk; }, &why, 5, &finished) && finished == QStringList{QStringLiteral("bc1qfour")},
              QStringLiteral("a removal that did not reach the file finishes at a start, labels and all"));
        db.close();
    }
}

// What the window goes by: a controller with a database, and a list, on dead servers.
void testWindow(const Check& check, const wallet::AccountKey& key)
{
    const AddressOf first = addressOf(key, 0, 0);
    const AddressOf second = addressOf(key, 0, 1);
    WalletController controller;
    wallet::ScanSettings settings = controller.store().settings;
    settings.servers.insert(wallet::networkInfo(key.network).id, {QStringLiteral("127.0.0.1:1")});
    QString error;
    controller.applySettings(settings, &error);
    check(!controller.labelsBlocker().isEmpty() && !controller.labelsBlocker().contains(QStringLiteral("window")), QStringLiteral("with no wallet, labels say to add one first, and do not blame another window"));
    wallet::AccountKey own = key;
    QList<wallet::AccountKey> keys{own};
    if (!controller.addKeysWallet(keys, QStringLiteral("Shop"), &error)) {
        check(false, QStringLiteral("a wallet for the test: %1").arg(error));
        return;
    }
    check(waitFor([&controller] { return !controller.loadingSaved(); }, 15000), QStringLiteral("its database opens"));
    wallet::UtxoHistory* history = controller.historyOf(key.network);
    if (!history || !waitFor([&controller] { return controller.labelsBlocker().isEmpty(); }, 5000)) {
        check(false, QStringLiteral("a history to work with: %1").arg(controller.labelsBlocker()));
        return;
    }
    wallet::HistoryBook* book = history->book();
    const quint32 wallet = controller.store().wallets.first().id;
    const QString bitcoin = wallet::networkInfo(key.network).id;

    const auto record = [&](int n, qint64 time, int height, const QVector<wallet::OurOutput>& outputs, const std::vector<chain::OutPoint>& inputs, qint64 outTotal, bool proven = true) {
        wallet::TxRecord tx;
        tx.network = key.network;
        tx.txid[0] = uint8_t(n);
        tx.txid[31] = 0x42;
        tx.time = time;
        tx.height = height;
        tx.proven = proven;
        tx.outTotal = outTotal;
        tx.outputs = outputs;
        tx.inputs = inputs;
        return tx;
    };
    const auto pays = [](const AddressOf& to, qint64 value, quint32 n = 0) {
        wallet::OurOutput output;
        output.n = n;
        output.value = value;
        output.scripthash = to.scripthash;
        return output;
    };
    const wallet::TxRecord in = record(1, 1700000000, 100, {pays(first, 100000000)}, {chain::OutPoint{hashOf("00000000000000000000000000000000000000000000000000000000000000a1"), 0}}, 100000000);
    // Pays someone 0.1 and the rest back, for a fee of 0.0001.
    const wallet::TxRecord out = record(2, 1700000100, 101, {pays(second, 89990000, 1)}, {chain::OutPoint{in.txid, 0}}, 99990000);
    // Everything to the wallets, but someone else paid for most of it.
    const wallet::TxRecord joined = record(3, 1700000200, 102, {pays(first, 150000000)}, {chain::OutPoint{out.txid, 1}, chain::OutPoint{hashOf("00000000000000000000000000000000000000000000000000000000000000a2"), 0}}, 150000000);
    const wallet::TxRecord waiting = record(4, 1600000000, 0, {pays(second, 500)}, {chain::OutPoint{hashOf("00000000000000000000000000000000000000000000000000000000000000a3"), 0}}, 500, false);
    book->addSaved({in, out, joined, waiting});
    book->describeAddress(first.scripthash, wallet, first.address);
    book->describeAddress(second.scripthash, wallet, second.address);

    ui::HistoryPage page(&controller);
    ui::HistoryTableModel* lines = page.tableModel();
    lines->setWatched(true);
    lines->rebuildNow();
    check(lines->rowCount() == 4, QStringLiteral("the list has its lines (%1)").arg(lines->rowCount()));
    const auto lineOf = [&](const wallet::TxRecord& tx) { return lines->lineAt(lines->rowOf(book, tx.txid)); };
    check(lines->rowOf(book, waiting.txid) == 0, QStringLiteral("what is pending stands above every block, however long ago it was first seen"));
    check(lineOf(joined) && lineOf(joined)->kind == ui::TxKind::Received, QStringLiteral("what mostly someone else paid for is received, even with every output ours"));
    check(lineOf(out) && lineOf(out)->kind == ui::TxKind::Sent, QStringLiteral("and a payment with change back is sent"));
    check(lines->receivedShown() == 100000000 + 60010000 && lines->spentShown() == 10010000,
          QStringLiteral("in and out are what the lines came to, change and what is pending left out (%1, %2)").arg(lines->receivedShown()).arg(lines->spentShown()));

    const ui::HistoryTableModel::ExportLine sent = lines->exportLine(*lineOf(out), lines->filter());
    check(sent.amount.startsWith(QLatin1Char('-')) && sent.addresses.contains(first.address) && sent.addresses.contains(second.address), QStringLiteral("an export of a line is signed, with every address of it"));

    // The same line narrowed to another wallet says only that much, whatever the list does meanwhile.
    ui::HistoryTableModel::Filter onlySecond;
    onlySecond.scripthash = second.scripthash;
    const ui::HistoryTableModel::ExportLine narrowed = lines->exportLine(*lineOf(out), onlySecond);
    check(!narrowed.addresses.contains(first.address) && narrowed.addresses.contains(second.address), QStringLiteral("narrowed, only what it is narrowed to"));

    // A row chosen stays that transaction, through a sort.
    {
        auto* view = page.findChild<QTableView*>();
        const int row = lines->rowOf(book, in.txid);
        view->selectRow(row);
        view->sortByColumn(ui::HistoryTableModel::ColAmount, Qt::AscendingOrder);
        const QModelIndexList chosen = view->selectionModel()->selectedRows();
        check(chosen.size() == 1 && chosen.first().row() == lines->rowOf(book, in.txid), QStringLiteral("a line chosen stays chosen when the list is sorted"));
        // Sorted by what takes text.
        view->sortByColumn(ui::HistoryTableModel::ColWallet, Qt::AscendingOrder);
        view->sortByColumn(ui::HistoryTableModel::ColLabel, Qt::AscendingOrder);
        view->sortByColumn(ui::HistoryTableModel::ColDate, Qt::DescendingOrder);
        check(lines->rowCount() == 4 && lines->rowOf(book, waiting.txid) == 0, QStringLiteral("and sorted by anything it holds the same lines"));
    }

    // Copy goes by the cell chosen, through a rebuild, and what says there is nothing is nothing to copy.
    {
        auto* view = page.findChild<QTableView*>();
        const int row = lines->rowOf(book, in.txid);
        view->selectRow(row);
        view->selectionModel()->setCurrentIndex(lines->index(row, ui::HistoryTableModel::ColTx), QItemSelectionModel::NoUpdate);
        lines->rebuildNow();
        check(view->currentIndex().column() == ui::HistoryTableModel::ColTx && view->currentIndex().row() == lines->rowOf(book, in.txid), QStringLiteral("the cell chosen stays chosen when the list is rebuilt"));
        check(ui::HistoryPage::textToCopy(view->currentIndex()) == hexOf(in.txid), QStringLiteral("and Copy gives what it means, whole"));
        check(ui::HistoryPage::textToCopy(lines->index(lines->rowOf(book, in.txid), ui::HistoryTableModel::ColFee)).isEmpty(), QStringLiteral("a dash is nothing to copy"));
    }

    // A search goes through addresses' labels, and follows them.
    {
        auto* search = page.findChild<QLineEdit*>(QStringLiteral("mimSearch"));
        search->setText(QStringLiteral("Till 9"));
        check(waitFor([&lines] { return lines->rowCount() == 0; }, 2000), QStringLiteral("a search for a label nobody has finds nothing"));
        check(controller.setAddressLabel(second.address, QStringLiteral("Till 9"), &error), QStringLiteral("an address gets that label"));
        check(waitFor([&lines] { return lines->rowCount() > 0; }, 3000), QStringLiteral("and the search finds it, without being typed again"));
        check(controller.setAddressLabel(second.address, QString(), &error) && waitFor([&lines] { return lines->rowCount() == 0; }, 3000), QStringLiteral("and no longer when it is gone"));
        search->clear();
        check(waitFor([&lines] { return lines->rowCount() == 4; }, 3000), QStringLiteral("cleared, all the lines are back"));
    }

    // Narrowed to one address, a line says that one's wallet, not every wallet it touched.
    {
        ui::HistoryTableModel::Filter one;
        one.scripthash = second.scripthash;
        lines->setWalletNamesForTest({{wallet, QStringLiteral("Shop")}, {wallet + 1, QStringLiteral("Other")}});
        book->describeAddress(first.scripthash, wallet + 1, first.address);
        const ui::HistoryTableModel::ExportLine there = lines->exportLine(*lineOf(out), one);
        const ui::HistoryTableModel::ExportLine everywhere = lines->exportLine(*lineOf(out), ui::HistoryTableModel::Filter());
        check(there.wallet == QLatin1String("Shop") && there.addresses == second.address, QStringLiteral("an export narrowed to an address is of that address's wallet (%1)").arg(there.wallet));
        check(everywhere.wallet.contains(QLatin1String("Shop")) && everywhere.wallet.contains(QLatin1String("Other")), QStringLiteral("and not narrowed, of both (%1)").arg(everywhere.wallet));
        book->describeAddress(first.scripthash, wallet, first.address);
        lines->setWalletNamesForTest({{wallet, QStringLiteral("Shop")}});
    }

    // "All of it" is all of it, whatever was left in the filters.
    {
        auto* period = page.findChild<QComboBox*>(QStringLiteral("historyPeriod"));
        auto* type = page.findChild<QComboBox*>(QStringLiteral("historyType"));
        period->setCurrentIndex(1);
        type->setCurrentIndex(2);
        check(lines->rowCount() == 0, QStringLiteral("a filter left on hides nearly all of it"));
        page.showAddress(key.network, first.scripthash, first.address);
        check(lines->rowCount() == 3 && period->currentIndex() == 0 && type->currentIndex() == 0, QStringLiteral("an address opens to all of its own"));
        page.showAll();
        check(lines->rowCount() == 4, QStringLiteral("and Show all to all there is"));
    }

    // Labels: secrets stay out, an id these wallets know is no secret.
    const chain::Hash256 knownId = in.txid;
    const QString knownHex = QString::fromStdString(chain::hashHex(knownId));
    check(controller.setTxNote(key.network, out.txid, {QStringLiteral("Invoice 77"), QStringLiteral("refund of ") + knownHex}, &error), QStringLiteral("a note can name a transaction of these wallets"));
    check(!controller.setTxNote(key.network, out.txid, {QStringLiteral("Invoice 77"), QStringLiteral("key ") + QStringLiteral("ab").repeated(32)}, &error) && !error.isEmpty(), QStringLiteral("but not what could be a key"));
    check(book->noteOf(out.txid).note.contains(knownHex), QStringLiteral("which leaves what was there"));
    check(controller.setAddressLabel(first.address, QStringLiteral("Till 1"), &error), QStringLiteral("an address's label"));

    // A file of labels: only what a line says changes anything.
    QTemporaryDir folder;
    const QString labelsPath = folder.filePath(QStringLiteral("labels.jsonl"));
    {
        wallet::LabelLine labelOnly;
        labelOnly.txid = out.txid;
        labelOnly.network = bitcoin;
        labelOnly.label = QStringLiteral("From another wallet");
        wallet::LabelLine noteOnlyAddress;
        noteOnlyAddress.isTransaction = false;
        noteOnlyAddress.address = first.address;
        noteOnlyAddress.note = QStringLiteral("nothing to say of its label");
        wallet::LabelLine blank;
        blank.txid = in.txid;
        blank.network = bitcoin;
        blank.label = QStringLiteral("   ");
        wallet::LabelLine secret;
        secret.txid = in.txid;
        secret.network = bitcoin;
        secret.label = QStringLiteral("ab").repeated(32);
        QFile file(labelsPath);
        file.open(QIODevice::WriteOnly);
        file.write(wallet::writeLabelLine(labelOnly) + wallet::writeLabelLine(noteOnlyAddress) + wallet::writeLabelLine(blank) + wallet::writeLabelLine(secret));
    }
    int added = -9;
    int skipped = -9;
    int later = -9;
    QString importError;
    controller.importLabelsFile(labelsPath, &controller, [&](int count, int left, int notYet, const QString& why) {
        added = count;
        skipped = left;
        later = notYet;
        importError = why;
    });
    check(waitFor([&added] { return added != -9; }, 15000) && added == 1 && skipped == 2 && later == 1, QStringLiteral("a file of labels goes in, and says what it left out (%1, %2 and %3)").arg(added).arg(skipped).arg(later));
    check(book->noteOf(out.txid).label == QLatin1String("From another wallet") && book->noteOf(out.txid).note.contains(knownHex), QStringLiteral("and a line with a label alone leaves the note"));
    check(controller.addressLabel(first.address) == QLatin1String("Till 1"), QStringLiteral("nor does one with nothing for an address take its label"));
    check(!book->noteOf(in.txid).label.contains(QStringLiteral("abab")), QStringLiteral("and what could be a key stays out"));

    const QString backup = folder.filePath(QStringLiteral("backup.jsonl"));
    bool written = false;
    QString writeError;
    controller.writeLabelsFile(backup, &controller, [&](bool ok, const QString& why) {
        written = ok;
        writeError = why;
    });
    check(waitFor([&written] { return written; }, 15000), QStringLiteral("every label and note goes to a file, off this thread"));
    QFile copy(backup);
    copy.open(QIODevice::ReadOnly);
    int leftOut = 0;
    const QVector<wallet::LabelLine> read = wallet::readLabelLines(copy.readAll(), &leftOut);
    check(read.size() == 2 && leftOut == 0 && controller.labelCount() == 2, QStringLiteral("and they are all there (%1)").arg(read.size()));
    check(controller.labelCount(wallet) >= 1, QStringLiteral("what would go with this wallet is known before it goes"));

    // Narrowed to an address, and its wallet goes: the page goes back to what there is.
    page.showAddress(key.network, first.scripthash, first.address);
    check(controller.removeAllWallets(&error), QStringLiteral("and it can all be removed"));
    check(controller.labelCount() == 0, QStringLiteral("with its labels and notes"));
    check(lines->filter().scripthash.isEmpty(), QStringLiteral("and a page narrowed to one of its addresses no longer is"));
    wallet::UtxoHistory::setFirstPauseForTest(0);
}

void testSavedHistory(const Check& check, const wallet::AccountKey& key)
{
    QTemporaryDir folder;
    const QString path = folder.filePath(QStringLiteral("history.db"));
    const QByteArray keyBytes(32, 'k');
    const QString bitcoin = wallet::networkInfo(key.network).id;

    const auto sampleTx = [&key](int n) {
        wallet::TxRecord tx;
        tx.network = key.network;
        tx.txid[0] = uint8_t(n);
        tx.txid[31] = 0xee;
        tx.height = 100 + n;
        tx.time = 1700000000 + n;
        tx.fee = n % 2 == 0 ? 1000 : -1;
        tx.vsize = 141;
        tx.blockPos = n;
        tx.outTotal = 5000;
        tx.proven = n % 3 == 0;
        tx.replaceable = n % 5 == 0;
        tx.usdThen = n % 4 == 0 ? 30000.5 : 0.0;
        tx.usdThenAt = n % 4 == 0 ? 1700000100 : 0;
        tx.seen = n == 7 ? wallet::TxSeen::Replaced : wallet::TxSeen::Alive;
        if (n == 7)
            tx.replacedBy[1] = 9;
        chain::OutPoint point;
        point.txid[2] = uint8_t(n);
        point.n = uint32_t(n);
        tx.inputs.push_back(point);
        wallet::OurOutput output;
        output.n = 1;
        output.value = 4000 + n;
        output.scripthash = QStringLiteral("%1").arg(n, 64, 16, QLatin1Char('0'));
        tx.outputs.append(output);
        return tx;
    };

    {
        store::LedgerDb db;
        QString why;
        check(db.open(path, keyBytes, &why) == store::LedgerDb::Open::Ready, QStringLiteral("a new database opens at the newest schema"));
        wallet::HistoryDelta delta;
        for (int n = 0; n < 12; ++n)
            delta.txs.append(sampleTx(n));
        delta.addresses.append({QStringLiteral("%1").arg(3, 64, 16, QLatin1Char('0')), 7, QStringLiteral("bc1qthree"), QStringLiteral("cd")});
        delta.headers.insert(101, QByteArray(80, 'h'));
        delta.headers.insert(102, QByteArray(80, 'i'));
        check(db.saveHistory(key.network, delta, &why), QStringLiteral("transactions go in"));
        check(db.saveNote(key.network, sampleTx(3).txid, {QStringLiteral("Order 17"), QStringLiteral("paid by Ana\nsecond line")}, &why), QStringLiteral("a label and a note go in"));
        check(db.saveAddressLabel(QStringLiteral("bc1qthree"), QStringLiteral("Till 2"), &why), QStringLiteral("an address's label goes in"));
        // What changes, in place.
        wallet::HistoryDelta again;
        wallet::TxRecord moved = sampleTx(5);
        moved.height = 999;
        moved.outputs[0].value = 123;
        again.txs.append(moved);
        again.removedTxs.append(sampleTx(6).txid);
        again.dropHeadersFrom = 102;
        check(db.saveHistory(key.network, again, &why), QStringLiteral("and a change replaces what was there"));
        db.close();
    }
    {
        store::LedgerDb db;
        QString why;
        check(db.open(path, keyBytes, &why) == store::LedgerDb::Open::Ready, QStringLiteral("it opens again"));
        QVector<wallet::TxRecord> all;
        check(db.loadHistory(key.network, 5, [&all](QVector<wallet::TxRecord>&& chunk) { all += chunk; }, &why), QStringLiteral("and reads"));
        check(all.size() == 11, QStringLiteral("every transaction but the one removed"));
        const auto find = [&all](const chain::Hash256& txid) {
            return std::find_if(all.cbegin(), all.cend(), [&txid](const wallet::TxRecord& tx) { return tx.txid == txid; });
        };
        bool same = true;
        for (int n = 0; n < 12; ++n) {
            if (n == 5 || n == 6)
                continue;
            const wallet::TxRecord want = sampleTx(n);
            const auto got = find(want.txid);
            same = same && got != all.cend() && got->height == want.height && got->time == want.time && got->fee == want.fee && got->vsize == want.vsize
                   && got->blockPos == want.blockPos && got->outTotal == want.outTotal && got->proven == want.proven && got->replaceable == want.replaceable
                   && got->usdThen == want.usdThen && got->usdThenAt == want.usdThenAt && got->seen == want.seen && got->replacedBy == want.replacedBy
                   && got->inputs.size() == 1 && got->inputs[0] == want.inputs[0] && got->outputs.size() == 1 && got->outputs[0].n == 1
                   && got->outputs[0].value == want.outputs[0].value && got->outputs[0].scripthash == want.outputs[0].scripthash;
        }
        check(same, QStringLiteral("each exactly as it went in"));
        const auto changed = find(sampleTx(5).txid);
        check(changed != all.cend() && changed->height == 999 && changed->outputs.size() == 1 && changed->outputs[0].value == 123, QStringLiteral("the changed one as it was changed"));
        const QVector<wallet::AddressHistory> addresses = db.loadAddressHistory(key.network, &why);
        check(addresses.size() == 1 && addresses[0].wallet == 7 && addresses[0].status == QLatin1String("cd") && addresses[0].address == QLatin1String("bc1qthree"), QStringLiteral("what an address is complete up to"));
        const QMap<int, QByteArray> headers = db.loadHeaders(key.network, &why);
        check(headers.size() == 1 && headers.contains(101), QStringLiteral("headers, without those of a chain that gave way"));
        const QHash<chain::Hash256, wallet::TxNote> notes = db.loadNotes(key.network, &why);
        check(notes.value(sampleTx(3).txid).label == QLatin1String("Order 17") && notes.value(sampleTx(3).txid).note.contains(QLatin1Char('\n')), QStringLiteral("notes, whole"));
        check(db.loadAddressLabels(&why).value(QStringLiteral("bc1qthree")) == QLatin1String("Till 2"), QStringLiteral("labels"));
        check(db.saveNote(key.network, sampleTx(3).txid, {}, &why) && db.loadNotes(key.network, &why).isEmpty(), QStringLiteral("and an empty note is none"));
        db.close();
    }

    // A file of the schema before this one, with what it held.
    const QString older = folder.filePath(QStringLiteral("older.db"));
    {
        sqlite3* raw = nullptr;
        sqlite3_open_v2(older.toUtf8().constData(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        const QByteArray pragma = "PRAGMA key = \"x'" + keyBytes.toHex() + "'\"";
        sqlite3_exec(raw, pragma.constData(), nullptr, nullptr, nullptr);
        const char* schema = R"sql(
            CREATE TABLE name(id INTEGER PRIMARY KEY, text TEXT NOT NULL UNIQUE);
            CREATE TABLE address(id INTEGER PRIMARY KEY, wallet INTEGER NOT NULL, network INTEGER NOT NULL REFERENCES name(id), type INTEGER NOT NULL REFERENCES name(id),
                account INTEGER NOT NULL, branch INTEGER NOT NULL, idx INTEGER NOT NULL, address TEXT NOT NULL, scripthash TEXT, status TEXT, checked INTEGER NOT NULL,
                used INTEGER NOT NULL, confirmed INTEGER NOT NULL, unconfirmed INTEGER NOT NULL, nonce INTEGER NOT NULL, UNIQUE(wallet, network, type, account, branch, idx));
            CREATE TABLE balance(address INTEGER NOT NULL REFERENCES address(id) ON DELETE CASCADE, asset INTEGER NOT NULL REFERENCES name(id), amount BLOB NOT NULL, PRIMARY KEY(address, asset)) WITHOUT ROWID;
            CREATE TABLE price(asset TEXT PRIMARY KEY, usd REAL NOT NULL, at INTEGER NOT NULL, source TEXT NOT NULL) WITHOUT ROWID;
            CREATE TABLE network_time(network TEXT PRIMARY KEY, at INTEGER, nonces_at INTEGER, sweep_from INTEGER, sweep_wallet INTEGER, sweep_account INTEGER, sweep_index INTEGER, sweep_clears INTEGER) WITHOUT ROWID;
            INSERT INTO name(id, text) VALUES(1, 'bitcoin'), (2, 'p2wpkh');
            INSERT INTO address(wallet, network, type, account, branch, idx, address, scripthash, status, checked, used, confirmed, unconfirmed, nonce)
                VALUES(5, 1, 2, 0, 0, 0, 'bc1qold', 'ab', 'cd', 1, 1, 7000, 0, 0);
            PRAGMA user_version = 1;
        )sql";
        sqlite3_exec(raw, schema, nullptr, nullptr, nullptr);
        sqlite3_close_v2(raw);
    }
    {
        store::LedgerDb db;
        QString why;
        check(db.open(older, keyBytes, &why) == store::LedgerDb::Open::Ready, QStringLiteral("a file from the release before opens"));
        QVector<wallet::AddressRow> rows;
        check(db.loadRows({5}, 100, [&rows](QVector<wallet::AddressRow>&& chunk) { rows += chunk; }, &why) && rows.size() == 1 && rows[0].confirmed == 7000,
              QStringLiteral("with the rows it held"));
        wallet::HistoryDelta delta;
        delta.txs.append(sampleTx(1));
        check(db.saveHistory(key.network, delta, &why), QStringLiteral("and takes a history now"));
        QVector<wallet::TxRecord> read;
        check(db.loadHistory(key.network, 10, [&read](QVector<wallet::TxRecord>&& chunk) { read += chunk; }, &why) && read.size() == 1, QStringLiteral("and gives it back"));
        db.close();
    }
    Q_UNUSED(bitcoin);
}

} // namespace

void runHistoryTest(const Check& check, QString* report, const QList<wallet::AccountKey>& someKeys)
{
    Q_UNUSED(report);
    const auto native = std::find_if(someKeys.cbegin(), someKeys.cend(), [](const wallet::AccountKey& key) { return key.type == chain::ScriptType::P2WPKH; });
    if (native == someKeys.cend()) {
        check(false, QStringLiteral("a native SegWit key to work with"));
        return;
    }
    testChainUnits(check, *native);
    testBook(check, *native);
    testSavedHistory(check, *native);
    testLedger(check, *native);
    testFlow(check, *native);
    testChainMoves(check, *native);
    testLiars(check, *native);
    testPrices(check, *native);
    testWindow(check, *native);
}

} // namespace app
