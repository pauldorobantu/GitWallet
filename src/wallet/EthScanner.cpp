#include "wallet/EthScanner.h"

#include "chain/Hash.h"
#include "net/EthRpc.h"
#include "net/Multicall.h"
#include "wallet/WalletModel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QTimer>

#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace wallet {

namespace {

// 50 addresses per attempt: one JSON-RPC batch (a single HTTP request, see
// EthRpc::kMaxBatch) plus one eth_call with 151 Multicall3 sub-calls (~1M gas):
// the chain-id proof, then three balances per address.
constexpr int kChunk = 50;
static_assert(kChunk <= net::EthRpc::kMaxBatch, "a chunk must fit one JSON-RPC batch");

constexpr int kMaxRetries = 3; // then the chunk is skipped until the next pass
constexpr int kRetryMs = 10'000;
constexpr int kProgressMs = 250;
constexpr int kMaxGap = 100'000;
constexpr int kMinRefreshSeconds = 10; // public endpoints rate-limit; never poll faster
constexpr int kMaxRefreshSeconds = 24 * 3600;
constexpr qsizetype kMaxReported = 64; // distinct error texts remembered at once
constexpr quint64 kMainnetChainId = 1;
constexpr quint32 kGetChainIdSelector = 0x3408e470u; // Multicall3.getChainId()

int hexValue(QChar c)
{
    const char16_t u = c.unicode();
    if (u >= u'0' && u <= u'9') return int(u - u'0');
    if (u >= u'a' && u <= u'f') return int(u - u'a') + 10;
    if (u >= u'A' && u <= u'F') return int(u - u'A') + 10;
    return -1;
}

// A JSON-RPC quantity ("0x0", "0x715") as a quint64. Values that do not fit
// saturate instead of wrapping, so a bogus huge nonce can never read as 0.
bool parseQuantity(const QJsonValue& value, quint64* out)
{
    if (!value.isString())
        return false;
    const QString text = value.toString();
    if (text.size() < 3 || text.at(0) != QLatin1Char('0')
        || (text.at(1) != QLatin1Char('x') && text.at(1) != QLatin1Char('X')))
        return false;
    constexpr quint64 kMax = std::numeric_limits<quint64>::max();
    quint64 v = 0;
    bool saturated = false;
    for (qsizetype i = 2; i < text.size(); ++i) {
        const int d = hexValue(text.at(i));
        if (d < 0)
            return false;
        if (v > (kMax >> 4))
            saturated = true;
        else
            v = (v << 4) | quint64(d);
    }
    *out = saturated ? kMax : v;
    return true;
}

// Multicall3.getChainId() returns block.chainid of the chain the eth_call runs
// on. It goes first in every aggregate3, so the reply that carries the balances
// also proves where they come from, whichever endpoint EthRpc failed over to:
// Multicall3 sits at this same address on BSC, Polygon, the L2s and testnets, so
// reaching it proves nothing by itself. The selector is derived and checked the
// way net::multicall checks its own (all four default endpoints answered chain
// id 1 to it on 2026-10-31); a mismatch leaves the calldata empty, and
// sendBalances() then sends nothing.
const QByteArray& chainIdCall()
{
    static const QByteArray call = []() -> QByteArray {
        try {
            const std::string signature("getChainId()");
            const chain::Bytes h = chain::keccak256(chain::Bytes(signature.begin(), signature.end()));
            if (h.size() < 4)
                return {};
            const quint32 derived = (quint32(h[0]) << 24) | (quint32(h[1]) << 16)
                                    | (quint32(h[2]) << 8) | quint32(h[3]);
            if (derived != kGetChainIdSelector)
                return {};
            return QByteArray(reinterpret_cast<const char*>(h.data()), 4);
        } catch (const std::exception&) {
            return {};
        }
    }();
    return call;
}

// A 32-byte ABI word as a number; false unless the value fits in 64 bits.
bool wordValue(const QByteArray& word, quint64* out)
{
    if (word.size() != 32)
        return false;
    quint64 v = 0;
    for (qsizetype i = 0; i < 32; ++i) {
        const quint8 b = quint8(word.at(i));
        if (i < 24 && b != 0)
            return false;
        v = (v << 8) | b;
    }
    *out = v;
    return true;
}

enum class BalanceReply {
    Ok,         // usable; a reverted sub-call only leaves its balance unknown
    Malformed,  // not an aggregate3 answer of the expected shape
    OtherChain, // the endpoint does not serve Ethereum mainnet: use none of it
};

// The aggregate3 reply: the chain id, then per address getEthBalance and
// balanceOf on every token. known[k] stays false where the k-th balance sub-call
// reverted: that balance is unknown and the row keeps its old value.
// No number counts unless the reply shows it ran on mainnet: chain id 1, and
// every token answering with one uint256 word as USDT and USDC always do there.
// A call to an address without code "succeeds" with empty data, which is how
// the mainnet token addresses answer on another chain, and an eth_call to a
// missing Multicall3 returns no data at all. `why` then names the evidence.
BalanceReply parseBalances(const QJsonValue& result, qsizetype addresses,
                           const QList<AssetInfo>& tokens, QList<bool>* known,
                           QList<chain::Amount>* amounts, QString* why)
{
    try {
        const qsizetype stride = 1 + tokens.size();
        const qsizetype count = addresses * stride;
        QByteArray raw;
        if (!result.isString() || !net::multicall::fromHex(result.toString(), &raw))
            return BalanceReply::Malformed;
        if (raw.isEmpty()) {
            *why = QStringLiteral("no Multicall3 contract");
            return BalanceReply::OtherChain;
        }
        QList<net::multicall::Result> decoded;
        if (!net::multicall::decodeAggregate3(raw, &decoded) || decoded.size() != 1 + count)
            return BalanceReply::Malformed;

        const net::multicall::Result& id = decoded.at(0);
        quint64 chainId = 0;
        const bool readable = id.success && wordValue(id.returnData, &chainId);
        if (!readable || chainId != kMainnetChainId) {
            *why = readable ? QStringLiteral("chain id %1").arg(chainId)
                            : QStringLiteral("no valid chain id in its reply");
            return BalanceReply::OtherChain;
        }

        QList<bool> isKnown(count, false);
        QList<chain::Amount> values(count);
        for (qsizetype k = 0; k < count; ++k) {
            const net::multicall::Result& r = decoded.at(1 + k);
            if (!r.success)
                continue;
            if (r.returnData.size() != 32) {
                const qsizetype slot = k % stride; // 0: getEthBalance, else tokens[slot - 1]
                if (slot == 0)
                    return BalanceReply::Malformed; // Multicall3 itself always answers one word
                *why = QStringLiteral("%1 contract missing").arg(tokens.at(slot - 1).symbol);
                return BalanceReply::OtherChain;
            }
            const auto* p = reinterpret_cast<const uint8_t*>(r.returnData.constData());
            values[k] = chain::Amount::fromBigEndian(chain::Bytes(p, p + 32));
            isKnown[k] = true;
        }
        *known = isKnown;
        *amounts = values;
        return BalanceReply::Ok;
    } catch (const std::exception&) {
        return BalanceReply::Malformed;
    }
}

bool anyBalance(const AddressRow& row)
{
    if (!row.ethWei.isZero())
        return true;
    for (auto it = row.tokens.cbegin(); it != row.tokens.cend(); ++it)
        if (!it.value().isZero())
            return true;
    return false;
}

// Only the scan fields change here; skipping identical rows keeps a refresh
// pass from repainting the whole table every minute.
bool scanFieldsDiffer(const AddressRow& a, const AddressRow& b, const QList<AssetInfo>& tokens)
{
    if (a.checked != b.checked || a.used != b.used || a.nonce != b.nonce || a.ethWei != b.ethWei)
        return true;
    for (const AssetInfo& t : tokens)
        if (a.tokens.value(t.asset) != b.tokens.value(t.asset))
            return true;
    return false;
}

// Server error texts end up in the UI: one short line.
QString clip(const QString& text)
{
    QString s = text.simplified();
    if (s.size() > 160)
        s = s.left(159) + QStringLiteral("…");
    return s;
}

} // namespace

