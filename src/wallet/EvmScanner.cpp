#include "wallet/EvmScanner.h"

#include "chain/Hash.h"
#include "net/EthRpc.h"
#include "net/Multicall.h"
#include "wallet/WalletModel.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace wallet {

namespace {

// Endpoint failover and rate limits. Never carries a seed or a key: only server names,
// addresses and counts reach it.
Q_STATIC_LOGGING_CATEGORY(lcEvm, "gitwallet.net.rpc")

// What one attempt asks at most. (a) is one JSON-RPC batch, eth_chainId and then the
// nonces wanted, at most kMaxNonces: EthRpc posts up to kMaxBatch calls as a single HTTP
// request and fails over with the whole of it, so one endpoint answers every call of the
// batch. (b) is one eth_call with the chain-id proof, then one balance per address and
// asset, at most kMaxSubCalls. Public endpoints answered 500 calls a batch and 2,000
// sub-calls an eth_call without complaint, but limit plain calls to tens a second
// (docs/ENDPOINTS.md); others cap a batch at 3 or 10, so an attempt that fails halves
// the next one, down to kMinChunk, and answers grow it back.
constexpr int kMaxNonces = 200;
constexpr int kMaxChunk = 600;
constexpr int kMinChunk = 2;
constexpr int kMaxSubCalls = 1800;
static_assert(kMaxNonces + 1 <= net::EthRpc::kMaxBatch,
              "eth_chainId and the nonces of a chunk must fit one JSON-RPC batch");
// How long a sweep waits after the last one began: an hour, or more where reading them all
// at a pace a public endpoint keeps up takes that long.
constexpr qint64 kNoncePassMs = 3600 * 1000;
constexpr qint64 kNoncesPerSecond = 50;
// A start reads every nonce again as long as it takes a few minutes; past that it trusts
// what the database kept, for the interval above.
constexpr qint64 kReadNoncesAtStart = 10'000;
// Batches of the smallest size in a row that miss a nonce: no endpoint gives them now, and
// what a pass is for is the balances. Asked again after a while.
constexpr int kMaxNonceFailures = 3;
constexpr qint64 kNonceRetrySeconds = 600;
// Good batches after which a higher ceiling is tried: it may have been a bad moment.
constexpr int kBatchesBeforeProbe = 64;
// How far past its plan a pass may run before the figures stop passing for current.
constexpr qint64 kPassGraceMs = 60 * 1000;

constexpr int kMaxRetries = 3; // then the chunk is skipped until the next pass
constexpr int kRetryMs = 10'000;
constexpr int kProgressMs = 250;
constexpr int kMaxGap = 100'000;
// While a pass runs longer than the refresh interval, unused addresses after the last
// used one that go ahead of it in every short round, per account.
constexpr qint64 kHotAhead = 20;
// Rows derived per account at most. Every used address pushes the window `gap`
// rows further and the model never drops a row, so an endpoint that calls every
// address used (a nonce of 1, or 1 wei, for each) would otherwise grow the
// wallet in a pass that never ends, until memory runs out. Far above what a
// shop seed needs (the controller's largest gap is 5,000), and well below 2^31,
// where non-hardened BIP32 indices end.
constexpr qint64 kMaxEthRows = 100'000;
// Rows derived over all accounts together: wallets add up.
constexpr qint64 kMaxEthTotalRows = EvmScanner::kMaxFollowedRows;
// Rows derived per event-loop turn when a pass sets up its windows: BtcScanner's
// slice, so that many wallets or a large gap never freeze the window.
constexpr qint64 kDeriveSlice = 250;
// A nonce counts the transactions an address has sent. No address comes
// anywhere near 2^32 of them, so a larger one is a malformed reply, not proof
// that the address was used.
constexpr quint64 kMaxPlausibleNonce = quint64(1) << 32;
// Public endpoints rate-limit; never poll faster.
int& minRefreshSeconds()
{
    static int seconds = 10;
    return seconds;
}
constexpr int kMaxRefreshSeconds = 24 * 3600;
constexpr qsizetype kMaxReported = 64;      // distinct messages remembered at once
constexpr qsizetype kMaxServerDetail = 200; // characters of server text a problem() carries
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
// way net::multicall checks its own; a mismatch leaves the calldata empty, and
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
// No number counts unless the reply shows it ran on this network: its chain id, and
// every token answering with one uint256 word as a listed token always does there.
// A call to an address without code "succeeds" with empty data, which is how
// a contract address answers on another chain, and an eth_call to a
// missing Multicall3 returns no data at all. `why` then names the evidence.
BalanceReply parseBalances(const QJsonValue& result, qsizetype addresses, quint64 expectedChainId,
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
        if (!readable || chainId != expectedChainId) {
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

// Only the scan fields change here; skipping identical rows keeps a refresh
// pass from repainting the whole table every minute.
bool scanFieldsDiffer(const AddressRow& a, const AddressRow& b)
{
    return a.checked != b.checked || a.used != b.used || a.nonce != b.nonce || a.balances != b.balances;
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

// Settings accept any https URL; this is how an endpoint of another chain, an L2 or a
// testnet gets noticed. Both requests of an attempt word it alike, so an
// endpoint that fails both proofs is reported once.
QString otherChainText(const QString& network, const QString& why)
{
    return QStringLiteral("%1: the RPC endpoint does not look like %1 (%2), "
                          "so its answers are ignored; check the %1 endpoints in Settings")
        .arg(network, why);
}

} // namespace

EvmScanner::EvmScanner(Network network, net::BudgetPool* budgets, WalletModel* model, QObject* parent)
    : ChainScanner(network, model, parent)
    , rpc_(new net::EthRpc(info().name, info().chainId, info().servers, info().serverPace, budgets, this))
    , nonceLimit_(kMaxNonces)
    , nonceCeiling_(kMaxNonces)
{
    connect(rpc_, &net::EthRpc::endpointChanged, this, &ChainScanner::serverChanged);
    // Nobody else can reach the client, so its diagnostics end up here.
    connect(rpc_, &net::EthRpc::log, this, [](const QString& line) { qCDebug(lcEvm).noquote() << line; });

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

    hotTimer_ = new QTimer(this);
    connect(hotTimer_, &QTimer::timeout, this, &EvmScanner::queueHotRound);

    // The window assumes its rows exist; after a clear, start over. A stop() or
    // start() the controller issues right after supersedes this restart, since
    // the scan itself only begins on the next event-loop turn. The balances
    // that balancesAt() dated are gone with the rows, stopped or not, unless other
    // wallets' answered rows stayed (one wallet removed): those figures are as old
    // as ever.
    if (model_) {
        connect(model_, &WalletModel::cleared, this, [this] {
            bool rowsLeft = false;
            for (int r = 0; r < model_->size() && !rowsLeft; ++r)
                rowsLeft = model_->at(r).network == network_ && model_->at(r).checked;
            if (!rowsLeft) {
                balancesAt_ = QDateTime();
                noncesAt_ = QDateTime();
                refreshFailing_ = false;
                sweepOpen_ = false;
                sweepClears_ = false;
                restoredSweep_ = NonceSweep();
            }
            if (running_)
                start();
        });
    }
}

EvmScanner::~EvmScanner()
{
    // Before the client goes: a scanner that stopped acts on none of its replies.
    stop();
    delete rpc_;
    rpc_ = nullptr;
}

void EvmScanner::setAccounts(const QList<AccountKey>& accounts)
{
    // Applies at the next start(); a running scan works on its own copy.
    accounts_.clear();
    owned_.clear();
    for (const AccountKey& key : accounts) {
        // A second key for the same account number of a wallet would fight over its rows.
        if (!key.serves(network_) || ownsAccount(key.wallet, key.account))
            continue;
        accounts_.append(key);
        owned_.insert({key.wallet, key.account});
    }
}

void EvmScanner::setSettings(const ScanSettings& settings)
{
    gap_ = qBound(1, settings.evmGap, kMaxGap);
    refreshSeconds_ = qBound(minRefreshSeconds(), settings.evmRefreshSeconds, kMaxRefreshSeconds);
    const QStringList lines = settings.servers.value(info().id);
    // Other endpoints: what the last ones said about every nonce may be why.
    if (configured_ && lines != serverLines_) {
        noncesWanted_ = true;
        nonceLimit_ = kMaxNonces;
        nonceCeiling_ = kMaxNonces;
    }
    configured_ = true;
    serverLines_ = lines;
    if (refreshTimer_->isActive())
        refreshTimer_->start(refreshSeconds_ * 1000);
}

void EvmScanner::start()
{
    stopWork();
    if (!rpc_ || !model_) {
        running_ = false;
        return;
    }
    running_ = true;
    finished_ = false;
    answered_ = false;
    firstPass_ = true;
    nonceMissed_.clear();
    sweepRetryAt_ = QDateTime();
    passActive_ = true; // counts as a pass already, so refreshNow() waits for it
    reported_.clear();
    lastChecked_ = -1;
    lastDerived_ = -1;
    if (!accounts_.isEmpty())
        rpc_->setEndpoints(serverLines_); // cancels what is still on its way from an earlier run
    chunkLimit_ = kMaxChunk;
    // Deferred: the caller finishes its own setup before any signal arrives,
    // and a stop()/start() issued right after cancels this one cleanly.
    defer(&EvmScanner::beginScan);
}

void EvmScanner::stop()
{
    stopWork();
    if (rpc_)
        rpc_->cancelAll();
    running_ = false;
    finished_ = false;
    answered_ = false;
}

void EvmScanner::restoreTimes(const QDateTime& balancesAt, const QDateTime& noncesAt, const NonceSweep& sweep)
{
    // Of rows that are there: the database keeps a time after its rows have gone.
    if (!model_ || model_->rowsOn(network_) == 0)
        return;
    if (!balancesAt_.isValid())
        balancesAt_ = balancesAt;
    if (!noncesAt_.isValid())
        noncesAt_ = noncesAt;
    if (!sweepOpen_ && !restoredSweep_.from.isValid())
        restoredSweep_ = sweep;
}

NonceSweep EvmScanner::nonceSweep() const
{
    if (!sweepOpen_ || acc_.isEmpty())
        return restoredSweep_;
    // The first it has not read: a row missed before where it has got to comes first.
    int account = sweepAccount_;
    quint32 index = sweepIndex_;
    for (const QPair<int, quint32>& missed : nonceMissed_) {
        if (missed.first < account || (missed.first == account && missed.second < index)) {
            account = missed.first;
            index = missed.second;
        }
    }
    NonceSweep out;
    out.from = sweepFrom_;
    out.clears = sweepClears_;
    const bool atEnd = account >= acc_.size();
    const AccountKey& key = acc_.at(atEnd ? acc_.size() - 1 : account).key;
    out.wallet = key.wallet;
    out.account = key.account;
    out.index = atEnd ? std::numeric_limits<quint32>::max() : index;
    return out;
}

void EvmScanner::setMinRefreshForTest(int seconds)
{
    minRefreshSeconds() = seconds;
}

void EvmScanner::allowLocalHttpForTest()
{
    rpc_->allowLocalHttpForTest();
}

void EvmScanner::refreshNow()
{
    if (!running_ || passActive_)
        return;
    sweepRetryAt_ = QDateTime(); // someone is waiting: nonces no endpoint gave are asked for again
    refreshTimer_->stop();
    startPass();
}

bool EvmScanner::isScanning() const
{
    return running_ && !finished_;
}

int EvmScanner::checkedCount() const
{
    int checked = 0, derived = 0;
    countRows(&checked, &derived);
    return checked;
}

int EvmScanner::derivedCount() const
{
    int checked = 0, derived = 0;
    countRows(&checked, &derived);
    return derived;
}

bool EvmScanner::limitReached() const
{
    if (totalCut_)
        return true;
    for (const AccountState& a : acc_)
        if (cutAtLimit(a))
            return true;
    return false;
}

bool EvmScanner::wrongChain() const
{
    // Judged by the endpoint requests go to now: once EthRpc has moved on from
    // a rejected one, the answers of the next decide.
    return rpc_ && !offMainnet_.isEmpty() && offMainnet_.contains(rpc_->currentEndpoint());
}

QString EvmScanner::serverName() const
{
    // Host only: a configured endpoint may carry an API key in its path, query
    // or user info, and this name is shown on screen and written to reports.
    return rpc_ ? QUrl(rpc_->currentEndpoint()).host() : QString();
}

ScanTiers EvmScanner::tiers() const
{
    ScanTiers t;
    t.live = int(totalRows_);
    // A pass that takes longer than the refresh leaves the rest to its own pace.
    if (lastPassSeconds_ > refreshSeconds_) {
        t.live = hotRows_;
        t.rounds = int(totalRows_) - hotRows_;
        t.roundSeconds = lastPassSeconds_;
    }
    return t;
}

QDateTime EvmScanner::balancesAt() const
{
    return balancesAt_;
}

bool EvmScanner::refreshFailing() const
{
    if (refreshFailing_)
        return true;
    // No whole pass for this long: an outage that fails nothing, a night's sleep. Until
    // the first answers are in, the time is from an earlier run and says so anyway.
    if (!running_ || !finished_ || !balancesAt_.isValid())
        return false;
    // The pass under way may be one that reads every nonce, a lot longer than the last.
    const qint64 sweeping = sweepAsking_ && passActive_ ? totalRows_ / kNoncesPerSecond : 0;
    const qint64 planned = qMax(qMax<qint64>(lastPassSeconds_, refreshSeconds_), sweeping) * 1000;
    return balancesAt_.msecsTo(QDateTime::currentDateTime()) > 2 * planned + kPassGraceMs;
}

// ------------------------------------------------------------ the window ----

void EvmScanner::beginScan()
{
    tokens_.clear();
    for (const Asset token : info().tokens)
        tokens_.append(assetInfo(token));

    acc_.clear();
    for (const AccountKey& key : std::as_const(accounts_)) {
        AccountState a;
        a.key = key;
        try {
            a.chainNode = key.node().deriveChild(0);
        } catch (const std::exception&) {
            // The parser's own text is not passed on: it could quote the key.
            report({Problem::Skipped,
                    QStringLiteral("%1 account %2: the stored public key could not be read")
                        .arg(info().name)
                        .arg(key.account),
                    QString()});
            continue;
        }
        acc_.append(a);
    }

    totalRows_ = 0;
    totalCut_ = false;
    placeRestoredSweep();
    const quint64 gen = gen_;
    adoptRows();
    if (gen != gen_)
        return; // a model slot restarted or stopped the scan
    startPass();
}

// Rows of these accounts already in the model (a restart with new settings, or the
// database at the start) are reused: they size the windows at once and need no
// derivation. One walk over the rows for all accounts: a thousand accounts must not
// walk them a thousand times.
void EvmScanner::adoptRows()
{
    struct Seen {
        qint64 maxIndex = -1;
        qint64 lowest = -1;
        QString lowestAddress;
    };
    QHash<QPair<quint32, quint32>, int> positions;
    for (int i = 0; i < acc_.size(); ++i)
        positions.insert({acc_.at(i).key.wallet, acc_.at(i).key.account}, i);
    QList<Seen> seen(acc_.size());
    const bool clearing = noncesWanted_ || (sweepOpen_ && sweepClears_);
    for (int r = 0; r < model_->size(); ++r) {
        const AddressRow& row = model_->at(r);
        if (row.network != network_ || row.change != 0 || qint64(row.index) >= kMaxEthRows) // past the cap: never derived here
            continue;
        const auto found = positions.constFind({row.wallet, row.account});
        if (found == positions.cend())
            continue;
        Seen& one = seen[*found];
        const qint64 index = row.index;
        one.maxIndex = qMax(one.maxIndex, index);
        // Unless the endpoints changed, and it may be their word that made them used. The
        // answers of this sweep decide then: the flag only says what to read first.
        if (!clearing && row.used)
            acc_[*found].lastUsed = qMax(acc_.at(*found).lastUsed, index);
        if (row.used)
            acc_[*found].savedUsed = qMax(acc_.at(*found).savedUsed, index);
        if (one.lowest < 0 || index < one.lowest) {
            one.lowest = index;
            one.lowestAddress = row.address;
        }
    }

    const quint64 gen = gen_;
    for (int i = 0; i < acc_.size(); ++i) {
        const Seen& one = seen.at(i);
        if (one.maxIndex < 0)
            continue;
        // Rows of another key (a wallet replaced without clearing the model) would
        // show someone else's balances; re-deriving one row tells them apart.
        bool replace = false;
        AddressRow probe;
        QString why;
        if (!deriveRow(acc_.at(i), quint32(one.lowest), &probe, &why)) {
            report({Problem::Skipped, why, QString()});
        } else if (probe.address.compare(one.lowestAddress, Qt::CaseInsensitive) != 0) {
            replace = true;
            acc_[i].lastUsed = -1;
            acc_[i].savedUsed = -1;
            // They dated the other key's balances.
            balancesAt_ = QDateTime();
            noncesAt_ = QDateTime();
            sweepOpen_ = false;
        }
        ensureRows(i, 0, one.maxIndex + 1, replace, false); // also fills any gap in the indices
        if (gen != gen_)
            return; // a model slot restarted or stopped the scan
    }
}

bool EvmScanner::deriveRow(const AccountState& account, quint32 index, AddressRow* out,
                           QString* why) const
{
    try {
        const chain::AddressInfo address = chain::addressFromChainNode(
            account.chainNode, chain::ScriptType::EthAccount, account.key.account, 0, index, info().format);
        AddressRow row;
        row.wallet = account.key.wallet;
        row.network = network_;
        row.type = chain::ScriptType::EthAccount;
        row.account = account.key.account;
        row.change = 0;
        row.index = index;
        row.address = QString::fromStdString(address.address);
        *out = row;
        return true;
    } catch (const std::exception& e) {
        *why = QStringLiteral("%1 account %2: address %3 could not be derived (%4)")
                   .arg(info().name)
                   .arg(account.key.account)
                   .arg(index)
                   .arg(QString::fromUtf8(e.what()));
        return false;
    }
}

// Makes rows [from, to) of account i exist in the model, deriving the missing
// ones (or, with `replace`, all of them), and optionally queues every one of
// them for the current pass.
void EvmScanner::ensureRows(int i, qint64 from, qint64 to, bool replace, bool enqueue)
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
                    QStringLiteral("%1 scan stopped at %2 addresses, the most GitWallet follows, shared by "
                                   "all wallets. Lower the gap in Settings or remove wallets you do not "
                                   "need here.")
                        .arg(info().name, english.toString(kMaxEthTotalRows)),
                    QString()});
            return;
        }
        if (replace
            || model_->find(acc_.at(i).key.wallet, network_, chain::ScriptType::EthAccount, acc_.at(i).key.account,
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

// The later of lastUsed + 1 and what was given out, + gap, but never past an index that
// failed to derive, nor past kMaxEthRows.
qint64 EvmScanner::windowEnd(const AccountState& account) const
{
    return qMin(qMin(qMax(account.lastUsed + 1, qint64(account.key.issued)) + gap_, account.limit), kMaxEthRows);
}

// The rows the window wants past kMaxEthRows are neither derived nor checked.
bool EvmScanner::cutAtLimit(const AccountState& account) const
{
    return qMin(qMax(account.lastUsed + 1, qint64(account.key.issued)) + gap_, account.limit) > kMaxEthRows;
}

// Derives (and, during a pass, queues) the part below `upTo` of the rows the window
// gained since it was last computed.
void EvmScanner::extendWindowTo(int i, bool enqueue, qint64 upTo)
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
                QStringLiteral("Stopped at %1 addresses on %2, the limit per account: the %3 "
                               "endpoint reports addresses that far out as used.")
                    .arg(english.toString(kMaxEthRows), where, info().name),
                QString()});
    }
    if (upTo > a.end)
        ensureRows(i, a.end, upTo, false, enqueue);
}

