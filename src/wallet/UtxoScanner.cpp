#include "wallet/UtxoScanner.h"

#include "chain/Hash.h"
#include "net/ElectrumClient.h"
#include "wallet/WalletModel.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QLoggingCategory>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace wallet {

namespace {

// Server rotation and failover. Never carries a seed or a key: only server names,
// addresses and counts reach it.
Q_STATIC_LOGGING_CATEGORY(lcUtxo, "gitwallet.net.electrum")

// Requests this scanner keeps handed to the client at once. The client sends
// kMaxInFlight (64) and queues the rest; feeding it in slices as replies come
// back keeps that queue short, so a reconnect re-sends little and a balance
// fetch never waits behind thousands of subscriptions. (onClientReady() alone
// goes past it: see there.)
constexpr int kMaxOutstanding = 100;

// Addresses derived per event-loop turn. Each costs a CKDpub, the script
// hashes and a model insert; slicing keeps the window responsive when a large
// gap or a long used run derives thousands of rows at once.
constexpr int kDeriveSlice = 250;
// Rows adopted from the model per turn: a lookup each, no key arithmetic.
constexpr int kAdoptSlice = 5000;

constexpr qint64 kProgressIntervalMs = 250;

// The controller clamps gaps itself; this only guards another caller.
constexpr int kMaxGap = 100000;

// Rows one scan watches at most, all chains together: kRowsPerGapRow times the sum
// of the chains' gaps, within [kMinWalletRows, kMaxWalletRows]. Every used status
// pushes a window `gap` rows further and the model never drops a row, so a buggy or
// hostile server that calls every address used would otherwise grow the wallet and
// its requests without end (a limit per chain still let six chains reach six times
// as much). Each chain's own gap window is derived first, inside the limit; for one
// wallet the rest leaves room for close to 10,000 used addresses or more at any gap
// the controller allows (20 to 5,000).
constexpr qint64 kRowsPerGapRow = 20;
constexpr int kMinWalletRows = 10000;
constexpr int kMaxWalletRows = UtxoScanner::kMaxFollowedRows;

// The end of a chain's indices. Only a gap window larger than the row limit (a
// caller other than the controller) gets near it; well below 2^31, where
// non-hardened BIP32 indices end.
constexpr int kMaxChainRows = 100000;

// Failed requests (a refused subscription, a failed balance) go out again
// after this delay, doubling per round: a server busy for a moment is asked
// again soon, one that keeps refusing an address is not hammered.
int& retryFirstMs()
{
    static int ms = 5000;
    return ms;
}
constexpr int kRetryMaxMs = 5 * 60 * 1000;

// Reads per second, first reads and rounds together: below the 250 at which four of
// five public servers answered 30,000 with no error and no slowdown, and at which the
// fifth asked for a pause after 22,000 (docs/ENDPOINTS.md).
constexpr int kReadsPerSecond = 200;
constexpr int kReadTickMs = 100;
constexpr int kReadsPerTick = kReadsPerSecond * kReadTickMs / 1000;
// A round takes at least this long, however few addresses it reads.
int& minRoundMs()
{
    static int ms = 5 * 60 * 1000;
    return ms;
}
// A server that refuses a read may be asking for a pause (one asked for 147 s): no more
// for this long, doubling while it keeps refusing.
int& readPauseFirstMs()
{
    static int ms = 30 * 1000;
    return ms;
}
constexpr int kReadPauseMaxMs = 10 * 60 * 1000;
// Reads refused in a row: a single address may have a history too long to give, this many
// mean the server is not reading, whatever its balances say.
constexpr int kRefusedReadsForFailing = 8;
// Reads a round can have refused and still count as whole, if each one's balance answered.
constexpr int kExcusedReads = 16;
// What makes a server worth leaving: this many requests refused with none answered at all...
constexpr int kRefusedForAMove = 32;
// ... or reads refused for this long, longer than the pauses servers have asked for.
constexpr int kReadsRefusedForAMove = 4;
constexpr qint64 kReadsRefusedMoveMs = 3 * 60 * 1000;
// How far past its plan a round may run before the figures stop passing for current.
constexpr qint64 kRoundGraceMs = 60 * 1000;
// Longer than a server stays quiet on a good connection: pings go out every minute.
constexpr qint64 kQuietSeconds = 120;
// This many refused subscriptions on one connection are a limit of that server's rather
// than a difficulty with one address: it follows what it has, and the rest are read.
constexpr int kRefusalsForALimit = 16;

int& livePlaces()
{
    static int places = UtxoScanner::kMaxLiveRows;
    return places;
}

// Of the live places, what the next unused addresses of all chains may take; the rest
// is for addresses holding coins.
int hotPlaces()
{
    return livePlaces() * 3 / 4;
}
constexpr int kMinHotAhead = 5;
constexpr int kHotAheadOnChange = 5; // a change address is only ever given out by the wallet that spends

// Whole coins to smallest units, as an upper bound: a reply above it is garbage, not a
// balance. Without a known supply, the most a balance can be.
qint64 maxUnitsOf(const AssetInfo& coin)
{
    const double units = coin.maxSupply * std::pow(10.0, coin.decimals);
    constexpr double kLimit = 9.0e18;
    return coin.maxSupply > 0.0 && units < kLimit ? qint64(units) : qint64(kLimit);
}

// Distinct problems reported per start(). Past that a misbehaving server
// would only bury the user in toasts.
constexpr int kMaxErrorsPerRun = 8;

// Characters of a server's own error text that problem() passes on.
constexpr qsizetype kMaxDetailChars = 200;

// The status of a scripthash is the SHA-256 of its history (64 hex digits), or
// null when it has none. This is the SHA-256 of nothing: a server that hashes
// an empty history instead of answering null still says "no history". Read as
// used, it would push every window further, address after address.
constexpr QLatin1String kEmptyHistoryStatus("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

qint64 nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// A status as text: "" (no history) or 64 hex digits. 0 unused, 1 used, -1
// neither (malformed). `normalized` receives "" or the lowercase hash, so a
// status compares equal however a server spells it.
int parseStatusText(const QString& text, QString* normalized)
{
    normalized->clear();
    if (text.isEmpty()) return 0;
    const bool hex = text.size() == 64 && std::all_of(text.cbegin(), text.cend(), [](QChar ch) {
                         const char16_t u = ch.unicode();
                         return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
                     });
    if (!hex) return -1;
    const QString lower = text.toLower();
    if (lower == kEmptyHistoryStatus) return 0;
    *normalized = lower;
    return 1;
}

// blockchain.scripthash.subscribe result: null = no history, otherwise the
// status hash. As parseStatusText(); any other JSON type is malformed.
int parseStatus(const QJsonValue& v, QString* normalized)
{
    if (v.isString()) return parseStatusText(v.toString(), normalized);
    normalized->clear();
    return v.isNull() ? 0 : -1;
}

// blockchain.scripthash.get_balance result: {"confirmed": sats, "unconfirmed":
// sats}. Unconfirmed is negative while a spend of confirmed coins waits in the
// mempool. The numbers come from a server, so they are range-checked.
bool parseBalance(const QJsonValue& v, qint64 kMaxSats, qint64* confirmed, qint64* unconfirmed)
{
    if (!v.isObject()) return false;
    const QJsonObject o = v.toObject();
    const QJsonValue c = o.value(QLatin1String("confirmed"));
    const QJsonValue u = o.value(QLatin1String("unconfirmed"));
    if (!c.isDouble() || !u.isDouble()) return false;
    constexpr qint64 kBad = std::numeric_limits<qint64>::min(); // toInteger(): not a whole number
    const qint64 ci = c.toInteger(kBad);
    const qint64 ui = u.toInteger(kBad);
    if (ci < 0 || ci > kMaxSats || ui == kBad || ui < -kMaxSats || ui > kMaxSats) return false;
    *confirmed = ci;
    *unconfirmed = ui;
    return true;
}

// What an Electrum server wrote (a JSON-RPC error text, a bad status), fit to
// pass on as problem()'s serverDetail: one line, without control or bidi
// characters, at most kMaxDetailChars long. Whatever it says, it stays apart
// from GitWallet's own message; only the UI decides what of it to show.
QString serverDetail(const QString& text)
{
    QString line;
    line.reserve(text.size());
    for (const QChar c : text) {
        if (c.isSpace())
            line.append(QLatin1Char(' '));
        else if (c.category() != QChar::Other_Control && c.category() != QChar::Other_Format)
            line.append(c);
    }
    line = line.simplified();
    if (line.size() > kMaxDetailChars) {
        qsizetype cut = kMaxDetailChars - 1; // room for the ellipsis
        if (line.at(cut - 1).isHighSurrogate()) --cut; // never half a character
        line.truncate(cut);
        line.append(QChar(u'…'));
    }
    return line;
}

// Some servers quote the scripthash in their error text ("<hash> is not a valid
// script hash"); with it masked, one failure on 500 addresses is one report.
QString errorKey(const QString& text)
{
    static const QRegularExpression hex(QStringLiteral("[0-9A-Fa-f]{32,}"));
    QString key = text.trimmed();
    key.replace(hex, QStringLiteral("#"));
    return key;
}

} // namespace

// blockchain.scripthash.get_history result: [{"tx_hash": hex, "height": n, ...}, ...].
// The status it stands for, as the protocol defines it: the SHA-256 of
// "tx_hash:height:" for each entry, in the order given. As parseStatusText().
int UtxoScanner::statusOfHistory(const QJsonValue& v, QString* normalized)
{
    normalized->clear();
    if (!v.isArray()) return -1;
    const QJsonArray entries = v.toArray();
    if (entries.isEmpty()) return 0;
    QByteArray text; // grows as entries prove to be entries: the count is the server's word
    for (const QJsonValue& entry : entries) {
        const QJsonObject o = entry.toObject();
        const QString hash = o.value(QLatin1String("tx_hash")).toString();
        const QJsonValue height = o.value(QLatin1String("height"));
        constexpr qint64 kBad = std::numeric_limits<qint64>::min();
        const qint64 h = height.isDouble() ? height.toInteger(kBad) : kBad;
        if (hash.size() != 64 || h == kBad) return -1;
        text += hash.toLatin1();
        text += ':';
        text += QByteArray::number(h);
        text += ':';
    }
    const chain::Bytes digest = chain::sha256(reinterpret_cast<const uint8_t*>(text.constData()), size_t(text.size()));
    return parseStatusText(QString::fromStdString(chain::toHex(digest)), normalized);
}

void UtxoScanner::setLivePlacesForTest(int places)
{
    livePlaces() = places;
}

void UtxoScanner::setPacesForTest(int minRound, int retryFirst, int readPause)
{
    minRoundMs() = minRound;
    retryFirstMs() = retryFirst;
    readPauseFirstMs() = readPause;
}

void UtxoScanner::usePlainTcpForTest()
{
    client_->usePlainTcpForTest();
}

int UtxoScanner::Chain::window() const
{
    // lastUsed and floor never pass kMaxChainRows and gap is at most kMaxGap,
    // so this never comes near overflowing an int.
    return std::max(floor, lastUsed + 1 + gap);
}

int UtxoScanner::Chain::target() const
{
    return std::min(window(), kMaxChainRows);
}

int UtxoScanner::Chain::base() const
{
    // The window as it stands with nothing used: what the gap alone asks for,
    // plus the rows of another wallet to replace.
    return std::min(std::max(floor, gap), kMaxChainRows);
}

UtxoScanner::UtxoScanner(Network network, WalletModel* model, QObject* parent)
    : ChainScanner(network, model, parent), client_(new net::ElectrumClient(this))
{
    connect(client_, &net::ElectrumClient::ready, this, &UtxoScanner::onClientReady);
    connect(client_, &net::ElectrumClient::ready, this, &ChainScanner::serverChanged);
    connect(client_, &net::ElectrumClient::connectionLost, this, [this] {
        // Followed live, the figures stood for the last moment it was heard from.
        if (running_ && !busy() && roundsCount_ == 0 && client_->lastHeardAt().isValid())
            balancesAt_ = client_->lastHeardAt();
    });
    connect(client_, &net::ElectrumClient::connectionLost, this, &ChainScanner::serverChanged);
    connect(client_, &net::ElectrumClient::scripthashChanged, this, &UtxoScanner::onScripthashChanged);
    connect(client_, &net::ElectrumClient::wrongChain, this, [this](const QString& server) {
        wrongChain_ = true;
        report(Problem::WrongChain,
               QStringLiteral("%1: the server %2 answers for another chain, so it is not used; check the %1 servers "
                              "in Settings")
                   .arg(info().name, server));
        emit serverChanged();
    });
    // Nobody else can reach the client, so its diagnostics end up here.
    connect(client_, &net::ElectrumClient::log, this,
            [](const QString& line) { qCDebug(lcUtxo).noquote() << line; });
    if (model_) {
        connect(model_, &WalletModel::cleared, this, &UtxoScanner::onModelCleared);
        connect(model_, &QObject::destroyed, this, [this] { model_ = nullptr; });
    }
    readTimer_ = new QTimer(this);
    readTimer_->setInterval(kReadTickMs);
    connect(readTimer_, &QTimer::timeout, this, [this] {
        readTokens_ = std::min(readTokens_ + kReadsPerTick, 2 * kReadsPerTick);
        schedulePump();
    });
}

UtxoScanner::~UtxoScanner()
{
    // Before the client goes: it fails its pending requests while being destroyed, and
    // a scanner that stopped acts on none of those replies.
    stop();
    delete client_;
    client_ = nullptr;
}

void UtxoScanner::setSettings(const ScanSettings& settings)
{
    receiveGap_ = std::clamp(settings.utxoReceiveGap, 1, kMaxGap);
    changeGap_ = std::clamp(settings.utxoChangeGap, 1, kMaxGap);
    serverLines_ = settings.servers.value(info().id);
    // A running scan follows at once. Rows already derived stay, so a smaller gap only
    // stops further growth. The row limit moves with the gaps; deriveSlice() reports a
    // window it cuts short.
    for (Chain& c : chains_) c.gap = c.change == 0 ? receiveGap_ : changeGap_;
    if (!running_) return;
    scheduleDerive();
}

void UtxoScanner::setAccounts(const QList<AccountKey>& accounts)
{
    // Takes effect at the next start(). One chain per (wallet, type, account): a
    // repeated account would derive and subscribe every row twice.
    accounts_.clear();
    for (const AccountKey& a : accounts) {
        if (!chain::isUtxo(a.type) || a.network != network_) continue;
        const bool repeated = std::any_of(accounts_.cbegin(), accounts_.cend(), [&a](const AccountKey& b) {
            return b.wallet == a.wallet && b.type == a.type && b.account == a.account;
        });
        if (!repeated) accounts_.append(a);
    }
}

void UtxoScanner::start()
{
    ++generation_; // replies to anything sent before are dropped from now on
    running_ = true;
    chains_.clear();
    chainByKey_.clear();
    tracked_.clear();
    subQueue_.clear();
    readQueue_.clear();
    rounds_.clear();
    roundsHead_ = 0;
    roundsCount_ = 0;
    roundReadsOut_ = 0;
    readsOut_ = 0;
    refusedInRound_ = 0;
    excusedRows_ = 0;
    answersHere_ = 0;
    refusedHere_ = 0;
    refusedReads_ = 0;
    refusedInARow_ = 0;
    pauseServer_.clear();
    lastServer_.clear();
    roundInterrupted_ = false;
    answeredSaid_ = false;
    roundFailed_.clear();
    lastRoundFailed_ = false;
    forceRound_ = false;
    readsPausedUntilMs_ = 0;
    readPauseMs_ = 0;
    liveCount_ = 0;
    coinPlaces_ = 0;
    placesHere_ = livePlaces();
    refusalsHere_ = 0;
    wrongChain_ = false;
    readTokens_ = kReadsPerTick;
    roundStartMs_ = 0;
    roundEndMs_ = 0;
    roundStartedAt_ = QDateTime();
    runStartedAt_ = QDateTime::currentDateTime();
    balanceQueue_.clear();
    balanceQueued_.clear();
    balanceSeq_.clear();
    retry_.clear();
    balanceRetry_.clear();
    reportedErrors_.clear();
    subSent_ = 0;
    balanceSent_ = 0;
    checked_ = 0;
    retryDelayMs_ = 0;
    retryArmed_ = false; // a round still posted by the previous run sees the old generation
    idleReported_ = false;
    finished_ = false;
    limitReported_ = false;
    if (!client_ || !model_) {
        running_ = false;
        return;
    }

    // Reported once the state below is complete: a listener may react by
    // restarting the scan. The reasons are the wallet's own (its stored keys),
    // never a server's.
    QStringList skipped;
    for (const AccountKey& acct : accounts_) {
        const QString where = QString::fromStdString(chain::accountRootPath(acct.type, acct.account, info().format));
        chain::HdPubKey accountNode;
        try {
            accountNode = acct.node();
        } catch (const std::exception& e) {
            skipped.append(QStringLiteral("%1 account %2 skipped: %3").arg(info().name, where, QString::fromUtf8(e.what())));
            continue;
        }
        for (quint32 change = 0; change <= 1; ++change) {
            Chain c;
            c.wallet = acct.wallet;
            c.type = acct.type;
            c.account = acct.account;
            c.change = change;
            c.gap = change == 0 ? receiveGap_ : changeGap_;
            try {
                c.node = accountNode.deriveChild(change);
            } catch (const std::exception& e) {
                skipped.append(QStringLiteral("%1 chain %2/%3 skipped: %4")
                                   .arg(info().name, where, QString::number(change), QString::fromUtf8(e.what())));
                continue;
            }
            chainByKey_.insert(ChainKey{c.wallet, c.account, quint8(c.type), quint8(c.change)}, int(chains_.size()));
            chains_.append(c);
        }
    }
    // While not every address fits: places for the next unused ones, evenly over the
    // receive chains, after what the change chains take.
    const qsizetype receiveChains = std::count_if(chains_.cbegin(), chains_.cend(), [](const Chain& c) { return c.change == 0; });
    const qint64 forReceive = hotPlaces() - qint64(chains_.size() - receiveChains) * kHotAheadOnChange;
    hotReceive_ = std::clamp(receiveChains > 0 ? int(forReceive / receiveChains) : kMinHotAhead, kMinHotAhead,
                             std::max(receiveGap_, kMinHotAhead));
    tiered_ = false;
    noteTiered();
    adoptModelRows();

    // What Settings lists; none, or none that can be read: the built-in list.
    QList<net::ElectrumServer> servers;
    int lines = 0;
    for (const QString& line : std::as_const(serverLines_)) {
        const QString text = line.trimmed();
        if (text.isEmpty())
            continue;
        ++lines;
        net::ElectrumServer server;
        if (net::ElectrumServer::fromString(text, &server))
            servers.append(server);
        else
            qCInfo(lcUtxo).noquote() << "ignoring Electrum server" << text << "(expected host:port)";
    }
    const bool noValidServer = lines > 0 && servers.isEmpty();
    if (servers.isEmpty()) {
        for (const QString& line : info().servers) {
            net::ElectrumServer server;
            if (net::ElectrumServer::fromString(line, &server))
                servers.append(server);
        }
    }
    client_->setServers(servers);
    client_->setGenesis(info().genesis);

    // Rows are derived from the next event-loop turn on, even while the client
    // is still connecting, so the addresses show up offline too; requests wait
    // in pumpRequests() until the client is ready.
    scheduleDerive();
    noteProgress();
    const quint64 gen = generation_;
    for (const QString& s : std::as_const(skipped)) {
        report(Problem::Skipped, s);
        if (gen != generation_) return;
    }
    // The scanner listens by now, for the ready() this brings.
    if (!accounts_.isEmpty())
        client_->start();
    if (noValidServer) {
        // Posted, so it lands after the caller's own feedback ("Settings saved")
        // instead of being replaced by it.
        QTimer::singleShot(0, this, [this, gen] {
            if (gen == generation_ && running_)
                emit problem(Problem::BuiltInServers,
                             QStringLiteral("None of the Electrum servers in Settings is valid (host:port); using the "
                                            "built-in list."),
                             QString());
        });
    }
}

void UtxoScanner::adoptModelRows()
{
    // Rows already in the model (an earlier start(), e.g. after a settings
    // change) are adopted instead of derived again once the window reaches
    // them: no duplicates, and their last known state stays on screen
    // meanwhile. Their flags do not set the window, though: they came from the
    // previous server and gaps, and that server may be the very reason for the
    // change (a buggy or hostile one calls every address used). The window is
    // found again from this run's statuses, so another server or a smaller gap
    // really undoes a window an earlier server inflated, and the rows past it
    // are no longer subscribed. Rows that hold coins are followed from the
    // start wherever they are, so no balance on screen goes stale: the server
    // confirms them (a used one then moves the window past it) or clears them.
    QList<int> probeIndex(chains_.size(), -1);
    QList<QString> probeHash(chains_.size());
    QList<int> rowsEnd(chains_.size(), 0); // highest index in the model + 1
    QList<QStringList> funded(chains_.size());
    for (int r = 0; r < model_->size(); ++r) {
        const AddressRow& row = model_->at(r);
        if (row.network != network_ || !row.isUtxo() || row.index >= quint32(kMaxChainRows)) continue; // never derived
        const int ci = findChain(row);
        if (ci < 0) continue;
        const int index = static_cast<int>(row.index);
        rowsEnd[ci] = std::max(rowsEnd[ci], index + 1);
        if (row.used) chains_[ci].savedUsed = std::max(chains_.at(ci).savedUsed, index);
        if ((row.confirmed != 0 || row.unconfirmed != 0) && !row.scripthash.isEmpty())
            funded[ci].append(row.scripthash.toLower());
        if (probeIndex[ci] < 0 || index < probeIndex[ci]) {
            probeIndex[ci] = index;
            probeHash[ci] = row.scripthash.toLower();
        }
    }

    // One derivation per chain proves the rows came from this xpub. Rows left
    // over from another wallet are all derived again instead (floor), with none
    // of their flags trusted, so no foreign address survives on screen.
    for (int ci = 0; ci < chains_.size(); ++ci) {
        if (probeIndex[ci] < 0) continue;
        Chain& c = chains_[ci];
        try {
            const chain::AddressInfo address = chain::addressFromChainNode(
                c.node, c.type, c.account, c.change, quint32(probeIndex[ci]), info().format);
            c.reuse = QString::fromStdString(address.electrumScripthash) == probeHash[ci];
        } catch (const std::exception&) {
            c.reuse = false;
        }
        if (!c.reuse) {
            c.floor = rowsEnd[ci];
            c.savedUsed = -1;
            continue;
        }
        // Not through track(): no progress() while start() is half done.
        for (const QString& sh : std::as_const(funded[ci])) {
            if (tracked_.contains(sh)) continue;
            const int pos = model_->findByScripthash(network_, sh);
            if (pos < 0) continue;
            const AddressRow& row = model_->at(pos);
            tracked_.insert(sh, Tracked());
            placeRow(sh, c, int(row.index), row);
        }
    }
    // The addresses a payment comes to next first, as far as the rows say which they are.
    for (int ci = 0; ci < chains_.size(); ++ci) {
        const Chain& c = chains_.at(ci);
        if (c.savedUsed < 0 || !c.reuse) continue;
        for (int index = c.savedUsed + 1; index <= c.savedUsed + hotAhead(c); ++index) {
            const int pos = model_->find(c.wallet, network_, c.type, c.account, c.change, quint32(index));
            if (pos < 0) continue;
            const AddressRow& row = model_->at(pos);
            const QString sh = row.scripthash.toLower();
            if (sh.isEmpty() || tracked_.contains(sh)) continue;
            tracked_.insert(sh, Tracked());
            placeRow(sh, c, index, row);
        }
    }
}

void UtxoScanner::stop()
{
    running_ = false;
    ++generation_; // replies still on their way are dropped
    subQueue_.clear();
    readQueue_.clear();
    rounds_.clear();
    roundsHead_ = 0;
    roundsCount_ = 0;
    roundReadsOut_ = 0;
    readsOut_ = 0;
    roundFailed_.clear();
    lastRoundFailed_ = false;
    refusedReads_ = 0;
    wrongChain_ = false;
    if (readTimer_) readTimer_->stop();
    balanceQueue_.clear();
    balanceQueued_.clear();
    balanceSeq_.clear();
    retry_.clear();
    balanceRetry_.clear();
    retryArmed_ = false; // the posted round sees the old generation and does nothing
    subSent_ = 0;
    balanceSent_ = 0;
    // Scanner first, then its client: when the client fails its pending requests with
    // "stopped", nobody acts on those replies any more.
    if (client_) client_->stop();
    // tracked_ and checked_ stay, so checkedCount() / derivedCount() describe
    // the last run until the next start(). Requests already handed to the
    // client keep counting in outstanding_ until their replies arrive.
}

bool UtxoScanner::isScanning() const
{
    return busy();
}

bool UtxoScanner::connecting() const
{
    // One that answered for another chain is reason enough, and says more.
    return !accounts_.isEmpty() && client_ && !client_->isReady() && !wrongChain_;
}

bool UtxoScanner::wrongChain() const
{
    return wrongChain_ && client_ && !client_->isReady();
}

bool UtxoScanner::refreshFailing() const
{
    if (!running_)
        return false;
    // With nothing left to read there is nothing for these to hold back.
    const bool reading = roundsCount_ > 0 || !readQueue_.isEmpty() || !retry_.isEmpty();
    if (!roundFailed_.isEmpty() || (reading && (lastRoundFailed_ || refusedReads_ >= kRefusedReadsForFailing)))
        return true;
    // No whole round for this long: an outage, a night's sleep, reads held back. Until the
    // first answers are in, the time is from an earlier run and says so anyway. Two rounds
    // and the wait between them, at the pace rounds really keep.
    if (!answeredSaid_ || roundsCount_ == 0 || !balancesAt_.isValid())
        return false;
    const qint64 planned = std::max({qint64(roundsCount_) * 1000 / kReadsPerSecond, qint64(minRoundMs()), qint64(lastRoundSeconds_) * 1000});
    return balancesAt_.msecsTo(QDateTime::currentDateTime()) > 2 * planned + minRoundMs() + kRoundGraceMs;
}

bool UtxoScanner::answered() const
{
    // busy() without the windows: one cut at the row limit never ends, and everything
    // this scan does follow has answered all the same.
    return running_ && !deriveScheduled_ && checked_ >= tracked_.size() && subQueue_.isEmpty() && readQueue_.isEmpty()
           && subSent_ == 0 && retry_.isEmpty() && balanceQueue_.isEmpty() && balanceSent_ == 0 && balanceRetry_.isEmpty();
}

QDateTime UtxoScanner::balancesAt() const
{
    // Followed live: every answer stands for now, for as long as nothing waits and the
    // server can still be heard (after a night's sleep the socket does not know yet). Read
    // in rounds, what the last whole round read stands for when that round began.
    if (running_ && !busy() && client_ && client_->isReady() && roundsCount_ == 0) {
        const QDateTime now = QDateTime::currentDateTime();
        const QDateTime heard = client_->lastHeardAt();
        return heard.isValid() && heard.secsTo(now) > kQuietSeconds ? heard : now;
    }
    return balancesAt_;
}

void UtxoScanner::restoreTimes(const QDateTime& balancesAt, const QDateTime&, const NonceSweep&)
{
    // Of rows that are there: the database keeps a time after its rows have gone.
    if (!balancesAt_.isValid() && model_ && model_->rowsOn(network_) > 0)
        balancesAt_ = balancesAt;
}

void UtxoScanner::refreshNow()
{
    if (!running_ || roundsCount_ == 0)
        return; // none to read
    // Someone is waiting: a read goes out now, and says whether the server is back.
    readsPausedUntilMs_ = 0;
    if (roundEndMs_ != 0)
        forceRound_ = true; // one under way goes on
    schedulePump();
}

QString UtxoScanner::serverName() const
{
    return client_ ? client_->serverName() : QString();
}

ScanTiers UtxoScanner::tiers() const
{
    ScanTiers t;
    t.live = liveCount_;
    t.rounds = roundsCount_;
    if (roundsCount_ > 0) {
        // The last round's length, or what the pace makes of this one.
        const int planned = std::max(roundsCount_ / kReadsPerSecond, minRoundMs() / 1000);
        t.roundSeconds = std::max(lastRoundSeconds_, planned);
    }
    return t;
}

bool UtxoScanner::limitReached() const
{
    // A window that wants rows this scan may not derive: the row limit (or, for
    // a gap larger than the limit, the end of a chain's indices) cuts it short.
    // While it does, the scan never finishes (busy()), and the addresses past
    // the cut go unchecked.
    if (!running_) return false;
    return std::any_of(chains_.cbegin(), chains_.cend(),
                       [this](const Chain& c) { return c.next < c.window() && !mayDerive(c); });
}

int UtxoScanner::checkedCount() const
{
    return checked_;
}

int UtxoScanner::derivedCount() const
{
    return static_cast<int>(tracked_.size());
}

// ------------------------------------------------------------ client events ----

void UtxoScanner::onClientReady()
{
    if (!running_ || !client_) return;
    // A new connection: after a drop (possibly to another server), or the
    // client started again under the scan, which forgets every subscription.
    // The client would restore the subscriptions by itself, but a refused
    // restore never reaches this scanner (for an unused address not even as a
    // notification): the address would still count as followed while no
    // server watches it and nothing polls it. So the scanner subscribes again
    // everything it follows, from here, and the client restores nothing whose
    // subscribe is already queued when ready() returns. Every answer comes
    // back through onSubscribeReply(): a refusal goes to Retry and is polled
    // as a read, a status that moved meanwhile is applied. Subscriptions
    // still on their way (Sent) are re-sent by the client itself. Another connection may
    // take as many as there are places, whatever the last one refused.
    const QString server = client_->serverName();
    const bool otherServer = !lastServer_.isEmpty() && server != lastServer_;
    lastServer_ = server;
    wrongChain_ = false;
    placesHere_ = livePlaces();
    refusalsHere_ = 0;
    refusedInARow_ = 0;
    answersHere_ = 0;
    refusedHere_ = 0;
    // The pause a server asked for holds there, also when it closed on us, and nowhere else.
    if (server != pauseServer_) {
        readsPausedUntilMs_ = 0;
        readPauseMs_ = 0;
        refusedReads_ = 0;
    }
    if (roundEndMs_ == 0 && roundStartMs_ != 0)
        roundInterrupted_ = true;
    retry_.clear();
    QStringList again;
    QStringList placed;
    // What it will not read it did not read yesterday either: that goes with the server.
    if (otherServer)
        excusedRows_ = 0;
    for (auto it = tracked_.begin(); it != tracked_.end(); ++it) {
        it->viaRead = false;
        if (otherServer) {
            it->excused = false;
            it->readRefused = false;
            it->balanceAnswered = false;
        }
        if (it->sub == SubState::Live) {
            if (liveCount_ > 0)
                --liveCount_; // taken again by the subscribe below
            it->sub = SubState::Queued;
            again.append(it.key());
        } else if (it->sub == SubState::Retry) {
            it->sub = SubState::Queued;
            placed.append(it.key());
        }
    }
    // Rows it has not gone back on: what deserves a place asks for one, the rest at the
    // pace reads keep, and the client restores none of it by itself.
    for (const QString& sh : std::as_const(placed)) {
        client_->forgetScripthash(sh);
        const int pos = model_ ? model_->findByScripthash(network_, sh) : -1;
        const int ci = pos >= 0 ? findChain(model_->at(pos)) : -1;
        if (ci >= 0)
            placeRow(sh, chains_.at(ci), int(model_->at(pos).index), model_->at(pos));
        else
            readQueue_.append(sh);
    }
    // What a server refused may have made a small wallet look large.
    const bool wasTiered = tiered_;
    tiered_ = false;
    noteTiered();
    if (wasTiered && !tiered_) {
        QTimer::singleShot(0, this, [this, gen = generation_] {
            if (gen == generation_ && running_) rebalanceLive();
        });
    }
    // What the last server said a used address holds is its word: the new one is asked,
    // at the pace balances keep. The same status says nothing about that.
    if (otherServer && model_) {
        for (auto it = tracked_.cbegin(); it != tracked_.cend(); ++it) {
            const int pos = model_->findByScripthash(network_, it.key());
            if (pos >= 0 && model_->at(pos).used)
                queueBalance(it.key());
        }
    }
    // All of them now, past kMaxOutstanding: only what is queued before ready()
    // returns keeps the client from restoring it a second time, and the client
    // would have queued just as many.
    const quint64 gen = generation_;
    for (const QString& sh : std::as_const(again)) {
        if (gen != generation_ || !running_) return;
        if (client_ && client_->isReady())
            sendSubscribe(sh);
        else
            subQueue_.append(sh); // the connection went meanwhile: paced, once it is back
    }
    // Failed balances get another try now, possibly on another server,
    // without waiting for the retry round.
    const QSet<QString> balances = std::exchange(balanceRetry_, QSet<QString>());
    for (const QString& sh : balances) queueBalance(sh);
    idleReported_ = false;
    schedulePump();
}

void UtxoScanner::onScripthashChanged(const QString& scripthash, const QString& status)
{
    if (!running_ || !model_) return;
    const int pos = model_->findByScripthash(network_, scripthash);
    if (pos < 0) return;
    const AddressRow& row = model_->at(pos);
    if (!row.isUtxo() || findChain(row) < 0) return; // not ours
    const QString sh = row.scripthash.toLower(); // the spelling tracked_ uses

    QString text;
    const int parsed = parseStatusText(status, &text);
    if (parsed < 0) {
        // It proves neither history nor its absence; the balance does (coins
        // mark the address used).
        queueBalance(sh);
        schedulePump();
        report(Problem::Failure, QStringLiteral("An Electrum server sent an invalid address status."), status);
        return;
    }
    // A subscribe reply already on its way may predate this status; one sent
    // later (or a row not followed yet in this run) is at least as new.
    const auto it = tracked_.find(sh);
    if (it != tracked_.end()) {
        if (it->sub == SubState::Sent) it->notified = true;
        it->status = text;
    }
    const quint64 gen = generation_;
    applyStatus(sh, parsed == 1, text);
    if (gen != generation_) return;
    // The status changes with every new transaction, confirmation or reorg,
    // so the balance is fetched again even when `used` stayed the same.
    queueBalance(sh);
    markConfirmed(sh);
    schedulePump();
}

void UtxoScanner::onModelCleared()
{
    // What balancesAt() dated is gone with the rows, stopped or not, unless other wallets'
    // answered rows stayed: those figures are as old as ever.
    if (model_ && model_->rowsOn(network_) == model_->uncheckedOn(network_))
        balancesAt_ = QDateTime();
    if (!running_) {
        tracked_.clear();
        checked_ = 0;
        return;
    }
    // Every row this scan follows is gone (the owner cleared the model without
    // restarting the scanner): start over, after the model's other listeners
    // have seen the clear.
    QTimer::singleShot(0, this, [this, gen = generation_] {
        if (gen == generation_ && running_) start();
    });
}

// --------------------------------------------------------------- derivation ----

void UtxoScanner::scheduleDerive()
{
    idleReported_ = false;
    if (deriveScheduled_) return;
    deriveScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        deriveScheduled_ = false;
        deriveSlice();
    });
}