EthScanner::EthScanner(net::EthRpc* rpc, WalletModel* model, QObject* parent)
    : QObject(parent)
    , rpc_(rpc)
    , model_(model)
{
    // Single-shot and re-armed when a pass ends, so passes never overlap and
    // the interval is measured from the end of the previous one.
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        if (running_ && !passActive_)
            startPass();
    });

    retryTimer_ = new QTimer(this);
    retryTimer_->setSingleShot(true);
    retryTimer_->setInterval(kRetryMs);
    connect(retryTimer_, &QTimer::timeout, this, [this] {
        if (running_ && passActive_ && pendingSeq_ == 0 && !chunk_.isEmpty())
            sendNonces();
    });

    progressTimer_ = new QTimer(this);
    progressTimer_->setSingleShot(true);
    progressTimer_->setInterval(kProgressMs);
    connect(progressTimer_, &QTimer::timeout, this, [this] {
        if (running_)
            emitProgress();
    });

    // The window assumes its rows exist; after a clear, start over. A stop() or
    // start() the controller issues right after supersedes this restart, since
    // the scan itself only begins on the next event-loop turn.
    if (model_) {
        connect(model_, &WalletModel::cleared, this, [this] {
            if (running_)
                start();
        });
    }
}

void EthScanner::setAccounts(const QList<AccountKey>& accounts)
{
    // Applies at the next start(); a running scan works on its own copy.
    accounts_.clear();
    for (const AccountKey& key : accounts) {
        // A second key for the same account number would fight over its rows.
        if (key.type != chain::ScriptType::EthAccount || ownsAccount(key.account))
            continue;
        accounts_.append(key);
    }
}

