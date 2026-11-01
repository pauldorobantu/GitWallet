#include "wallet/EthScanner.h"

#include "chain/Hash.h"
#include "net/EthRpc.h"
#include "net/Multicall.h"
#include "wallet/WalletModel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QPointer>
#include <QTimer>

#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace wallet {

namespace {

// 49 addresses per attempt. (a) is one JSON-RPC batch of 50 calls, eth_chainId
// and then the nonces: EthRpc posts up to kMaxBatch calls as a single HTTP
// request and fails over with the whole of it, so one endpoint answers every
// call of the batch. (b) is one eth_call with 148 Multicall3 sub-calls (~1M
// gas): the chain-id proof, then three balances per address.
constexpr int kChunk = 49;
static_assert(kChunk + 1 <= net::EthRpc::kMaxBatch,
              "eth_chainId and the nonces of a chunk must fit one JSON-RPC batch");

constexpr int kMaxRetries = 3; // then the chunk is skipped until the next pass
constexpr int kRetryMs = 10'000;
constexpr int kProgressMs = 250;
constexpr int kMaxGap = 100'000;
// Rows derived per account at most. Every used address pushes the window `gap`
// rows further and the model never drops a row, so an endpoint that calls every
// address used (a nonce of 1, or 1 wei, for each) would otherwise grow the
// wallet in a pass that never ends, until memory runs out. Far above what a
// shop seed needs (the controller's largest gap is 5,000), and well below 2^31,
// where non-hardened BIP32 indices end.
constexpr qint64 kMaxEthRows = 100'000;
// Rows derived over all accounts together: wallets add up, and a pass over this
// many already takes most of an hour at one chunk at a time.
constexpr qint64 kMaxEthTotalRows = 200'000;
// Rows derived per event-loop turn when a pass sets up its windows: BtcScanner's
// slice, so that many wallets or a large gap never freeze the window.
constexpr qint64 kDeriveSlice = 250;
// A nonce counts the transactions an address has sent. No mainnet address comes
// anywhere near 2^32 of them, so a larger one is a malformed reply, not proof
// that the address was used.
constexpr quint64 kMaxPlausibleNonce = quint64(1) << 32;
constexpr int kMinRefreshSeconds = 10; // public endpoints rate-limit; never poll faster
constexpr int kMaxRefreshSeconds = 24 * 3600;
constexpr qsizetype kMaxReported = 64;      // distinct messages remembered at once
constexpr qsizetype kMaxServerDetail = 200; // characters of server text a problem() carries
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

// What an endpoint sent (EthRpc's failure texts quote it), as problem() passes
// it on: kept apart from GitWallet's own sentence and cleaned up, one line of
// at most kMaxServerDetail characters without control or format characters
// (bidi overrides, zero-width marks) that would make it read other than it is.
QString serverDetail(const QString& text)
{
    const QList<uint> chars = text.left(4 * kMaxServerDetail).toUcs4(); // lone surrogates read U+FFFD
    QList<char32_t> kept;
    kept.reserve(chars.size());
    for (const uint c : chars) {
        if (QChar::isSpace(c))
            kept.append(U' ');
        else if (QChar::category(c) != QChar::Other_Control && QChar::category(c) != QChar::Other_Format)
            kept.append(char32_t(c));
    }
    QString line = QString::fromUcs4(kept.constData(), kept.size()).simplified();
    if (line.size() > kMaxServerDetail) {
        line.truncate(kMaxServerDetail - 1);
        if (line.back().isHighSurrogate())
            line.chop(1); // never half a character
        line += QStringLiteral("…");
    }
    return line;
}

// Settings accept any https URL; this is how a BSC, Polygon, L2 or testnet
// endpoint gets noticed. Both requests of an attempt word it alike, so an
// endpoint that fails both proofs is reported once.
QString notMainnet(const QString& why)
{
    return QStringLiteral("Ethereum: the RPC endpoint does not look like Ethereum mainnet (%1), "
                          "so its answers are ignored; check the Ethereum endpoints in Settings")
        .arg(why);
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
    // the scan itself only begins on the next event-loop turn. The balances
    // that balancesAt() dated are gone with the rows, stopped or not, unless other
    // wallets' rows stayed (one wallet removed): those figures are as old as ever.
    if (model_) {
        connect(model_, &WalletModel::cleared, this, [this] {
            bool rowsLeft = false;
            for (int r = 0; r < model_->size() && !rowsLeft; ++r)
                rowsLeft = model_->at(r).type == chain::ScriptType::EthAccount;
            if (!rowsLeft) {
                balancesAt_ = QDateTime();
                refreshFailing_ = false;
            }
            if (running_)
                start();
        });
    }
}

void EthScanner::setAccounts(const QList<AccountKey>& accounts)
{
    // Applies at the next start(); a running scan works on its own copy.
    accounts_.clear();
    owned_.clear();
    for (const AccountKey& key : accounts) {
        // A second key for the same account number of a wallet would fight over its rows.
        if (key.type != chain::ScriptType::EthAccount || ownsAccount(key.wallet, key.account))
            continue;
        accounts_.append(key);
        owned_.insert({key.wallet, key.account});
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

bool EthScanner::limitReached() const
{
    if (totalCut_)
        return true;
    for (const AccountState& a : acc_)
        if (cutAtLimit(a))
            return true;
    return false;
}

bool EthScanner::wrongChain() const
{
    // Judged by the endpoint requests go to now: once EthRpc has moved on from
    // a rejected one, the answers of the next decide.
    return rpc_ && !offMainnet_.isEmpty() && offMainnet_.contains(rpc_->currentEndpoint());
}

QDateTime EthScanner::balancesAt() const
{
    return balancesAt_;
}

bool EthScanner::refreshFailing() const
{
    return refreshFailing_;
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
            report({Problem::Skipped,
                    QStringLiteral("Ethereum account %1: the stored public key could not be read")
                        .arg(key.account),
                    QString()});
            continue;
        }
        acc_.append(a);
    }

    totalRows_ = 0;
    totalCut_ = false;
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
        if (row.type != chain::ScriptType::EthAccount || row.wallet != a.key.wallet
            || row.account != a.key.account
            || row.change != 0 || qint64(row.index) >= kMaxEthRows) // past the cap: never derived here
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
    QString why;
    if (!deriveRow(a, quint32(lowest), &probe, &why)) {
        report({Problem::Skipped, why, QString()});
    } else if (probe.address.compare(lowestAddress, Qt::CaseInsensitive) != 0) {
        replace = true;
        a.lastUsed = -1;
        balancesAt_ = QDateTime(); // it dated the other key's balances
    }
    ensureRows(i, 0, maxIndex + 1, replace, false); // also fills any gap in the indices
}

bool EthScanner::deriveRow(const AccountState& account, quint32 index, AddressRow* out,
                           QString* why) const
{
    try {
        const chain::AddressInfo info = chain::addressFromChainNode(
            account.chainNode, chain::ScriptType::EthAccount, account.key.account, 0, index);
        AddressRow row;
        row.wallet = account.key.wallet;
        row.type = chain::ScriptType::EthAccount;
        row.account = account.key.account;
        row.change = 0;
        row.index = index;
        row.path = QString::fromStdString(info.path);
        row.address = QString::fromStdString(info.address);
        *out = row;
        return true;
    } catch (const std::exception& e) {
        *why = QStringLiteral("Ethereum account %1: address %2 could not be derived (%3)")
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
        if (totalRows_ >= kMaxEthTotalRows) {
            // Like the limit per account: what exists stays checked, the rest is
            // not derived, and the scan stays unfinished instead of looking whole.
            totalCut_ = true;
            static const QLocale english(QLocale::English, QLocale::UnitedStates);
            report({Problem::Limit,
                    QStringLiteral("Ethereum scan stopped at %1 addresses, the most GitWallet follows, shared by "
                                   "all wallets. Lower the Ethereum gap in Settings or remove wallets you do not "
                                   "need here.")
                        .arg(english.toString(kMaxEthTotalRows)),
                    QString()});
            return;
        }
        if (replace
            || model_->find(acc_.at(i).key.wallet, chain::ScriptType::EthAccount, acc_.at(i).key.account,
                            0, idx) < 0) {
            AddressRow row;
            QString why;
            if (!deriveRow(acc_.at(i), idx, &row, &why)) {
                // An underivable index is possible in BIP32 (odds ~2^-127) and
                // other failures are permanent: the window ends here, visibly,
                // and the addresses past it are never scanned.
                acc_[i].limit = index;
                report({Problem::Skipped, why, QString()});
                return;
            }
            model_->upsert(row);
            if (gen != gen_)
                return; // a model slot restarted or stopped the scan
        }
        if (enqueue)
            queue_.append(Slot{i, idx, QString()});
        if (index + 1 > acc_.at(i).end) {
            totalRows_ += index + 1 - acc_.at(i).end;
            acc_[i].end = index + 1;
        }
    }
}

// lastUsed + 1 + gap, but never past an index that failed to derive, nor past
// kMaxEthRows.
qint64 EthScanner::windowEnd(const AccountState& account) const
{
    return qMin(qMin(account.lastUsed + 1 + gap_, account.limit), kMaxEthRows);
}

// The rows the window wants past kMaxEthRows are neither derived nor checked.
bool EthScanner::cutAtLimit(const AccountState& account) const
{
    return qMin(account.lastUsed + 1 + gap_, account.limit) > kMaxEthRows;
}

// Derives (and, during a pass, queues) the rows the window gained since it was
// last computed.
void EthScanner::extendWindow(int i, bool enqueue)
{
    extendWindowTo(i, enqueue, windowEnd(acc_.at(i)));
}

void EthScanner::extendWindowTo(int i, bool enqueue, qint64 upTo)
{
    AccountState& a = acc_[i];
    if (!a.capReported && cutAtLimit(a)) {
        // Once per account and start(). The passes still check every row up to
        // the limit, so the rows shown stay right.
        a.capReported = true;
        static const QLocale english(QLocale::English, QLocale::UnitedStates); // "100,000", as the UI groups
        const QString where = QString::fromStdString(
                                  chain::accountRootPath(chain::ScriptType::EthAccount, a.key.account))
                              + QStringLiteral("/0");
        report({Problem::Limit,
                QStringLiteral("Stopped at %1 addresses on %2, the limit per account: the Ethereum "
                               "endpoint reports addresses that far out as used.")
                    .arg(english.toString(kMaxEthRows), where),
                QString()});
    }
    if (upTo > a.end)
        ensureRows(i, a.end, upTo, false, enqueue);
}

// -------------------------------------------------------------- a pass ----

// The initial scan and every refresh are the same pass: all known rows of the
// window, checked chunk by chunk, one chunk in flight at a time.
void EthScanner::startPass()
{
    passActive_ = true;
    passFailed_ = false;
    passSkipped_ = false;
    queue_.clear();
    queueHead_ = 0;
    deriving_ = 0;
    deriveWindows();
}

// Brings every window up to date, kDeriveSlice rows per event-loop turn, then
// queues the whole pass. A slice that does not finish them comes back next turn.
void EthScanner::deriveWindows()
{
    const quint64 gen = gen_;
    qint64 budget = kDeriveSlice;
    while (deriving_ < acc_.size() && budget > 0 && !totalCut_) {
        const int i = deriving_;
        const qint64 before = acc_.at(i).end;
        extendWindowTo(i, false, qMin(windowEnd(acc_.at(i)), before + budget));
        if (gen != gen_)
            return; // a model slot restarted or stopped the scan
        budget -= acc_.at(i).end - before;
        // Whole, or ended early at an index that cannot be derived: on to the next.
        if (acc_.at(i).end >= windowEnd(acc_.at(i)))
            ++deriving_;
    }
    if (deriving_ < acc_.size() && !totalCut_) {
        noteProgress();
        defer(&EthScanner::deriveWindows);
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
        const AccountKey& key = acc_.at(s.account).key;
        const int pos = model_->find(key.wallet, chain::ScriptType::EthAccount, key.account, 0, s.index);
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

// (a) One JSON-RPC batch: eth_chainId, then eth_getTransactionCount of every
// address in the chunk. The batch is a single HTTP request (see kChunk), so its
// chain id proves where its nonces come from, whichever endpoint answered it;
// the eth_call of (b) cannot, since EthRpc may fail over between the two. No
// nonce counts without chain id 1: one read on another chain would mark the
// address used here (sticky) and push the window. EthRpc itself withholds the
// answers of an endpoint whose eth_chainId is not 1 and replies with an error
// instead (EthRpc::isOffChain() tells it apart), so the chain id check here is
// the second line. Endpoint failure texts quote the endpoint: they go out as
// the server detail only.
void EthScanner::sendNonces()
{
    chunkIssues_.clear();
    QList<QPair<QString, QJsonArray>> calls;
    calls.reserve(1 + chunk_.size());
    calls.append(qMakePair(QStringLiteral("eth_chainId"), QJsonArray()));
    for (const Slot& s : std::as_const(chunk_))
        calls.append(qMakePair(QStringLiteral("eth_getTransactionCount"),
                               QJsonArray{s.address, QStringLiteral("latest")}));

    const qsizetype n = chunk_.size();
    const quint64 gen = gen_;
    const quint64 seq = ++nextSeq_;
    pendingSeq_ = seq;
    QPointer<EthScanner> self(this);
    rpc_->batch(calls, [self, gen, seq, n](const QList<QJsonValue>& results, const QStringList& errors,
                                           const QStringList& endpoints) {
        if (!self || !self->acceptReply(gen, seq))
            return;
        QList<quint64> nonces(n, quint64(0));
        QList<bool> ok(n, false);
        QList<Issue> issues;
        const QString idError = errors.value(0, QStringLiteral("no reply"));
        quint64 chainId = 0;
        if (!idError.isEmpty() && self->rpc_->isOffChain(endpoints.value(0))) {
            // EthRpc already skips it; recorded here so wrongChain() can say so.
            if (!self->offMainnet_.contains(endpoints.value(0)))
                self->offMainnet_.append(endpoints.value(0));
            issues << Issue{Problem::WrongChain, notMainnet(QStringLiteral("eth_chainId is not 1")),
                            serverDetail(idError)};
        } else if (!idError.isEmpty()) {
            issues << Issue{Problem::Failure, QStringLiteral("Ethereum nonce lookup failed"),
                            serverDetail(idError)};
        } else if (!parseQuantity(results.value(0), &chainId)) {
            issues << Issue{Problem::Failure,
                            QStringLiteral("Ethereum nonce lookup failed: no valid chain id in the reply"),
                            QString()};
        } else if (chainId != kMainnetChainId) {
            issues << Issue{Problem::WrongChain, notMainnet(QStringLiteral("chain id %1").arg(chainId)),
                            QString()};
            self->refuseEndpoint(endpoints.value(0), true);
            if (!self || gen != self->gen_)
                return; // a slot of EthRpc's signals restarted or stopped the scan
        } else {
            for (qsizetype i = 0; i < n; ++i) {
                const qsizetype call = 1 + i; // after eth_chainId
                const QString err = errors.value(call, QStringLiteral("no reply"));
                quint64 nonce = 0;
                if (err.isEmpty() && parseQuantity(results.value(call), &nonce)
                    && nonce <= kMaxPlausibleNonce) {
                    nonces[i] = nonce;
                    ok[i] = true;
                } else if (!err.isEmpty()) {
                    issues << Issue{Problem::Failure, QStringLiteral("Ethereum nonce lookup failed"),
                                    serverDetail(err)};
                } else {
                    issues << Issue{Problem::Failure,
                                    QStringLiteral("Ethereum nonce lookup failed: unexpected reply"),
                                    QString()};
                }
            }
        }
        self->onNonces(nonces, ok, issues);
    });
}

void EthScanner::onNonces(const QList<quint64>& nonces, const QList<bool>& ok,
                          const QList<Issue>& issues)
{
    nonce_ = nonces;
    nonceOk_ = ok;
    for (const Issue& issue : issues)
        noteFailure(issue);
    // (b) runs even when (a) failed: whatever balances arrive are still shown.
    defer(&EthScanner::sendBalances);
}

// (b) One eth_call to Multicall3.aggregate3: getChainId first, then per address
// getEthBalance plus balanceOf on every ERC-20 contract, in that order. An
// endpoint that answers it with something unusable, or with what shows another
// chain (or no mainnet state), may well have said chain id 1 to eth_chainId:
// only this reply gives it away, so it is rejected here, before the retry.
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
                   {Issue{Problem::Failure,
                          QStringLiteral("Ethereum balance lookup failed: the Multicall3 request "
                                         "could not be encoded"),
                          QString()}},
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
               [self, gen, seq, n, tokens = tokens_](const QJsonValue& result, const QString& rpcError,
                                                     const QString& endpoint) {
                   if (!self || !self->acceptReply(gen, seq))
                       return;
                   const qsizetype count = n * (1 + tokens.size());
                   QList<bool> known(count, false);
                   QList<chain::Amount> amounts(count);
                   QList<Issue> issues;
                   bool otherChain = false;
                   bool unusable = false;
                   QString why;
                   if (!rpcError.isEmpty()) {
                       issues << Issue{Problem::Failure, QStringLiteral("Ethereum balance lookup failed"),
                                       serverDetail(rpcError)};
                   } else {
                       switch (parseBalances(result, n, tokens, &known, &amounts, &why)) {
                       case BalanceReply::Ok:
                           self->offMainnet_.removeAll(endpoint); // it serves mainnet (again)
                           if (known.contains(false))
                               issues << Issue{Problem::Failure,
                                               QStringLiteral("Ethereum balance lookup: some balance calls failed"),
                                               QString()};
                           break;
                       case BalanceReply::Malformed:
                           unusable = true;
                           issues << Issue{Problem::Failure,
                                           QStringLiteral("Ethereum balance lookup failed: unexpected Multicall3 reply"),
                                           QString()};
                           break;
                       case BalanceReply::OtherChain:
                           otherChain = true;
                           issues << Issue{Problem::WrongChain, notMainnet(why), QString()};
                           break;
                       }
                   }
                   if (unusable || otherChain) {
                       self->refuseEndpoint(endpoint, otherChain);
                       if (!self || gen != self->gen_)
                           return; // a slot of EthRpc's signals restarted or stopped the scan
                   }
                   self->applyChunk(known, amounts, issues, otherChain);
               });
}

// Writes what the attempt learned into the rows, grows the window past newly
// used addresses, then moves on, or schedules a retry of the rows still missing
// an answer. Each request proves its own chain: nonceOk_ is only set where (a)
// itself answered chain id 1 (see sendNonces()), and the balances count only
// where (b) did. With `otherChain` the nonces are dropped as well, so an
// attempt that met a non-mainnet endpoint changes no row and is retried whole,
// on another endpoint if there is one (sendBalances() rejected that one).
void EthScanner::applyChunk(const QList<bool>& known, const QList<chain::Amount>& amounts,
                            const QList<Issue>& issues, bool otherChain)
{
    for (const Issue& issue : issues)
        noteFailure(issue);

    const quint64 gen = gen_;
    const qsizetype stride = 1 + tokens_.size();
    const QList<Slot> attempt = chunk_;
    QList<Slot> failed;
    QList<int> grown; // accounts whose last used index moved up

    for (qsizetype i = 0; i < attempt.size(); ++i) {
        const Slot& s = attempt.at(i);
        const AccountKey& key = acc_.at(s.account).key;
        const int pos = model_->find(key.wallet, chain::ScriptType::EthAccount, key.account, 0, s.index);
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
        // An index at or past kMaxEthRows never moves the window (none is queued).
        if (row.used && qint64(s.index) > a.lastUsed && qint64(s.index) < kMaxEthRows) {
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
    QList<Issue> messages = chunkIssues_;
    if (attempts_ < kMaxRetries) {
        ++attempts_;
        chunk_ = failed; // only the rows still missing an answer
        retryTimer_->start();
    } else {
        // Skipped for this pass: the rows stay unchecked (or keep their last
        // confirmed values) and the next refresh pass tries them again.
        chunk_.clear();
        defer(&EthScanner::nextChunk);
        messages << Issue{Problem::Failure,
                          QStringLiteral("Ethereum: some addresses could not be checked after %1 "
                                         "retries; the next refresh tries them again")
                              .arg(kMaxRetries),
                          QString()};
        // The pass can no longer answer every row. Said now, not when it ends:
        // during an outage a large window takes hours to fail chunk by chunk,
        // and its balances must not pass for current meanwhile. The progress()
        // noted above goes out even with the same counts, as the cue to look.
        passSkipped_ = true;
        if (!refreshFailing_) {
            refreshFailing_ = true;
            lastChecked_ = -1;
        }
    }
    for (const Issue& m : std::as_const(messages))
        report(m);
}

void EthScanner::finishPass()
{
    passActive_ = false;
    queue_.clear();
    queueHead_ = 0;
    chunk_.clear();
    // The balances are current as of now only if every row of the pass got a
    // full answer and the whole window exists; otherwise balancesAt() keeps
    // the end of the last pass that did.
    const bool windowDone = windowChecked();
    const bool complete = !passSkipped_ && windowDone;
    if (complete)
        balancesAt_ = QDateTime::currentDateTime();
    refreshFailing_ = !complete;
    // A pass without failures ends an outage: the next one is reported again
    // instead of staying silent for the rest of the session.
    if (!passFailed_)
        reported_.clear();
    if (!acc_.isEmpty())
        refreshTimer_->start(refreshSeconds_ * 1000);

    const quint64 gen = gen_;
    // Skipped chunks leave the window incomplete; the refresh passes keep
    // retrying them, and the scan counts as finished once nothing is missing.
    if (!finished_ && windowDone) {
        finished_ = true;
        progressTimer_->stop();
        emitProgress(); // the final counts before scanFinished()
        if (gen != gen_)
            return;
        emit scanFinished();
    }
    if (gen == gen_) // a slot may have restarted or stopped the scan
        emit passFinished();
}

bool EthScanner::windowChecked() const
{
    for (const AccountState& a : acc_) {
        const qint64 target = windowEnd(a);
        for (qint64 index = 0; index < target; ++index) {
            const int pos = model_->find(a.key.wallet, chain::ScriptType::EthAccount, a.key.account, 0,
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
    chunkIssues_.clear();
    attempts_ = 0;
    passActive_ = false;
    passFailed_ = false;
    passSkipped_ = false;
    offMainnet_.clear();
}

// `url` (as the reply named it) gave an answer nothing can be read from: EthRpc
// moves requests to another endpoint and tries it last for a while. `offMainnet`:
// the answer showed it does not serve Ethereum mainnet (see wrongChain()).
void EthScanner::refuseEndpoint(const QString& url, bool offMainnet)
{
    if (url.isEmpty())
        return;
    // Recorded first: endpointChanged() slots may already ask wrongChain().
    if (offMainnet && !offMainnet_.contains(url))
        offMainnet_.append(url);
    rpc_->rejectEndpoint(url);
}

void EthScanner::noteFailure(const Issue& issue)
{
    for (const Issue& noted : std::as_const(chunkIssues_))
        if (noted.message == issue.message)
            return;
    chunkIssues_.append(issue);
}

// Each distinct message is emitted once per outage, with the server detail it
// first came with, and from the event loop, so a slot that reacts by
// restarting the scanner never runs in the middle of a step. Keyed on
// GitWallet's own sentence alone: an endpoint that varies its error texts
// cannot multiply the reports.
void EthScanner::report(const Issue& issue)
{
    if (issue.message.isEmpty() || reported_.contains(issue.message))
        return;
    if (reported_.size() >= kMaxReported)
        reported_.clear(); // messages naming varying accounts or indices must not pile up forever
    reported_.append(issue.message);
    const quint64 gen = gen_;
    QTimer::singleShot(0, this, [this, gen, issue] {
        if (gen == gen_)
            emit problem(issue.kind, issue.message, issue.serverDetail);
    });
}

// A chunk updates up to kChunk rows at once: coalesce into one signal per 250 ms.
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
            || !ownsAccount(row.wallet, row.account))
            continue;
        ++*derived;
        if (row.checked)
            ++*checked;
    }
}

bool EthScanner::ownsAccount(quint32 wallet, quint32 account) const
{
    return owned_.contains({wallet, account});
}

} // namespace wallet