void UtxoScanner::deriveSlice()
{
    if (!running_ || !model_) return;
    const quint64 gen = generation_;
    int slice = kDeriveSlice;
    int adopted = kAdoptSlice;
    for (int ci = 0; ci < chains_.size() && slice > 0 && adopted > 0; ++ci) {
        while (slice > 0 && adopted > 0 && mayDerive(chains_.at(ci))) {
            const int index = chains_[ci].next++;
            const int derived = adoptOrDerive(ci, index);
            slice -= derived;
            adopted -= 1 - derived;
            // A model listener may have restarted or stopped the scan.
            if (gen != generation_ || !running_ || !model_) return;
        }
    }
    noteTiered();
    const bool more = std::any_of(chains_.cbegin(), chains_.cend(), [this](const Chain& c) { return mayDerive(c); });
    if (more) {
        scheduleDerive();
    } else {
        reportLimits();
        if (gen != generation_ || !running_) return; // a problem() listener restarted or stopped the scan
    }
    schedulePump();
    checkIdle();
}

int UtxoScanner::adoptOrDerive(int chainIndex, int index)
{
    const Chain& c = chains_.at(chainIndex);
    const quint32 i = quint32(index);
    const int pos = model_->find(c.wallet, network_, c.type, c.account, c.change, i);
    if (pos >= 0 && c.reuse) {
        const QString sh = model_->at(pos).scripthash.toLower();
        if (!sh.isEmpty()) {
            if (!tracked_.contains(sh)) track(sh);
            return 0;
        }
    }

    AddressRow row;
    row.wallet = c.wallet;
    row.network = network_;
    row.type = c.type;
    row.account = c.account;
    row.change = c.change;
    row.index = i;
    try {
        const chain::AddressInfo address = chain::addressFromChainNode(c.node, c.type, c.account, c.change, i, info().format);
        row.address = QString::fromStdString(address.address);
        row.scripthash = QString::fromStdString(address.electrumScripthash);
    } catch (const std::exception& e) {
        // CKDpub fails for about one index in 2^127; BIP32 says to skip it, so
        // no address is missing and there is nothing to report as a problem.
        qInfo().noquote() << QStringLiteral("%1 skipped: %2")
                                 .arg(QString::fromStdString(chain::accountPath(c.type, c.account, c.change, i, info().format)),
                                      QString::fromUtf8(e.what()));
        return 1;
    }
    const QString sh = row.scripthash;
    const quint64 gen = generation_;
    model_->upsert(row); // also replaces a foreign row at this position
    if (gen == generation_ && running_ && !tracked_.contains(sh)) track(sh);
    return 1;
}