void EthScanner::setGap(int gap)
{
    gap_ = qBound(1, gap, kMaxGap);
}

void EthScanner::setRefreshSeconds(int seconds)
{
    refreshSeconds_ = qBound(kMinRefreshSeconds, seconds, kMaxRefreshSeconds);
    if (refreshTimer_->isActive())
        refreshTimer_->start(refreshSeconds_ * 1000);
}

void EthScanner::start()
{
    stopWork();
    if (!rpc_ || !model_) {
        running_ = false;
        return;
    }
    running_ = true;
    finished_ = false;
    passActive_ = true; // counts as a pass already, so refreshNow() waits for it
    reported_.clear();
    lastChecked_ = -1;
    lastDerived_ = -1;
    // Deferred: the caller finishes its own setup before any signal arrives,
    // and a stop()/start() issued right after cancels this one cleanly.
    defer(&EthScanner::beginScan);
}

void EthScanner::stop()
{
    stopWork();
    running_ = false;
    finished_ = false;
}

void EthScanner::refreshNow()
{
    if (!running_ || passActive_)
        return;
    refreshTimer_->stop();
    startPass();
}

bool EthScanner::isScanning() const
{
    return running_ && !finished_;
}

int EthScanner::checkedCount() const
{
    int checked = 0, derived = 0;
    countRows(&checked, &derived);
    return checked;
}

int EthScanner::derivedCount() const
{
    int checked = 0, derived = 0;
    countRows(&checked, &derived);
    return derived;
}

// ------------------------------------------------------------ the window ----

void EthScanner::beginScan()
{
    const QList<AssetInfo> erc20 = erc20Assets();
    tokens_.clear();
    for (const AssetInfo& t : erc20)
        if (!t.contract.isEmpty())
            tokens_.append(t);

    acc_.clear();
    for (const AccountKey& key : std::as_const(accounts_)) {
        AccountState a;
        a.key = key;
        try {
            a.chainNode = key.node().deriveChild(0);
        } catch (const std::exception&) {
            // The parser's own text is not passed on: it could quote the key.
            report(QStringLiteral("Ethereum account %1: the stored public key could not be read")
                       .arg(key.account));
            continue;
        }
        acc_.append(a);
    }

    const quint64 gen = gen_;
    for (int i = 0; i < acc_.size(); ++i) {
        adoptRows(i);
        if (gen != gen_)
            return; // a model slot restarted or stopped the scan
    }
    startPass();
}

// Rows of this account already in the model (a restart with new settings) are
// reused: they size the window at once and need no derivation.
void EthScanner::adoptRows(int i)
{
    AccountState& a = acc_[i];
    qint64 maxIndex = -1;
    qint64 lowest = -1;
    QString lowestAddress;
    for (int r = 0; r < model_->size(); ++r) {
        const AddressRow& row = model_->at(r);
        if (row.type != chain::ScriptType::EthAccount || row.account != a.key.account
            || row.change != 0)
            continue;
        const qint64 index = row.index;
        maxIndex = qMax(maxIndex, index);
        if (row.used)
            a.lastUsed = qMax(a.lastUsed, index);
        if (lowest < 0 || index < lowest) {
            lowest = index;
            lowestAddress = row.address;
        }
    }
    if (maxIndex < 0)
        return;

    // Rows of another key (a wallet replaced without clearing the model) would
    // show someone else's balances; re-deriving one row tells them apart.
    bool replace = false;
    AddressRow probe;
    QString problem;
    if (!deriveRow(a, quint32(lowest), &probe, &problem)) {
        report(problem);
    } else if (probe.address.compare(lowestAddress, Qt::CaseInsensitive) != 0) {
        replace = true;
        a.lastUsed = -1;
    }
    ensureRows(i, 0, maxIndex + 1, replace, false); // also fills any gap in the indices
}