// -------------------------------------------------------------- a pass ----

// The initial scan and every refresh are the same pass: all known rows of the
// window, checked chunk by chunk, one chunk in flight at a time.
void EvmScanner::startPass()
{
    passActive_ = true;
    passFailed_ = false;
    passSkipped_ = false;
    queue_.clear();
    queueHead_ = 0;
    hotQueue_.clear();
    deriving_ = 0;
    growing_.clear(); // deriveWindows() covers every window
    noncesDecided_ = false;
    passClock_.start();
    passStartedAt_ = QDateTime::currentDateTime();
    hotTimer_->start(refreshSeconds_ * 1000);
    // The timer ran out with the last pass: what it would have read goes first.
    if (lastPassSeconds_ > refreshSeconds_)
        queueHotRound();
    deriveWindows();
}

bool EvmScanner::hotRow(const AccountState& account, const AddressRow& row) const
{
    const qint64 index = row.index;
    // What was given out and has not been paid yet, then the unused ones after that.
    const qint64 last = qMax(account.lastUsed, account.savedUsed);
    return !row.balances.isEmpty() || (index > last && index <= qMax(last, qint64(account.key.issued) - 1) + kHotAhead);
}

void EvmScanner::noteIssued(const AccountKey& key)
{
    if (!running_)
        return; // the next start() reads it from the key
    for (int i = 0; i < acc_.size(); ++i) {
        if (acc_.at(i).key.wallet != key.wallet || acc_.at(i).key.account != key.account)
            continue;
        acc_[i].key.issued = qMax(acc_.at(i).key.issued, key.issued);
        if (acc_.at(i).end >= windowEnd(acc_.at(i)))
            return;
        // In the pass under way, or one now: the next address must not wait for a refresh.
        if (!passActive_)
            refreshNow();
        else if (!growing_.contains(i))
            growing_.append(i);
        return;
    }
}