void UtxoScanner::track(const QString& scripthash)
{
    const int pos = model_ ? model_->findByScripthash(network_, scripthash) : -1;
    const int ci = pos >= 0 ? findChain(model_->at(pos)) : -1;
    tracked_.insert(scripthash, Tracked());
    if (ci >= 0)
        placeRow(scripthash, chains_.at(ci), int(model_->at(pos).index), model_->at(pos));
    else
        subQueue_.append(scripthash);
    idleReported_ = false;
    noteProgress();
}

void UtxoScanner::noteTiered()
{
    // What is here and what the gaps still ask for: a wallet that fits is followed
    // whole, from its first address on. A walk over the chains: asked once per slice of
    // rows, not per row.
    if (tiered_ || qint64(tracked_.size()) + reservedRows() <= placesNow())
        return;
    tiered_ = true;
    // What took a place while everything fit may not deserve one now. After this turn:
    // a caller may be in the middle of a row.
    QTimer::singleShot(0, this, [this, gen = generation_] {
        if (gen == generation_ && running_) rebalanceLive();
    });
}

int UtxoScanner::placesNow() const
{
    return std::min(livePlaces(), placesHere_);
}

void UtxoScanner::rebalanceLive()
{
    // Those who lost their claim first: their places go to those who have one.
    const QStringList everyRow = tracked_.keys();
    for (const SubState state : {SubState::Live, SubState::Rounds}) {
        for (const QString& sh : everyRow) {
            const auto it = tracked_.constFind(sh);
            if (it != tracked_.cend() && it->sub == state)
                reconsider(sh);
        }
    }
    // Rows on their way again: the next idle is one more to report.
    idleReported_ = false;
    schedulePump();
}