bool EthScanner::deriveRow(const AccountState& account, quint32 index, AddressRow* out,
                           QString* problem) const
{
    try {
        const chain::AddressInfo info = chain::addressFromChainNode(
            account.chainNode, chain::ScriptType::EthAccount, account.key.account, 0, index);
        AddressRow row;
        row.type = chain::ScriptType::EthAccount;
        row.account = account.key.account;
        row.change = 0;
        row.index = index;
        row.path = QString::fromStdString(info.path);
        row.address = QString::fromStdString(info.address);
        *out = row;
        return true;
    } catch (const std::exception& e) {
        *problem = QStringLiteral("Ethereum account %1: address %2 could not be derived (%3)")
                       .arg(account.key.account)
                       .arg(index)
                       .arg(QString::fromUtf8(e.what()));
        return false;
    }
}

// Makes rows [from, to) of account i exist in the model, deriving the missing
// ones (or, with `replace`, all of them), and optionally queues every one of
// them for the current pass.
void EthScanner::ensureRows(int i, qint64 from, qint64 to, bool replace, bool enqueue)
{
    const quint64 gen = gen_;
    for (qint64 index = from; index < to; ++index) {
        const quint32 idx = quint32(index);
        if (replace
            || model_->find(chain::ScriptType::EthAccount, acc_.at(i).key.account, 0, idx) < 0) {
            AddressRow row;
            QString problem;
            if (!deriveRow(acc_.at(i), idx, &row, &problem)) {
                // An underivable index is possible in BIP32 (odds ~2^-127) and
                // other failures are permanent: the window ends here, visibly.
                acc_[i].limit = index;
                report(problem);
                return;
            }
            model_->upsert(row);
            if (gen != gen_)
                return; // a model slot restarted or stopped the scan
        }
        if (enqueue)
            queue_.append(Slot{i, idx, QString()});
        acc_[i].end = qMax(acc_.at(i).end, index + 1);
    }
}

// target = lastUsed + 1 + gap: derives (and, during a pass, queues) the rows
// the window gained since it was last computed.
void EthScanner::extendWindow(int i, bool enqueue)
{
    const AccountState& a = acc_.at(i);
    const qint64 target = qMin(a.lastUsed + 1 + gap_, a.limit);
    if (target > a.end)
        ensureRows(i, a.end, target, false, enqueue);
}

// -------------------------------------------------------------- a pass ----

// The initial scan and every refresh are the same pass: all known rows of the
// window, checked chunk by chunk, one chunk in flight at a time.
void EthScanner::startPass()
{
    const quint64 gen = gen_;
    passActive_ = true;
    passFailed_ = false;
    queue_.clear();
    queueHead_ = 0;
    for (int i = 0; i < acc_.size(); ++i) {
        extendWindow(i, false);
        if (gen != gen_)
            return;
    }
    for (int i = 0; i < acc_.size(); ++i)
        for (qint64 index = 0; index < acc_.at(i).end; ++index)
            queue_.append(Slot{i, quint32(index), QString()});
    noteProgress();
    defer(&EthScanner::nextChunk);
}

void EthScanner::nextChunk()
{
    chunk_.clear();
    attempts_ = 0;
    while (queueHead_ < queue_.size() && chunk_.size() < kChunk) {
        Slot s = queue_.at(queueHead_++);
        const int pos = model_->find(chain::ScriptType::EthAccount, acc_.at(s.account).key.account,
                                     0, s.index);
        if (pos < 0)
            continue;
        s.address = model_->at(pos).address;
        chunk_.append(s);
    }
    if (chunk_.isEmpty()) {
        finishPass();
        return;
    }
    sendNonces();
}