// A pass longer than the refresh interval must not leave a fresh address waiting for
// it: these go first, whatever the pass has got to.
void EvmScanner::queueHotRound()
{
    if (!running_ || !passActive_ || !hotQueue_.isEmpty())
        return;
    QHash<QPair<quint32, quint32>, int> positions;
    for (int i = 0; i < acc_.size(); ++i)
        positions.insert({acc_.at(i).key.wallet, acc_.at(i).key.account}, i);
    for (int r = 0; r < model_->size(); ++r) {
        const AddressRow& row = model_->at(r);
        if (row.network != network_)
            continue;
        const auto found = positions.constFind({row.wallet, row.account});
        if (found != positions.cend() && hotRow(acc_.at(*found), row))
            hotQueue_.append(Slot{*found, row.index, QString()});
    }
    hotRows_ = int(hotQueue_.size());
}

// Brings every window up to date, kDeriveSlice rows per event-loop turn, then
// queues the whole pass. A slice that does not finish them comes back next turn.
void EvmScanner::deriveWindows()
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
        defer(&EvmScanner::deriveWindows);
        return;
    }
    for (int i = 0; i < acc_.size(); ++i)
        for (qint64 index = 0; index < acc_.at(i).end; ++index)
            queue_.append(Slot{i, quint32(index), QString()});
    noteProgress();
    defer(&EvmScanner::nextChunk);
}