void UtxoScanner::freeCoinPlace(Tracked& tracked)
{
    if (!tracked.forCoins) return;
    tracked.forCoins = false;
    if (coinPlaces_ > 0) --coinPlaces_;
}

void UtxoScanner::enterRounds(const QString& scripthash, Tracked& tracked)
{
    if (tracked.sub != SubState::Rounds) {
        // The first read in rounds: what stood for now until here stops doing so.
        if (roundsCount_ == 0 && answeredSaid_ && client_) {
            const QDateTime heard = client_->lastHeardAt();
            balancesAt_ = heard.isValid() ? heard : QDateTime::currentDateTime();
        }
        ++roundsCount_;
    }
    tracked.sub = SubState::Rounds;
    if (!tracked.inRounds) {
        tracked.inRounds = true;
        rounds_.append(scripthash);
    }
}

int UtxoScanner::hotAhead(const Chain& c) const
{
    return c.change == 0 ? hotReceive_ : std::min(hotReceive_, kHotAheadOnChange);
}

bool UtxoScanner::isNext(const Chain& c, int index) const
{
    const int last = std::max(c.lastUsed, c.savedUsed);
    return index > last && index <= last + hotAhead(c);
}

bool UtxoScanner::wantsLive(const Chain& c, int index, const AddressRow& row) const
{
    return !tiered() || isNext(c, index) || row.confirmed != 0 || row.unconfirmed != 0;
}