// (a) One JSON-RPC batch: eth_getTransactionCount of every address in the chunk.
void EthScanner::sendNonces()
{
    chunkErrors_.clear();
    QList<QPair<QString, QJsonArray>> calls;
    calls.reserve(chunk_.size());
    for (const Slot& s : std::as_const(chunk_))
        calls.append(qMakePair(QStringLiteral("eth_getTransactionCount"),
                               QJsonArray{s.address, QStringLiteral("latest")}));

    const qsizetype n = chunk_.size();
    const quint64 gen = gen_;
    const quint64 seq = ++nextSeq_;
    pendingSeq_ = seq;
    QPointer<EthScanner> self(this);
    rpc_->batch(calls, [self, gen, seq, n](const QList<QJsonValue>& results,
                                           const QStringList& errors) {
        if (!self || !self->acceptReply(gen, seq))
            return;
        QList<quint64> nonces(n, quint64(0));
        QList<bool> ok(n, false);
        QStringList problems;
        for (qsizetype i = 0; i < n; ++i) {
            const QString err = i < errors.size() ? errors.at(i) : QStringLiteral("no reply");
            if (err.isEmpty() && i < results.size() && parseQuantity(results.at(i), &nonces[i]))
                ok[i] = true;
            else if (!err.isEmpty())
                problems << QStringLiteral("Ethereum nonce lookup failed: %1").arg(clip(err));
            else
                problems << QStringLiteral("Ethereum nonce lookup failed: unexpected reply");
        }
        problems.removeDuplicates();
        self->onNonces(nonces, ok, problems);
    });
}

void EthScanner::onNonces(const QList<quint64>& nonces, const QList<bool>& ok,
                          const QStringList& problems)
{
    nonce_ = nonces;
    nonceOk_ = ok;
    for (const QString& p : problems)
        noteFailure(p);
    // (b) runs even when (a) failed: whatever balances arrive are still shown.
    defer(&EthScanner::sendBalances);
}

// (b) One eth_call to Multicall3.aggregate3: getChainId first, then per address
// getEthBalance plus balanceOf on every ERC-20 contract, in that order.
void EthScanner::sendBalances()
{
    const QString multicall = QString::fromLatin1(net::multicall::kAddress);
    const qsizetype n = chunk_.size();
    const qsizetype count = n * (1 + tokens_.size()); // balance sub-calls, after the chain id
    QString data;
    bool encoded = true;
    try {
        QList<net::multicall::Call> calls;
        calls.reserve(1 + count);
        calls.append(net::multicall::Call{multicall, chainIdCall()});
        for (const Slot& s : std::as_const(chunk_)) {
            calls.append(net::multicall::Call{multicall,
                                              net::multicall::encodeGetEthBalance(s.address)});
            for (const AssetInfo& t : std::as_const(tokens_))
                calls.append(net::multicall::Call{t.contract,
                                                  net::multicall::encodeBalanceOf(s.address)});
        }
        // The encoders report a bad address with empty output; a request built
        // from it would only fail on chain, so it is never sent.
        for (const net::multicall::Call& c : std::as_const(calls))
            encoded = encoded && !c.callData.isEmpty();
        const QByteArray aggregate = encoded ? net::multicall::encodeAggregate3(calls) : QByteArray();
        encoded = !aggregate.isEmpty();
        data = net::multicall::toHex(aggregate);
    } catch (const std::exception&) {
        encoded = false;
    }
    if (!encoded) {
        applyChunk(QList<bool>(count, false), QList<chain::Amount>(count),
                   {QStringLiteral("Ethereum balance lookup failed: the Multicall3 request "
                                   "could not be encoded")},
                   false);
        return;
    }

    QJsonObject tx;
    tx.insert(QStringLiteral("to"), multicall);
    tx.insert(QStringLiteral("data"), data);
    const quint64 gen = gen_;
    const quint64 seq = ++nextSeq_;
    pendingSeq_ = seq;
    QPointer<EthScanner> self(this);
    rpc_->call(QStringLiteral("eth_call"), QJsonArray{tx, QStringLiteral("latest")},
               [self, gen, seq, n, tokens = tokens_](const QJsonValue& result,
                                                     const QString& rpcError) {
                   if (!self || !self->acceptReply(gen, seq))
                       return;
                   const qsizetype count = n * (1 + tokens.size());
                   QList<bool> known(count, false);
                   QList<chain::Amount> amounts(count);
                   QStringList problems;
                   bool otherChain = false;
                   QString why;
                   if (!rpcError.isEmpty()) {
                       problems << QStringLiteral("Ethereum balance lookup failed: %1")
                                       .arg(clip(rpcError));
                   } else {
                       switch (parseBalances(result, n, tokens, &known, &amounts, &why)) {
                       case BalanceReply::Ok:
                           if (known.contains(false))
                               problems << QStringLiteral(
                                   "Ethereum balance lookup: some balance calls failed");
                           break;
                       case BalanceReply::Malformed:
                           problems << QStringLiteral(
                               "Ethereum balance lookup failed: unexpected Multicall3 reply");
                           break;
                       case BalanceReply::OtherChain:
                           // Settings accept any https URL; this is how a BSC,
                           // Polygon, L2 or testnet endpoint gets noticed.
                           otherChain = true;
                           problems << QStringLiteral(
                                           "Ethereum: the RPC endpoint does not look like "
                                           "Ethereum mainnet (%1), so its answers are ignored; "
                                           "check the Ethereum endpoints in Settings")
                                           .arg(why);
                           break;
                       }
                   }
                   self->applyChunk(known, amounts, problems, otherChain);
               });
}