// The windows a used address moved during the pass, queued as they grow: the same
// slices as deriveWindows(), so a far-out address never freezes the window either.
void EvmScanner::growWindows()
{
    const quint64 gen = gen_;
    qint64 budget = kDeriveSlice;
    while (!growing_.isEmpty() && budget > 0 && !totalCut_) {
        const int i = growing_.constFirst();
        const qint64 before = acc_.at(i).end;
        extendWindowTo(i, true, qMin(windowEnd(acc_.at(i)), before + budget));
        if (gen != gen_)
            return; // a model slot restarted or stopped the scan
        budget -= acc_.at(i).end - before;
        // Whole, or ended early at an index that cannot be derived: on to the next.
        if (acc_.at(i).end >= windowEnd(acc_.at(i)))
            growing_.removeFirst();
    }
    if (totalCut_)
        growing_.clear();
    noteProgress();
    defer(growing_.isEmpty() ? &EvmScanner::nextChunk : &EvmScanner::growWindows);
}

void EvmScanner::nextChunk()
{
    if (!growing_.isEmpty()) {
        growWindows();
        return;
    }
    chunk_.clear();
    attempts_ = 0;
    if (!noncesDecided_)
        decideNonces();
    else
        moveSweepOn();
    // What one eth_call can read, with this network's tokens.
    const int perCall = int(kMaxSubCalls / (1 + tokens_.size()));
    const int limit = qBound(kMinChunk, qMin(chunkLimit_, perCall), kMaxChunk);
    const int nonceLimit = qMin(limit, nonceLimit_);
    int nonces = 0;
    const auto take = [&](const Slot& slot, bool fromPass) {
        const AccountKey& key = acc_.at(slot.account).key;
        const int pos = model_->find(key.wallet, network_, chain::ScriptType::EthAccount, key.account, 0, slot.index);
        if (pos < 0)
            return;
        Slot s = slot;
        s.address = model_->at(pos).address;
        // A slot back from a failed attempt still wants what it wanted.
        s.readNonce = slot.readNonce || !model_->at(pos).checked || (fromPass && sweepWants(slot));
        nonces += s.readNonce ? 1 : 0;
        chunk_.append(s);
    };
    while (!hotQueue_.isEmpty() && chunk_.size() < limit && nonces < nonceLimit)
        take(hotQueue_.takeLast(), false);
    while (queueHead_ < queue_.size() && chunk_.size() < limit && nonces < nonceLimit)
        take(queue_.at(queueHead_++), true);
    if (chunk_.isEmpty()) {
        finishPass();
        return;
    }
    sendNonces();
}