bool UtxoScanner::mayGoLive(Tracked& tracked, const Chain& c, int index)
{
    if (tracked.viaRead || liveCount_ >= placesNow()) return false;
    if (!tiered() || isNext(c, index)) return true;
    // Holding coins: within what the next unused addresses leave, however many hold some.
    if (coinPlaces_ >= placesNow() - hotPlaces()) return false;
    ++coinPlaces_;
    tracked.forCoins = true;
    return true;
}

void UtxoScanner::placeRow(const QString& scripthash, const Chain& c, int index, const AddressRow& row)
{
    const auto it = tracked_.find(scripthash);
    const bool live = it != tracked_.end() && wantsLive(c, index, row) && mayGoLive(*it, c, index);
    (live ? subQueue_ : readQueue_).append(scripthash);
}

void UtxoScanner::reconsider(const QString& scripthash)
{
    if (!running_ || !model_ || !client_) return;
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end()) return;
    const int pos = model_->findByScripthash(network_, scripthash);
    const int ci = pos >= 0 ? findChain(model_->at(pos)) : -1;
    if (ci < 0) return;
    const Chain& c = chains_.at(ci);
    const int index = int(model_->at(pos).index);
    const bool wants = wantsLive(c, index, model_->at(pos));
    if (wants && it->sub == SubState::Rounds && mayGoLive(*it, c, index)) {
        it->sub = SubState::Queued; // rounds_ skips it from now on
        if (roundsCount_ > 0) --roundsCount_;
        roundFailed_.remove(scripthash); // the subscribe answers for it
        subQueue_.append(scripthash);
        schedulePump();
    } else if (!wants && it->sub == SubState::Live && tiered()) {
        client_->unsubscribeScripthash(scripthash);
        if (liveCount_ > 0) --liveCount_;
        freeCoinPlace(*it);
        enterRounds(scripthash, *it);
        schedulePump();
    }
}

void UtxoScanner::reconsiderRange(int chainIndex, int from, int to)
{
    const Chain& c = chains_.at(chainIndex);
    for (int index = from; index <= to; ++index) {
        const int pos = model_->find(c.wallet, network_, c.type, c.account, c.change, quint32(index));
        if (pos >= 0) reconsider(model_->at(pos).scripthash.toLower());
    }
}

int UtxoScanner::rowLimitFor(qint64 gaps)
{
    return static_cast<int>(std::clamp(gaps * kRowsPerGapRow, qint64(kMinWalletRows), qint64(kMaxWalletRows)));
}

int UtxoScanner::rowLimit() const
{
    qint64 gaps = 0;
    for (const Chain& c : chains_) gaps += c.gap;
    return rowLimitFor(gaps);
}

qint64 UtxoScanner::reservedRows() const
{
    // Kept free inside the limit: a chain that grows first must not crowd
    // another out of the addresses its gap alone asks for.
    qint64 rows = 0;
    for (const Chain& c : chains_) rows += std::max(0, c.base() - c.next);
    return rows;
}

bool UtxoScanner::mayDerive(const Chain& c) const
{
    if (c.next >= c.target()) return false;
    // What the gap alone asks for, unless so many wallets add up past the ceiling
    // that a server would refuse the rest anyway.
    if (c.next < c.base()) return tracked_.size() < kMaxWalletRows;
    // Growth a used status asked for: only within the row limit. Rows of the windows: what
    // is followed only for the coins an earlier run saved is no reason to stop.
    qint64 derived = 0;
    for (const Chain& chain : chains_) derived += chain.next;
    return derived + reservedRows() < rowLimit();
}

// ----------------------------------------------------------------- requests ----

void UtxoScanner::schedulePump()
{
    // Always from the event loop: replies can arrive inside the client's own
    // call stack (even inside its stop() or destructor), which must not be
    // re-entered with new requests.
    if (pumpScheduled_) return;
    pumpScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        pumpScheduled_ = false;
        pumpRequests();
    });
}

void UtxoScanner::pumpRequests()
{
    while (running_ && client_ && client_->isReady() && outstanding_ < kMaxOutstanding) {
        // Balances first: they are few (used addresses only) and they are
        // what the totals wait for. Then what is followed live, then what is read,
        // at the pace reads keep: first reads, what failed, then the round.
        // A wallet this large asks for a balance per used address at every start: those
        // keep the pace of reads, and a pause a server asked for.
        const bool paced = tiered();
        if (!balanceQueue_.isEmpty() && !(paced && readsPaused())) {
            if (paced && !takeReadToken()) break;
            sendBalance(balanceQueue_.takeFirst());
        } else if (!subQueue_.isEmpty()) {
            sendSubscribe(subQueue_.takeFirst());
        } else if (readsPaused() || (readPauseMs_ > 0 && readsOut_ > 0)) {
            break; // the read timer comes back; after a refusal, one read at a time until one answers
        } else if (!readQueue_.isEmpty()) {
            if (!takeReadToken()) break;
            sendRead(readQueue_.takeFirst(), false);
        } else if (roundsCount_ > 0) {
            if (roundStartMs_ == 0) {
                // The first reads have just read them all: the clock starts here.
                roundStartMs_ = roundEndMs_ = nowMs();
                armRoundTimer();
            }
            if (roundEndMs_ != 0 && !forceRound_ && nowMs() - roundStartMs_ < minRoundMs()) break; // armRoundTimer() comes back for it
            if (roundEndMs_ == 0 && roundsHead_ >= rounds_.size()) break; // waiting for its replies
            if (!takeReadToken()) break;
            nextRoundRead();
        } else {
            break;
        }
    }
    const bool roundDue = roundsCount_ > 0 && (roundEndMs_ == 0 || forceRound_ || nowMs() - roundStartMs_ >= minRoundMs());
    const bool reads = running_ && (!readQueue_.isEmpty() || roundDue || (tiered() && !balanceQueue_.isEmpty()));
    if (reads && !readTimer_->isActive())
        readTimer_->start();
    else if (!reads && readTimer_->isActive())
        readTimer_->stop();
    checkIdle();
}

bool UtxoScanner::takeReadToken()
{
    if (readTokens_ <= 0) return false;
    --readTokens_;
    return true;
}

void UtxoScanner::nextRoundRead()
{
    if (roundEndMs_ != 0) {
        roundStartMs_ = nowMs();
        roundStartedAt_ = QDateTime::currentDateTime();
        roundEndMs_ = 0;
        roundsHead_ = 0;
        forceRound_ = false;
    }
    // A row that went live meanwhile keeps its place, in case it comes back; it is just
    // not read here. Nothing is sent when that was all that was left.
    while (roundsHead_ < rounds_.size()) {
        const QString sh = rounds_.at(roundsHead_++);
        const auto it = tracked_.find(sh);
        if (it != tracked_.end() && it->sub == SubState::Rounds) {
            sendRead(sh, true);
            return;
        }
    }
    finishRoundIfDone();
}