// Writes what the attempt learned into the rows, grows the window past newly
// used addresses, then moves on, or schedules a retry of the rows still missing
// an answer. With `otherChain` the nonces of the attempt are dropped as well:
// (a) went to the same endpoint unless EthRpc failed over in between, and an
// address used on another chain must not read as used (sticky) here.
void EthScanner::applyChunk(const QList<bool>& known, const QList<chain::Amount>& amounts,
                            const QStringList& problems, bool otherChain)
{
    for (const QString& p : problems)
        noteFailure(p);

    const quint64 gen = gen_;
    const qsizetype stride = 1 + tokens_.size();
    const QList<Slot> attempt = chunk_;
    QList<Slot> failed;
    QList<int> grown; // accounts whose last used index moved up

    for (qsizetype i = 0; i < attempt.size(); ++i) {
        const Slot& s = attempt.at(i);
        const int pos = model_->find(chain::ScriptType::EthAccount, acc_.at(s.account).key.account,
                                     0, s.index);
        if (pos < 0 || model_->at(pos).address.compare(s.address, Qt::CaseInsensitive) != 0)
            continue; // replaced or removed while the request was out
        const AddressRow old = model_->at(pos);
        AddressRow row = old;

        bool ok = !otherChain && nonceOk_.value(i, false);
        if (ok)
            row.nonce = nonce_.at(i);
        const qsizetype base = i * stride;
        for (qsizetype k = 0; k < stride; ++k) {
            if (!known.value(base + k, false)) {
                ok = false; // unknown: keep the old value
                continue;
            }
            if (k == 0)
                row.ethWei = amounts.at(base);
            else
                row.tokens[tokens_.at(k - 1).asset] = amounts.at(base + k);
        }
        // A failed attempt never downgrades a row: what an earlier pass
        // confirmed stays shown until a newer answer replaces it.
        row.checked = old.checked || ok;
        // Sticky, so the window never shrinks behind an address that was used.
        row.used = old.used || row.nonce > 0 || anyBalance(row);

        if (scanFieldsDiffer(row, old, tokens_)) {
            model_->upsert(row);
            if (gen != gen_)
                return; // a model slot restarted or stopped the scan
        }
        AccountState& a = acc_[s.account];
        if (row.used && qint64(s.index) > a.lastUsed) {
            a.lastUsed = s.index;
            if (!grown.contains(s.account))
                grown.append(s.account);
        }
        if (!ok)
            failed.append(s);
    }

    for (int a : std::as_const(grown)) {
        extendWindow(a, true);
        if (gen != gen_)
            return;
    }
    noteProgress();

    if (failed.isEmpty()) {
        chunk_.clear();
        defer(&EthScanner::nextChunk);
        return;
    }

    passFailed_ = true;
    QStringList messages = chunkErrors_;
    if (attempts_ < kMaxRetries) {
        ++attempts_;
        chunk_ = failed; // only the rows still missing an answer
        retryTimer_->start();
    } else {
        // Skipped for this pass: the rows stay unchecked (or keep their last
        // confirmed values) and the next refresh pass tries them again.
        chunk_.clear();
        defer(&EthScanner::nextChunk);
        messages << QStringLiteral("Ethereum: some addresses could not be checked after %1 "
                                   "retries; the next refresh tries them again")
                        .arg(kMaxRetries);
    }
    for (const QString& m : std::as_const(messages))
        report(m);
}