bool EvmScanner::noncesDue() const
{
    if (!noncesAt_.isValid())
        return true;
    const qint64 age = (sweepEndedAt_.isValid() ? sweepEndedAt_ : noncesAt_).msecsTo(QDateTime::currentDateTime());
    return age < 0 || age > qMax(kNoncePassMs, totalRows_ * 1000 / kNoncesPerSecond);
}

void EvmScanner::decideNonces()
{
    noncesDecided_ = true;
    const bool fresh = noncesWanted_ || (!sweepOpen_ && ((firstPass_ && totalRows_ <= kReadNoncesAtStart) || noncesDue()));
    if (fresh) {
        sweepClears_ = noncesWanted_ || (sweepOpen_ && sweepClears_);
        noncesWanted_ = false;
        sweepOpen_ = true;
        sweepFrom_ = passStartedAt_;
        sweepAccount_ = 0;
        sweepIndex_ = 0;
        sweepRetryAt_ = QDateTime();
        nonceMissed_.clear();
    }
    firstPass_ = false;
    nonceFailures_ = 0;
    sweepAsking_ = sweepOpen_ && (!sweepRetryAt_.isValid() || QDateTime::currentDateTime() >= sweepRetryAt_);
}

void EvmScanner::placeRestoredSweep()
{
    const NonceSweep sweep = std::exchange(restoredSweep_, NonceSweep());
    if (!sweep.from.isValid() || acc_.isEmpty())
        return;
    sweepOpen_ = true;
    sweepFrom_ = sweep.from;
    sweepClears_ = sweep.clears;
    sweepAccount_ = 0;
    sweepIndex_ = 0;
    // Its account may have gone with a wallet: from the top then, under the same time.
    for (int i = 0; i < acc_.size(); ++i) {
        if (acc_.at(i).key.wallet == sweep.wallet && acc_.at(i).key.account == sweep.account) {
            sweepAccount_ = i;
            sweepIndex_ = sweep.index;
            break;
        }
    }
}

bool EvmScanner::sweepWants(const Slot& slot) const
{
    if (!sweepAsking_ || nonceFailures_ >= kMaxNonceFailures)
        return false;
    return slot.account > sweepAccount_ || (slot.account == sweepAccount_ && slot.index >= sweepIndex_)
           || nonceMissed_.contains({slot.account, slot.index});
}

void EvmScanner::moveSweepOn()
{
    // Only forward: a window that grows queues its rows last, and they want a nonce anyway.
    if (!sweepOpen_ || !sweepAsking_ || nonceFailures_ >= kMaxNonceFailures)
        return;
    if (queueHead_ >= queue_.size()) {
        sweepAccount_ = int(acc_.size());
        sweepIndex_ = 0;
        return;
    }
    const Slot& next = queue_.at(queueHead_);
    if (next.account > sweepAccount_ || (next.account == sweepAccount_ && next.index > sweepIndex_)) {
        sweepAccount_ = next.account;
        sweepIndex_ = next.index;
    }
}