void UtxoScanner::finishRoundIfDone()
{
    // Whole when every read is back, and the balances that may make up for a refused one:
    // what it read stands for when it began, unless some of it did not answer. Those are
    // asked again by the next round; none is worth holding the others back.
    if (roundStartMs_ == 0 || roundEndMs_ != 0 || roundsHead_ < rounds_.size() || roundReadsOut_ > 0) return;
    if (!roundFailed_.isEmpty() && (balanceSent_ > 0 || !balanceQueue_.isEmpty())) return;
    roundEndMs_ = nowMs();
    if (!roundInterrupted_)
        lastRoundSeconds_ = int((roundEndMs_ - roundStartMs_) / 1000);
    roundInterrupted_ = false;
    // A balance makes up for an address a server will not read, not for a server that reads
    // nothing: a payment to an empty address shows in neither.
    lastRoundFailed_ = !roundFailed_.isEmpty() || refusedInRound_ > kExcusedReads;
    refusedInRound_ = 0;
    roundFailed_.clear();
    if (!lastRoundFailed_)
        balancesAt_ = roundStartedAt_;
    armRoundTimer();
    noteProgress(); // the time moved, and maybe what the status says
    emit passFinished();
}

void UtxoScanner::armRoundTimer()
{
    const qint64 wait = minRoundMs() - (nowMs() - roundStartMs_);
    QTimer::singleShot(std::chrono::milliseconds(std::max<qint64>(wait, 0) + 1), this, [this, gen = generation_] {
        if (gen == generation_) schedulePump();
    });
}

void UtxoScanner::pauseReads()
{
    // Once per pause: the replies of one burst all say the same.
    if (readsPaused()) return;
    readPauseMs_ = readPauseMs_ <= 0 ? readPauseFirstMs() : std::min(readPauseMs_ * 2, kReadPauseMaxMs);
    readsPausedUntilMs_ = nowMs() + readPauseMs_;
    pauseServer_ = client_ ? client_->serverName() : QString();
}

void UtxoScanner::noteRefusal(bool read)
{
    const qint64 now = nowMs();
    if (read && refusedReads_++ == 0) firstRefusedMs_ = now;
    ++refusedInARow_;
    // Having followed some is a limit of the server's, not a reason to leave it.
    const bool nothingAnswered = answersHere_ == 0 && refusedInARow_ >= kRefusedForAMove;
    const bool readsGone = refusedReads_ >= kReadsRefusedForAMove && now - firstRefusedMs_ >= kReadsRefusedMoveMs;
    if (!nothingAnswered && !readsGone) return;
    refusedInARow_ = 0;
    if (client_) client_->tryAnotherServer(QStringLiteral("refuses what it is asked"));
}

bool UtxoScanner::readsPaused() const
{
    return nowMs() < readsPausedUntilMs_;
}

void UtxoScanner::sendRead(const QString& scripthash, bool round)
{
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end() || (!round && it->sub != SubState::Queued)) return;
    if (round) {
        ++roundReadsOut_;
    } else {
        it->sub = SubState::Sent;
        ++subSent_;
    }
    ++readsOut_;
    ++outstanding_;
    const QPointer<UtxoScanner> self(this);
    const quint64 gen = generation_;
    client_->request(QStringLiteral("blockchain.scripthash.get_history"), QJsonArray{scripthash},
                     [self, gen, scripthash, round](const QJsonValue& result, const QString& error) {
        if (!self) return;
        QString status;
        const int parsed = error.isEmpty() ? statusOfHistory(result, &status) : -1;
        self->onReadReply(gen, scripthash, round, parsed, status, error);
    });
}

void UtxoScanner::onReadReply(quint64 generation, const QString& scripthash, bool round, int status,
                              const QString& statusText, const QString& error)
{
    if (outstanding_ > 0) --outstanding_;
    schedulePump();
    if (generation != generation_ || !running_) return;
    if (readsOut_ > 0) --readsOut_;
    if (round) {
        if (roundReadsOut_ > 0) --roundReadsOut_;
    } else if (subSent_ > 0) {
        --subSent_;
    }
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end()) return;
    // Gone live or back in a queue while a round's read was out: what comes of that
    // says what matters, and the place it holds now is not this reply's to change.
    if (round && it->sub != SubState::Rounds) {
        finishRoundIfDone();
        return;
    }

    if (!error.isEmpty() || status < 0) {
        const bool dropped = droppedByClient(error);
        if (!dropped) {
            ++refusedHere_;
            it->readRefused = true;
            if (it->balanceAnswered && !it->excused) {
                it->excused = true;
                ++excusedRows_;
            }
        }
        // An address it refused before, whose balance answered, says nothing new about the
        // server. A few of those are long histories; many is a server not reading.
        const bool excused = it->excused && excusedRows_ <= kExcusedReads;
        if (round) {
            // The row keeps what it showed, and this round is not whole. If its balance
            // answers, that will do for an address a server will not read for us (some
            // refuse long histories): one known for that is only asked for its balance.
            if (!excused) {
                roundFailed_.insert(scripthash);
                if (!dropped) ++refusedInRound_;
                noteProgress(); // refreshFailing() moved
            }
        } else {
            // Read again after the next ready() or retry round, at the pace reads keep.
            it->sub = SubState::Retry;
            retry_.append(scripthash);
            armRetry();
        }
        if (!dropped) queueBalance(scripthash);
        if (!dropped) {
            if (!excused) {
                pauseReads();
                noteRefusal(true);
            }
            report(Problem::Failure,
                   error.isEmpty() ? QStringLiteral("An Electrum server sent an invalid address history.")
                                   : QStringLiteral("An Electrum server refused to read an address."),
                   error);
        }
        if (generation != generation_) return;
        // Its balance may have answered while this was out.
        if (!round) settleRefusedRead(scripthash);
        if (generation != generation_) return;
        if (round) finishRoundIfDone();
        checkIdle();
        return;
    }

    readPauseMs_ = 0;
    readsPausedUntilMs_ = 0;
    refusedReads_ = 0;
    refusedInARow_ = 0;
    ++answersHere_;
    it->readRefused = false;
    if (it->excused) {
        it->excused = false;
        --excusedRows_;
    }
    const bool wasUnanswered = std::exchange(it->unanswered, false);
    enterRounds(scripthash, *it);
    roundFailed_.remove(scripthash);
    if (!it->confirmed || it->status != statusText || wasUnanswered) {
        const bool known = it->confirmed;
        it->status = statusText;
        applyStatus(scripthash, status == 1, statusText);
        if (generation != generation_) return;
        // A read that finds another history than the last one: what it holds moved too.
        if (known && status == 1) queueBalance(scripthash);
    }
    markConfirmed(scripthash);
    reconsider(scripthash);
    if (round) finishRoundIfDone();
    checkIdle();
}

void UtxoScanner::sendSubscribe(const QString& scripthash)
{
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end() || it->sub != SubState::Queued) return;
    // The places went while it waited: read like the others until one comes free.
    if (it->viaRead || liveCount_ >= placesNow()) {
        readQueue_.append(scripthash);
        freeCoinPlace(*it);
        return;
    }
    it->sub = SubState::Sent;
    it->notified = false;
    ++subSent_;
    ++outstanding_;
    ++liveCount_;
    const QPointer<UtxoScanner> self(this);
    const quint64 gen = generation_;
    client_->subscribeScripthash(scripthash, [self, gen, scripthash](const QJsonValue& result,
                                                                     const QString& error) {
        // Runs exactly once, possibly after the scanner is gone.
        if (!self) return;
        QString status;
        const int parsed = error.isEmpty() ? parseStatus(result, &status) : -1;
        self->onSubscribeReply(gen, scripthash, parsed, status, error);
    });
}

void UtxoScanner::onSubscribeReply(quint64 generation, const QString& scripthash, int status,
                                  const QString& statusText, const QString& error)
{
    if (outstanding_ > 0) --outstanding_;
    schedulePump();
    if (generation != generation_ || !running_) return; // a reply to an earlier start()
    if (subSent_ > 0) --subSent_;
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end()) return;

    if (!error.isEmpty() || status < 0) {
        if (liveCount_ > 0) --liveCount_;
        freeCoinPlace(*it);
        if (droppedByClient(error)) {
            // Nobody refused it: the client was stopped under the scan. The
            // round armed here asks again should no ready() come first.
            it->sub = SubState::Retry;
            retry_.append(scripthash);
            armRetry();
            return;
        }
        // Refused, which is no reason yet to look for another server: the address is read
        // like one with no place, so it does not wait on what this server will not do.
        // Enough of these are a limit of the server's, and it keeps what it took. A read
        // does not need the subscription, nor does get_balance: coins on an address this
        // server refuses to follow (some refuse very long histories) still reach the
        // total, and mark it used, so the window grows past it.
        client_->forgetScripthash(scripthash);
        ++refusedHere_;
        noteRefusal(false);
        it->viaRead = true;
        it->sub = SubState::Queued;
        readQueue_.append(scripthash);
        queueBalance(scripthash);
        if (++refusalsHere_ >= kRefusalsForALimit && placesHere_ > liveCount_) {
            placesHere_ = liveCount_; // none at all, if it follows nothing
            noteTiered();
        }
        schedulePump();
        report(Problem::Failure,
               error.isEmpty() ? QStringLiteral("An Electrum server sent an invalid address status.")
                               : QStringLiteral("An Electrum server refused to follow an address."),
               error);
        return;
    }

    it->sub = SubState::Live;
    refusedInARow_ = 0;
    ++answersHere_;
    it->readRefused = false;
    const bool wasUnanswered = std::exchange(it->unanswered, false);
    // A notification that arrived while this reply was on its way is newer.
    // A subscription made again after a reconnect that finds the status it
    // knew changes nothing: the balance that status implies is known, or on
    // its way (a pending request is sent again, a failed one retried).
    if (!it->notified && (wasUnanswered || !(it->confirmed && it->status == statusText))) {
        it->status = statusText;
        applyStatus(scripthash, status == 1, statusText);
        if (generation != generation_) return;
    }
    markConfirmed(scripthash);
    reconsider(scripthash);
    checkIdle();
}