void EthScanner::finishPass()
{
    passActive_ = false;
    queue_.clear();
    queueHead_ = 0;
    chunk_.clear();
    // A pass without failures ends an outage: the next one is reported again
    // instead of staying silent for the rest of the session.
    if (!passFailed_)
        reported_.clear();
    if (!acc_.isEmpty())
        refreshTimer_->start(refreshSeconds_ * 1000);

    // Skipped chunks leave the window incomplete; the refresh passes keep
    // retrying them, and the scan counts as finished once nothing is missing.
    if (!finished_ && windowChecked()) {
        finished_ = true;
        progressTimer_->stop();
        const quint64 gen = gen_;
        emitProgress(); // the final counts before scanFinished()
        if (gen == gen_)
            emit scanFinished();
    }
}

bool EthScanner::windowChecked() const
{
    for (const AccountState& a : acc_) {
        const qint64 target = qMin(a.lastUsed + 1 + gap_, a.limit);
        for (qint64 index = 0; index < target; ++index) {
            const int pos = model_->find(chain::ScriptType::EthAccount, a.key.account, 0,
                                         quint32(index));
            if (pos < 0 || !model_->at(pos).checked)
                return false;
        }
    }
    return true;
}

// ----------------------------------------------------------- plumbing ----

// A reply counts only if it belongs to this generation and is the request
// currently awaited; a late or repeated callback is dropped.
bool EthScanner::acceptReply(quint64 gen, quint64 seq)
{
    if (gen != gen_ || seq != pendingSeq_)
        return false;
    pendingSeq_ = 0;
    return true;
}

// Steps run from the event loop, never inside an EthRpc callback, and only
// while the generation that scheduled them is current.
void EthScanner::defer(void (EthScanner::*step)())
{
    const quint64 gen = gen_;
    QTimer::singleShot(0, this, [this, gen, step] {
        if (gen == gen_)
            (this->*step)();
    });
}

void EthScanner::stopWork()
{
    ++gen_;
    pendingSeq_ = 0;
    refreshTimer_->stop();
    retryTimer_->stop();
    progressTimer_->stop();
    acc_.clear();
    queue_.clear();
    queueHead_ = 0;
    chunk_.clear();
    nonce_.clear();
    nonceOk_.clear();
    chunkErrors_.clear();
    attempts_ = 0;
    passActive_ = false;
    passFailed_ = false;
}

void EthScanner::noteFailure(const QString& message)
{
    if (!chunkErrors_.contains(message))
        chunkErrors_.append(message);
}

// Each distinct text is emitted once, from the event loop, so a slot that
// reacts by restarting the scanner never runs in the middle of a step.
void EthScanner::report(const QString& message)
{
    if (message.isEmpty() || reported_.contains(message))
        return;
    if (reported_.size() >= kMaxReported)
        reported_.clear(); // texts with varying details must not pile up forever
    reported_.append(message);
    const quint64 gen = gen_;
    QTimer::singleShot(0, this, [this, gen, message] {
        if (gen == gen_)
            emit error(message);
    });
}

// A chunk updates up to 50 rows at once: coalesce into one signal per 250 ms.
void EthScanner::noteProgress()
{
    if (!progressTimer_->isActive())
        progressTimer_->start();
}

void EthScanner::emitProgress()
{
    int checked = 0, derived = 0;
    countRows(&checked, &derived);
    if (checked == lastChecked_ && derived == lastDerived_)
        return;
    lastChecked_ = checked;
    lastDerived_ = derived;
    emit progress(checked, derived);
}

void EthScanner::countRows(int* checked, int* derived) const
{
    *checked = 0;
    *derived = 0;
    if (!model_)
        return;
    for (int r = 0; r < model_->size(); ++r) {
        const AddressRow& row = model_->at(r);
        if (row.type != chain::ScriptType::EthAccount || row.change != 0
            || !ownsAccount(row.account))
            continue;
        ++*derived;
        if (row.checked)
            ++*checked;
    }
}

bool EthScanner::ownsAccount(quint32 account) const
{
    for (const AccountKey& key : accounts_)
        if (key.account == account)
            return true;
    return false;
}

} // namespace wallet