// (a) One JSON-RPC batch: eth_chainId, then eth_getTransactionCount of every
// address in the chunk. The batch is a single HTTP request (see kChunk), so its
// chain id proves where its nonces come from, whichever endpoint answered it;
// the eth_call of (b) cannot, since EthRpc may fail over between the two. No
// nonce counts without this network's chain id: one read on another chain would mark
// the address used here (sticky) and push the window. EthRpc itself withholds the
// answers of an endpoint whose eth_chainId is another and replies with an error
// instead (EthRpc::isOffChain() tells it apart), so the chain id check here is
// the second line. Endpoint failure texts quote the endpoint: they go out as
// the server detail only.
void EvmScanner::sendNonces()
{
    chunkIssues_.clear();
    const qsizetype n = chunk_.size();
    QList<QPair<QString, QJsonArray>> calls;
    calls.append(qMakePair(QStringLiteral("eth_chainId"), QJsonArray()));
    QList<qsizetype> asked; // positions in chunk_ of the nonces asked for
    for (qsizetype i = 0; i < n; ++i) {
        if (!chunk_.at(i).readNonce)
            continue;
        asked.append(i);
        calls.append(qMakePair(QStringLiteral("eth_getTransactionCount"),
                               QJsonArray{chunk_.at(i).address, QStringLiteral("latest")}));
    }
    // None wanted: on to the balances, whose reply proves its own chain.
    if (asked.isEmpty()) {
        onNonces(QList<quint64>(n, quint64(0)), QList<bool>(n, true), {});
        return;
    }

    const quint64 gen = gen_;
    const quint64 seq = ++nextSeq_;
    pendingSeq_ = seq;
    QPointer<EvmScanner> self(this);
    rpc_->batch(calls, [self, gen, seq, n, asked](const QList<QJsonValue>& results, const QStringList& errors,
                                                  const QStringList& endpoints) {
        if (!self || !self->acceptReply(gen, seq))
            return;
        QList<quint64> nonces(n, quint64(0));
        QList<bool> ok(n, false);
        for (qsizetype i = 0; i < n; ++i)
            ok[i] = !asked.contains(i); // what was not asked needs no answer; see the loop below
        QList<Issue> issues;
        const QString idError = errors.value(0, QStringLiteral("no reply"));
        quint64 chainId = 0;
        bool unusable = false; // it answered, and what it said about a nonce makes no sense
        if (!idError.isEmpty() && self->rpc_->isOffChain(endpoints.value(0))) {
            // EthRpc already skips it; recorded here so wrongChain() can say so.
            if (!self->offMainnet_.contains(endpoints.value(0)))
                self->offMainnet_.append(endpoints.value(0));
            issues << Issue{Problem::WrongChain,
                            otherChainText(self->info().name,
                                           QStringLiteral("eth_chainId is not %1").arg(self->info().chainId)),
                            serverDetail(idError)};
        } else if (!idError.isEmpty()) {
            issues << Issue{Problem::Failure, QStringLiteral("%1 nonce lookup failed").arg(self->info().name),
                            serverDetail(idError)};
        } else if (!parseQuantity(results.value(0), &chainId)) {
            issues << Issue{Problem::Failure,
                            QStringLiteral("%1 nonce lookup failed: no valid chain id in the reply").arg(self->info().name),
                            QString()};
        } else if (chainId != self->info().chainId) {
            issues << Issue{Problem::WrongChain,
                            otherChainText(self->info().name, QStringLiteral("chain id %1").arg(chainId)), QString()};
            self->refuseEndpoint(endpoints.value(0), true);
            if (!self || gen != self->gen_)
                return; // a slot of EthRpc's signals restarted or stopped the scan
        } else {
            for (qsizetype a = 0; a < asked.size(); ++a) {
                const qsizetype i = asked.at(a);
                const qsizetype call = 1 + a; // after eth_chainId
                const QString err = errors.value(call, QStringLiteral("no reply"));
                quint64 nonce = 0;
                if (err.isEmpty() && parseQuantity(results.value(call), &nonce)
                    && nonce <= kMaxPlausibleNonce) {
                    nonces[i] = nonce;
                    ok[i] = true;
                } else if (!err.isEmpty()) {
                    issues << Issue{Problem::Failure, QStringLiteral("%1 nonce lookup failed").arg(self->info().name),
                                    serverDetail(err)};
                } else {
                    unusable = true;
                    issues << Issue{Problem::Failure,
                                    QStringLiteral("%1 nonce lookup failed: unexpected reply").arg(self->info().name),
                                    QString()};
                }
            }
        }
        if (unusable) {
            self->refuseEndpoint(endpoints.value(0), false);
            if (!self || gen != self->gen_)
                return; // a slot of EthRpc's signals restarted or stopped the scan
        }
        self->onNonces(nonces, ok, issues);
    });
}

void EvmScanner::onNonces(const QList<quint64>& nonces, const QList<bool>& ok,
                          const QList<Issue>& issues)
{
    nonce_ = nonces;
    nonceOk_ = ok;
    for (const Issue& issue : issues)
        noteFailure(issue);
    // (b) runs even when (a) failed: whatever balances arrive are still shown.
    defer(&EvmScanner::sendBalances);
}