bool UtxoScanner::droppedByClient(const QString& error) const
{
    // ElectrumClient::stop() fails every pending request with "stopped", after
    // it stopped being ready. A server can send that very text as well, but a
    // server's reply only ever arrives while the client is ready: the state
    // tells the two apart where the text cannot.
    return error == QLatin1String("stopped") && (!client_ || !client_->isReady());
}

void UtxoScanner::queueBalance(const QString& scripthash)
{
    idleReported_ = false;
    if (balanceQueued_.contains(scripthash)) return;
    balanceQueued_.insert(scripthash);
    balanceQueue_.append(scripthash);
}

void UtxoScanner::sendBalance(const QString& scripthash)
{
    balanceQueued_.remove(scripthash);
    const quint64 seq = ++nextBalanceSeq_;
    balanceSeq_.insert(scripthash, seq);
    ++balanceSent_;
    ++outstanding_;
    const QPointer<UtxoScanner> self(this);
    const quint64 gen = generation_;
    const qint64 maxUnits = maxUnitsOf(assetInfo(info().coin));
    client_->request(QStringLiteral("blockchain.scripthash.get_balance"), QJsonArray{scripthash},
                     [self, gen, seq, scripthash, maxUnits](const QJsonValue& result, const QString& error) {
        if (!self) return;
        qint64 confirmed = 0;
        qint64 unconfirmed = 0;
        const bool ok = error.isEmpty() && parseBalance(result, maxUnits, &confirmed, &unconfirmed);
        self->onBalanceReply(gen, seq, scripthash, ok, confirmed, unconfirmed, error);
    });
}

void UtxoScanner::onBalanceReply(quint64 generation, quint64 seq, const QString& scripthash, bool ok,
                                qint64 confirmed, qint64 unconfirmed, const QString& error)
{
    if (outstanding_ > 0) --outstanding_;
    schedulePump();
    if (generation != generation_ || !running_) return;
    if (balanceSent_ > 0) --balanceSent_;
    // Only the newest request per address counts: replies can come back out
    // of order, and an older one may predate the transaction that triggered
    // the newer.
    if (balanceSeq_.value(scripthash) == seq) {
        balanceSeq_.remove(scripthash);
        if (ok) {
            balanceRetry_.remove(scripthash);
            refusedInARow_ = 0;
            ++answersHere_;
            const auto answered = tracked_.find(scripthash);
            if (answered != tracked_.end())
                answered->balanceAnswered = true;
            // A read this round could not get: the balance is what mattered of it.
            if (roundFailed_.remove(scripthash)) {
                if (answered != tracked_.end() && !answered->excused) {
                    answered->excused = true;
                    ++excusedRows_;
                }
                noteProgress();
            }
            applyBalance(scripthash, confirmed, unconfirmed);
            if (generation != generation_) return;
            settleRefusedRead(scripthash);
            if (generation != generation_) return;
            finishRoundIfDone();
        } else {
            // The old balance stays meanwhile, and so does the row's check: a
            // used address whose first balance this is stays unchecked. Nothing
            // else would ask again: a reconnect subscribes again without
            // fetching the balance when the status is unchanged, and a shop
            // address that is paid once never changes again. Until the retry
            // gets through, the scan is not finished, so no total is presented
            // as complete. The same when the client dropped the request because
            // it was stopped under the scan, except that no server failed, so
            // nothing is reported.
            balanceRetry_.insert(scripthash);
            armRetry();
            if (!droppedByClient(error)) {
                ++refusedHere_;
                // All there is to say of an address it will not read.
                const auto refused = tracked_.constFind(scripthash);
                if (refused != tracked_.cend() && refused->excused && refused->sub == SubState::Rounds)
                    roundFailed_.insert(scripthash);
                if (tiered()) pauseReads();
                noteRefusal(false);
                report(Problem::Failure,
                       error.isEmpty() ? QStringLiteral("An Electrum server sent an invalid balance.")
                                       : QStringLiteral("An Electrum server refused a balance request."),
                       error);
            }
            if (generation != generation_) return;
            // A round waiting on this balance has nothing more to wait for.
            finishRoundIfDone();
        }
        if (generation != generation_) return;
    }
    checkIdle();
}

void UtxoScanner::settleRefusedRead(const QString& scripthash)
{
    // This server will not read it (some refuse very long histories). If its row says what
    // it holds, that will do: it joins the rounds, which go on asking, and the scan does not
    // wait for it.
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end() || !it->readRefused || !it->balanceAnswered || !model_ || !client_) return;
    // Waiting for its next read, not with one out: the reply of that decides.
    if (it->sub != SubState::Retry && it->sub != SubState::Queued) return;
    const int pos = model_->findByScripthash(network_, scripthash);
    if (pos < 0) return;
    if (!model_->at(pos).checked) {
        // A zero proves nothing: another server may say. Each gets that chance, once for
        // this row, and when none is left the scan ends with it unanswered, and it reads so.
        if (++it->serversTried < client_->serverCount() && client_->tryAnotherServer(QStringLiteral("refuses to read an address")))
            return;
        it->unanswered = true;
    }
    if (!it->excused) {
        it->excused = true;
        ++excusedRows_;
    }
    enterRounds(scripthash, *it);
    markConfirmed(scripthash);
    recount(scripthash);
}

void UtxoScanner::armRetry()
{
    idleReported_ = false;
    if (retryArmed_ || !running_) return;
    // After ten quiet minutes, whatever failed before evidently got through:
    // a new failure starts the backoff over.
    if (retryDelayMs_ <= 0 || nowMs() - lastRetryMs_ > 2 * qint64(kRetryMaxMs)) retryDelayMs_ = retryFirstMs();
    retryArmed_ = true;
    QTimer::singleShot(std::chrono::milliseconds(retryDelayMs_), this, [this, gen = generation_] {
        if (gen != generation_) return; // start() / stop() disarmed it
        retryArmed_ = false;
        lastRetryMs_ = nowMs();
        retryDelayMs_ = std::min(retryDelayMs_ * 2, kRetryMaxMs);
        requeueFailed();
        schedulePump();
    });
    // The scan is busy again (a failure may come long after scanFinished()):
    // listeners re-read isScanning() on progress().
    noteProgress();
}

void UtxoScanner::requeueFailed()
{
    const QStringList again = std::exchange(retry_, QStringList());
    for (const QString& sh : again) {
        const auto it = tracked_.find(sh);
        if (it == tracked_.end() || it->sub != SubState::Retry) continue;
        it->sub = SubState::Queued;
        // What deserves a place asks for one, the rest at the pace reads keep.
        const int pos = model_ ? model_->findByScripthash(network_, sh) : -1;
        const int ci = pos >= 0 ? findChain(model_->at(pos)) : -1;
        if (ci >= 0)
            placeRow(sh, chains_.at(ci), int(model_->at(pos).index), model_->at(pos));
        else
            readQueue_.append(sh);
    }
    const QSet<QString> balances = std::exchange(balanceRetry_, QSet<QString>());
    for (const QString& sh : balances) queueBalance(sh);
    idleReported_ = false;
}

// ------------------------------------------------------------- model updates ----

void UtxoScanner::applyStatus(const QString& scripthash, bool used, const QString& status)
{
    if (!model_) return;
    const int pos = model_->findByScripthash(network_, scripthash);
    if (pos < 0) return;
    AddressRow row = model_->at(pos);
    const int ci = findChain(row);
    const int index = static_cast<int>(row.index);
    // An unused address is checked with its status alone. A used one is
    // checked once its balance has answered as well: until its first balance
    // arrives (an address unknown or unused until now), its zero is no answer
    // and the row must not read as empty. One whose balance is known (used and
    // checked) keeps it, checked, until the balance queued below replaces it,
    // so a new transaction never blanks a row on screen meanwhile.
    const bool checked = !used || (row.used && row.checked);
    bool changed = row.checked != checked || row.used != used || row.status != status;
    row.checked = checked;
    row.used = used;
    row.status = status;
    if (!used && (row.confirmed != 0 || row.unconfirmed != 0)) {
        // No history means no coins: a balance must not outlive a reorg.
        row.confirmed = 0;
        row.unconfirmed = 0;
        changed = true;
    }
    const quint64 gen = generation_;
    if (changed) model_->upsert(row);
    if (gen != generation_ || !running_) return;
    recount(scripthash);
    if (!used) return;
    // Every time, the first answer of a run too, with the status the same as the saved one:
    // the server that gave the saved balance may be why another one is being asked.
    queueBalance(scripthash);
    if (ci >= 0) noteUsed(ci, index);
}

void UtxoScanner::applyBalance(const QString& scripthash, qint64 confirmed, qint64 unconfirmed)
{
    if (!model_) return;
    const int pos = model_->findByScripthash(network_, scripthash);
    if (pos < 0) return;
    AddressRow row = model_->at(pos);
    // Coins imply history, even when no subscription has said so (one a
    // server refused). A zero balance proves nothing either way: such a row
    // keeps an unknown status instead of showing up as unused. Any balance,
    // zero included, completes the check of a row whose status says used.
    const bool coins = confirmed != 0 || unconfirmed != 0;
    const bool answered = coins || row.used;
    const bool becameUsed = coins && !row.used;
    const bool becameChecked = answered && !row.checked;
    const int ci = findChain(row);
    const int index = static_cast<int>(row.index);
    // A row that says it was used moves the window here too: this may be all a server that
    // will not read it ever says of it.
    if (!becameUsed && !becameChecked && row.confirmed == confirmed && row.unconfirmed == unconfirmed) {
        if (row.used && ci >= 0) noteUsed(ci, index);
        return;
    }
    row.checked = row.checked || answered;
    row.used = row.used || coins;
    row.confirmed = confirmed;
    row.unconfirmed = unconfirmed;
    const bool used = row.used;
    const quint64 gen = generation_;
    model_->upsert(row);
    if (gen != generation_ || !running_) return;
    recount(scripthash);
    if (used && ci >= 0) noteUsed(ci, index);
    if (gen == generation_) reconsider(scripthash);
}

void UtxoScanner::noteUsed(int chainIndex, int index)
{
    Chain& c = chains_[chainIndex];
    if (index <= c.lastUsed || index >= kMaxChainRows) return;
    const int oldLast = std::max(c.lastUsed, c.savedUsed);
    c.lastUsed = index;
    noteTiered();
    // The next addresses move on with it: those it passed first, for the places they leave.
    if (tiered()) {
        reconsiderRange(chainIndex, oldLast + 1, oldLast + hotAhead(c));
        reconsiderRange(chainIndex, oldLast + hotAhead(c) + 1, std::max(c.lastUsed, c.savedUsed) + hotAhead(c));
    }
    // The window always ends `gap` past the last used address, also after the
    // initial scan: a payment to any subscribed address can reveal more.
    // deriveSlice() derives what the row limit allows and reports the rest.
    if (c.next < c.window()) scheduleDerive();
}

void UtxoScanner::reportLimits()
{
    // Once per start(), when a window reaches past what this scan may follow.
    // Everything up to there is followed as usual; the scan then stays
    // unfinished (busy()), so no total is presented as complete. Emitted past
    // report()'s cap: it is what explains a scan that never ends.
    if (limitReported_ || !limitReached()) return;
    limitReported_ = true;
    static const QLocale english(QLocale::English, QLocale::UnitedStates); // "15,000", as the UI groups
    const QString count = english.toString(qlonglong(tracked_.size()));
    // The gaps alone ask for more than one connection follows, whatever a server
    // says: many wallets, or many keys at large gaps. Another server would stop at
    // the same place, and only smaller gaps or fewer wallets help.
    qint64 baseRows = 0;
    for (const Chain& c : chains_) baseRows += c.base();
    if (baseRows >= kMaxWalletRows) {
        emit problem(Problem::Limit,
                     QStringLiteral("%1 scan stopped at %2 addresses, the most GitWallet watches, shared by all "
                                    "wallets. Lower the gaps in Settings or remove wallets you do not need here.")
                         .arg(info().name, count),
                     QString());
        return;
    }
    const bool severalWallets = std::any_of(chains_.cbegin(), chains_.cend(), [this](const Chain& c) {
        return c.wallet != chains_.constFirst().wallet;
    });
    // At the ceiling, several wallets' real use adds up. Whether smaller gaps leave room
    // depends on how much of it is real use, and they lower the limit once they are small.
    if (severalWallets && rowLimit() >= kMaxWalletRows) {
        emit problem(Problem::Limit,
                     QStringLiteral("%1 scan stopped at %2 addresses, the most GitWallet watches, shared by all "
                                    "wallets. Remove wallets you do not need here; lower gaps help only if the gaps, "
                                    "not used addresses, fill the limit. If these wallets do not use that many, "
                                    "choose another server.")
                         .arg(info().name, count),
                     QString());
        return;
    }
    emit problem(Problem::Limit,
                 rowLimit() < kMaxWalletRows
                     ? QStringLiteral("%1 scan stopped at %2 addresses, the limit for these gap settings: the "
                                      "Electrum server reports addresses that far out as used. If %3 that many, "
                                      "raise the gaps in Settings; otherwise choose another server.")
                           .arg(info().name, count,
                                severalWallets ? QStringLiteral("these wallets really use")
                                               : QStringLiteral("this wallet really uses"))
                     : QStringLiteral("%1 scan stopped at %2 addresses, the most GitWallet watches: the "
                                      "Electrum server reports addresses that far out as used. If that is wrong, "
                                      "choose another server in Settings.")
                           .arg(info().name, count),
                 QString());
}

void UtxoScanner::markConfirmed(const QString& scripthash)
{
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end() || it->confirmed) return;
    it->confirmed = true;
    recount(scripthash);
}

void UtxoScanner::recount(const QString& scripthash)
{
    // checked_ counts what the rows say, for this run: a row counts once this
    // run has its status and the row is checked (applyStatus() and
    // applyBalance() leave a used address unchecked until its first balance).
    // It can go back down: a new transaction on an unused address.
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end() || !model_) return;
    const int pos = model_->findByScripthash(network_, scripthash);
    const bool counted = it->confirmed && pos >= 0 && (model_->at(pos).checked || it->unanswered);
    if (counted == it->counted) return;
    it->counted = counted;
    checked_ += counted ? 1 : -1;
    noteProgress();
}

int UtxoScanner::findChain(const AddressRow& row) const
{
    return chainByKey_.value(ChainKey{row.wallet, row.account, quint8(row.type), quint8(row.change)}, -1);
}

// ------------------------------------------------------------------ progress ----

bool UtxoScanner::busy() const
{
    if (!running_) return false;
    // checked_ leaves out a used address until its first balance has answered,
    // so the initial scan does not finish while one is on its way or waiting
    // for its retry.
    if (deriveScheduled_ || checked_ < tracked_.size()) return true;
    // A window not derived to its end: rows still to come, or a window the row
    // limit cuts short. Such a scan cannot complete, and must not look complete.
    for (const Chain& c : chains_) {
        if (c.next < c.window()) return true;
    }
    // A balance waiting for its retry counts too: until it arrives, the total
    // may be missing coins.
    return !subQueue_.isEmpty() || !readQueue_.isEmpty() || subSent_ > 0 || !retry_.isEmpty()
           || !balanceQueue_.isEmpty() || balanceSent_ > 0 || !balanceRetry_.isEmpty();
}

void UtxoScanner::checkIdle()
{
    if (!running_) return;
    // Everything it follows has answered, even if a limit keeps it from ending: what the
    // figures stand for moves on, and the listeners are to look.
    if (!answeredSaid_ && answered()) {
        answeredSaid_ = true;
        balancesAt_ = runStartedAt_;
        noteProgress();
    }
    // This connection has done all it was asked, and has been asked something.
    if (client_ && answeredSaid_ && answersHere_ > 0 && refusedHere_ == 0) client_->noteServerWorks();
    if (idleReported_ || busy()) return;
    idleReported_ = true;
    // Followed live, this is now. With rows read in rounds, it is when the last whole
    // round began, or for the first reads, when they began (answered() has said so).
    if (roundsCount_ == 0)
        balancesAt_ = QDateTime::currentDateTime();
    const quint64 gen = generation_;
    if (!finished_) {
        dropRowsPastWindows();
        if (gen != generation_) return;
    }
    emitProgress(); // the final count, even inside the throttle interval
    if (gen != generation_ || finished_) return;
    finished_ = true; // before emitting: a listener may restart the scan
    emit scanFinished();
}

void UtxoScanner::dropRowsPastWindows()
{
    // This run's answers have found its windows. What an earlier run, or the database,
    // holds past them is what no scan follows: a server may have called those addresses
    // used, and the rows would stay on screen, unchecked or with old flags, for good.
    // What holds coins is followed wherever it is, so it is not among them.
    if (model_)
        model_->dropRows([this](const AddressRow& row) {
            return row.network != network_ || findChain(row) < 0 || tracked_.contains(row.scripthash.toLower());
        });
    // From here on only this run's answers say which addresses come next.
    bool moved = false;
    for (Chain& c : chains_) {
        moved = moved || c.savedUsed > c.lastUsed;
        c.savedUsed = -1;
    }
    if (moved && tiered()) rebalanceLive();
}

void UtxoScanner::noteProgress()
{
    progressDirty_ = true;
    if (progressArmed_) return;
    const qint64 wait = kProgressIntervalMs - (nowMs() - lastProgressMs_);
    if (wait <= 0) {
        emitProgress();
        return;
    }
    progressArmed_ = true;
    QTimer::singleShot(std::chrono::milliseconds(wait), this, [this] {
        progressArmed_ = false;
        // Through noteProgress() again: the final emission of a finished
        // spell may have gone out meanwhile and restarted the interval.
        if (progressDirty_ && running_) noteProgress();
    });
}

void UtxoScanner::emitProgress()
{
    progressDirty_ = false;
    lastProgressMs_ = nowMs();
    emit progress(checkedCount(), derivedCount());
}

void UtxoScanner::report(Problem kind, const QString& message, const QString& serverText)
{
    // The server's wording goes into serverDetail only, cleaned up; `message`
    // is always one of GitWallet's own sentences.
    const QString detail = serverDetail(serverText);
    const QString key = message + QLatin1Char('\n') + errorKey(detail);
    if (reportedErrors_.contains(key) || reportedErrors_.size() >= kMaxErrorsPerRun) return;
    reportedErrors_.insert(key);
    emit problem(kind, message, detail);
}

} // namespace wallet