// (b) One eth_call to Multicall3.aggregate3: getChainId first, then per address
// getEthBalance plus balanceOf on every ERC-20 contract, in that order. An
// endpoint that answers it with something unusable, or with what shows another
// chain (or no state of this one), may well have said the right id to eth_chainId:
// only this reply gives it away, so it is rejected here, before the retry.
void EvmScanner::sendBalances()
{
    const QString multicall = info().multicall;
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
                          QStringLiteral("%1 balance lookup failed: the Multicall3 request "
                                         "could not be encoded")
                              .arg(info().name),
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
    QPointer<EvmScanner> self(this);
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
                   const QString name = self->info().name;
                   if (!rpcError.isEmpty()) {
                       issues << Issue{Problem::Failure, QStringLiteral("%1 balance lookup failed").arg(name),
                                       serverDetail(rpcError)};
                   } else {
                       switch (parseBalances(result, n, self->info().chainId, tokens, &known, &amounts, &why)) {
                       case BalanceReply::Ok:
                           self->offMainnet_.removeAll(endpoint); // it serves this network (again)
                           // Not one of them, or still some on a retry that asked for fewer: the
                           // size was not the reason, and there is no use asking it again.
                           unusable = !known.contains(true) || (self->attempts_ >= 1 && known.contains(false));
                           if (known.contains(false))
                               issues << Issue{Problem::Failure,
                                               QStringLiteral("%1 balance lookup: some balance calls failed").arg(name),
                                               QString()};
                           break;
                       case BalanceReply::Malformed:
                           unusable = true;
                           issues << Issue{Problem::Failure,
                                           QStringLiteral("%1 balance lookup failed: unexpected Multicall3 reply").arg(name),
                                           QString()};
                           break;
                       case BalanceReply::OtherChain:
                           otherChain = true;
                           issues << Issue{Problem::WrongChain, otherChainText(name, why), QString()};
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
void EvmScanner::applyChunk(const QList<bool>& known, const QList<chain::Amount>& amounts,
                            const QList<Issue>& issues, bool otherChain)
{
    for (const Issue& issue : issues)
        noteFailure(issue);

    const quint64 gen = gen_;
    const qsizetype stride = 1 + tokens_.size();
    const QList<Slot> attempt = chunk_;
    QList<Slot> failed;
    QList<int> grown; // accounts whose last used index moved up
    bool nonceLate = false; // a row's balances answered, and its nonce did not
    bool nonceAnswered = false;
    bool nonceMissing = false;
    int noncesAsked = 0;
    QList<Slot> late;

    for (qsizetype i = 0; i < attempt.size(); ++i) {
        const Slot& s = attempt.at(i);
        const AccountKey& key = acc_.at(s.account).key;
        const int pos = model_->find(key.wallet, network_, chain::ScriptType::EthAccount, key.account, 0, s.index);
        if (pos < 0 || model_->at(pos).address.compare(s.address, Qt::CaseInsensitive) != 0)
            continue; // replaced or removed while the request was out
        const AddressRow old = model_->at(pos);
        AddressRow row = old;

        const bool nonceRead = !otherChain && s.readNonce && nonceOk_.value(i, false);
        noncesAsked += s.readNonce ? 1 : 0;
        nonceMissing = nonceMissing || (s.readNonce && !nonceRead);
        const quint64 oldNonce = old.nonce;
        if (nonceRead) {
            row.nonce = nonce_.at(i);
            nonceAnswered = true;
            nonceMissed_.remove({s.account, s.index});
        }
        bool balancesOk = !otherChain;
        const qsizetype base = i * stride;
        for (qsizetype k = 0; k < stride; ++k) {
            if (!known.value(base + k, false)) {
                balancesOk = false; // unknown: keep the old value
                continue;
            }
            row.setBalance(k == 0 ? info().coin : tokens_.at(k - 1).asset, amounts.at(base + k));
        }
        const bool noNonce = s.readNonce && !nonceRead;
        // A failed attempt never downgrades a row: what an earlier pass
        // confirmed stays shown until a newer answer replaces it.
        row.checked = old.checked || (balancesOk && !noNonce);
        // It stays used once it held something: a spend by signature leaves its nonce at 0,
        // and the next address may be paid. A nonce that falls shows the earlier answer was
        // false, and after a change of endpoints nothing an earlier one said counts.
        const bool shown = row.nonce > 0 || !row.balances.isEmpty();
        const bool refuted = nonceRead && (sweepClears_ || oldNonce > 0);
        row.used = shown || (old.used && !refuted);

        if (scanFieldsDiffer(row, old)) {
            model_->upsert(row);
            if (gen != gen_)
                return; // a model slot restarted or stopped the scan
        }
        AccountState& a = acc_[s.account];
        // An index at or past kMaxEthRows never moves the window (none is queued). In a sweep
        // that clears, what an earlier endpoint said moves it no more than it did at the start.
        if ((sweepClears_ ? shown : row.used) && qint64(s.index) > a.lastUsed && qint64(s.index) < kMaxEthRows) {
            a.lastUsed = s.index;
            if (!grown.contains(s.account))
                grown.append(s.account);
        }
        // The nonce of a row that has answered before is the rare extra: it does not hold
        // fresh balances back, and is asked again next pass.
        if (!balancesOk || (noNonce && !old.checked)) {
            failed.append(s);
        } else if (noNonce) {
            nonceLate = true;
            nonceMissed_.insert({s.account, s.index});
            late.append(s);
        }
    }

    // How many at once this endpoint takes: some cap a batch at 3 or 10. Learned only from an
    // attempt whose balances answered: it was there, and refused the batch. In an outage
    // both fail, and nothing is learned. What the batch was too large for goes again at once.
    const bool learning = !otherChain && nonceMissing && known.contains(true) && noncesAsked > kMinChunk;
    if (!otherChain && noncesAsked > 0 && known.contains(true)) {
        if (nonceMissing) {
            goodBatches_ = 0;
            nonceCeiling_ = qMax(kMinChunk, noncesAsked - 1);
            nonceLimit_ = qMax(kMinChunk, qMin(noncesAsked / 2, nonceCeiling_));
        } else {
            nonceLimit_ = qMin(nonceLimit_ * 2, nonceCeiling_);
            if (++goodBatches_ >= kBatchesBeforeProbe) {
                goodBatches_ = 0;
                nonceCeiling_ = qMin(nonceCeiling_ * 2, kMaxNonces);
            }
        }
    }
    if (learning) {
        hotQueue_.append(late);
    } else if (nonceLate && noncesAsked <= kMinChunk) {
        if (++nonceFailures_ >= kMaxNonceFailures && sweepAsking_) {
            sweepAsking_ = false;
            sweepRetryAt_ = QDateTime::currentDateTime().addSecs(kNonceRetrySeconds);
        }
    } else if (nonceAnswered) {
        nonceFailures_ = 0;
    }

    for (int a : std::as_const(grown)) {
        if (!growing_.contains(a))
            growing_.append(a); // nextChunk() derives them first
    }
    noteProgress();

    if (failed.isEmpty()) {
        chunk_.clear();
        chunkLimit_ = qMin(chunkLimit_ * 2, kMaxChunk);
        if (nonceLate && !learning) {
            passFailed_ = true;
            for (const Issue& m : std::as_const(chunkIssues_))
                report(m);
        }
        defer(&EvmScanner::nextChunk);
        return;
    }

    // Too much at once for this endpoint, maybe; the next attempt asks for less.
    chunkLimit_ = qMax(chunkLimit_ / 2, kMinChunk);
    passFailed_ = true;
    QList<Issue> messages = chunkIssues_;
    if (attempts_ < kMaxRetries) {
        ++attempts_;
        // Only the rows still missing an answer, and no more than the smaller limit: the
        // others go first once this attempt is through.
        while (failed.size() > chunkLimit_) {
            const Slot later = failed.takeLast();
            hotQueue_.append(later);
            // The sweep moves on past it; should the run end first, it has not been read.
            if (later.readNonce)
                nonceMissed_.insert({later.account, later.index});
        }
        chunk_ = failed;
        retryTimer_->start();
    } else {
        // Skipped for this pass: the rows stay unchecked (or keep their last
        // confirmed values) and the next refresh pass tries them again.
        for (const Slot& slot : std::as_const(failed)) {
            if (slot.readNonce)
                nonceMissed_.insert({slot.account, slot.index});
        }
        chunk_.clear();
        defer(&EvmScanner::nextChunk);
        messages << Issue{Problem::Failure,
                          QStringLiteral("%1: some addresses could not be checked after %2 "
                                         "retries; the next refresh tries them again")
                              .arg(info().name)
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

void EvmScanner::finishPass()
{
    passActive_ = false;
    moveSweepOn();
    const int hotLeft = hotTimer_->isActive() ? hotTimer_->remainingTime() : -1;
    hotTimer_->stop();
    lastPassSeconds_ = int(passClock_.elapsed() / 1000);
    queue_.clear();
    queueHead_ = 0;
    hotQueue_.clear();
    chunk_.clear();
    // The balances stand for when the pass began, a long one too, only if every row of
    // it got a full answer and the whole window exists; otherwise balancesAt() keeps the
    // last pass that did. Past the limit over all accounts no row will ever exist:
    // nothing failed, and limitReached() says what is missing.
    const bool windowDone = windowChecked();
    const bool complete = !passSkipped_ && (windowDone || totalCut_);
    if (complete) {
        balancesAt_ = passStartedAt_;
        answered_ = true;
    }
    refreshFailing_ = !complete;
    // The end of a sweep: every row has had its nonce read, in this pass or an earlier one,
    // in this run or an earlier one.
    const quint64 settling = gen_;
    if (complete && sweepOpen_ && sweepAsking_ && sweepAccount_ >= acc_.size() && nonceMissed_.isEmpty()) {
        noncesAt_ = sweepFrom_;
        sweepEndedAt_ = QDateTime::currentDateTime();
        sweepOpen_ = false;
        settleWindows();
        sweepClears_ = false;
        if (settling != gen_)
            return; // a model slot restarted or stopped the scan
    }
    // A pass without failures ends an outage: the next one is reported again
    // instead of staying silent for the rest of the session.
    if (!passFailed_)
        reported_.clear();
    // A pass that outlasted the refresh: the next begins with the hot rows, at the tick
    // the hot timer would have given them, not a whole interval from here.
    if (!acc_.isEmpty())
        refreshTimer_->start(lastPassSeconds_ > refreshSeconds_ && hotLeft >= 0 ? hotLeft : refreshSeconds_ * 1000);

    const quint64 gen = gen_;
    // Skipped chunks leave the window incomplete; the refresh passes keep retrying them,
    // and the scan counts as finished once nothing is missing and a pass has answered.
    if (!finished_ && windowDone && complete) {
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

void EvmScanner::settleWindows()
{
    QHash<QPair<quint32, quint32>, int> positions;
    for (int i = 0; i < acc_.size(); ++i) {
        positions.insert({acc_.at(i).key.wallet, acc_.at(i).key.account}, i);
        acc_[i].lastUsed = -1;
        acc_[i].savedUsed = -1;
    }
    for (int r = 0; r < model_->size(); ++r) {
        const AddressRow& row = model_->at(r);
        if (row.network != network_ || row.change != 0 || qint64(row.index) >= kMaxEthRows || !row.used)
            continue;
        const auto found = positions.constFind({row.wallet, row.account});
        if (found != positions.cend())
            acc_[*found].lastUsed = qMax(acc_.at(*found).lastUsed, qint64(row.index));
    }
    bool shrunk = false;
    for (const AccountState& a : std::as_const(acc_))
        shrunk = shrunk || a.end > windowEnd(a);
    if (!shrunk)
        return;
    // A row past a window was never used, or the window would reach it.
    const quint64 gen = gen_;
    model_->dropRows([this, &positions](const AddressRow& row) {
        if (row.network != network_ || row.change != 0)
            return true;
        const auto found = positions.constFind({row.wallet, row.account});
        return found == positions.cend() || qint64(row.index) < windowEnd(acc_.at(*found));
    });
    if (gen != gen_)
        return;
    totalRows_ = 0;
    totalCut_ = false;
    for (AccountState& a : acc_) {
        a.end = qMin(a.end, windowEnd(a));
        a.capReported = false;
        totalRows_ += a.end;
    }
}

bool EvmScanner::windowChecked() const
{
    for (const AccountState& a : acc_) {
        const qint64 target = windowEnd(a);
        for (qint64 index = 0; index < target; ++index) {
            const int pos = model_->find(a.key.wallet, network_, chain::ScriptType::EthAccount, a.key.account, 0,
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
bool EvmScanner::acceptReply(quint64 gen, quint64 seq)
{
    if (gen != gen_ || seq != pendingSeq_)
        return false;
    pendingSeq_ = 0;
    return true;
}

// Steps run from the event loop, never inside an EthRpc callback, and only
// while the generation that scheduled them is current.
void EvmScanner::defer(void (EvmScanner::*step)())
{
    const quint64 gen = gen_;
    QTimer::singleShot(0, this, [this, gen, step] {
        if (gen == gen_)
            (this->*step)();
    });
}

void EvmScanner::stopWork()
{
    // Whatever comes next, in terms that outlive the list of accounts.
    if (sweepOpen_ && !acc_.isEmpty()) {
        restoredSweep_ = nonceSweep();
        sweepOpen_ = false;
    }
    ++gen_;
    pendingSeq_ = 0;
    refreshTimer_->stop();
    retryTimer_->stop();
    progressTimer_->stop();
    hotTimer_->stop();
    acc_.clear();
    queue_.clear();
    queueHead_ = 0;
    hotQueue_.clear();
    chunk_.clear();
    nonce_.clear();
    nonceOk_.clear();
    chunkIssues_.clear();
    attempts_ = 0;
    passActive_ = false;
    passFailed_ = false;
    passSkipped_ = false;
    offMainnet_.clear();
    // limitReached() reads these whether a scan runs or not.
    totalRows_ = 0;
    totalCut_ = false;
    growing_.clear();
}

// `url` (as the reply named it) gave an answer nothing can be read from: EthRpc
// moves requests to another endpoint and tries it last for a while. `offMainnet`:
// the answer showed it does not serve this network (see wrongChain()).
void EvmScanner::refuseEndpoint(const QString& url, bool offMainnet)
{
    if (url.isEmpty())
        return;
    // Recorded first: endpointChanged() slots may already ask wrongChain().
    if (offMainnet && !offMainnet_.contains(url))
        offMainnet_.append(url);
    rpc_->rejectEndpoint(url);
}

void EvmScanner::noteFailure(const Issue& issue)
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
void EvmScanner::report(const Issue& issue)
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
void EvmScanner::noteProgress()
{
    if (!progressTimer_->isActive())
        progressTimer_->start();
}

void EvmScanner::emitProgress()
{
    int checked = 0, derived = 0;
    countRows(&checked, &derived);
    if (checked == lastChecked_ && derived == lastDerived_)
        return;
    lastChecked_ = checked;
    lastDerived_ = derived;
    emit progress(checked, derived);
}

void EvmScanner::countRows(int* checked, int* derived) const
{
    *checked = 0;
    *derived = 0;
    if (!model_)
        return;
    for (int r = 0; r < model_->size(); ++r) {
        const AddressRow& row = model_->at(r);
        if (row.network != network_ || row.change != 0 || !ownsAccount(row.wallet, row.account))
            continue;
        ++*derived;
        if (row.checked)
            ++*checked;
    }
}

bool EvmScanner::ownsAccount(quint32 wallet, quint32 account) const
{
    return owned_.contains({wallet, account});
}

} // namespace wallet
