#include "wallet/BtcScanner.h"

#include "net/ElectrumClient.h"
#include "wallet/WalletModel.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <utility>

namespace wallet {

namespace {

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

constexpr qint64 kProgressIntervalMs = 250;

// The controller clamps gaps itself; this only guards another caller.
constexpr int kMaxGap = 100000;

// Bitcoin rows one scan follows at most, all chains together: kRowsPerGapRow
// times the sum of the chains' gaps, within [kMinWalletRows, kMaxWalletRows].
// Every used status pushes a window `gap` rows further and the model never
// drops a row, so a buggy or hostile server that calls every address used
// would otherwise grow the wallet, its subscriptions and its balance requests
// without end (a limit per chain still let six chains reach six times as
// much). Each chain's own gap window is derived first, inside the limit; for one
// wallet the rest leaves room for close to 10,000 used addresses or more at any
// gap the controller allows (20 to 5,000). The ceiling stays below what public
// servers let one client subscribe (ElectrumX: 50,000 per session by default;
// Fulcrum: 75,000 per IP), so an honest server never refuses part of a full
// wallet; all wallets share it, since they share one connection.
constexpr qint64 kRowsPerGapRow = 20;
constexpr int kMinWalletRows = 10000;
constexpr int kMaxWalletRows = BtcScanner::kMaxFollowedRows;

// The end of a chain's indices. Only a gap window larger than the row limit (a
// caller other than the controller) gets near it; well below 2^31, where
// non-hardened BIP32 indices end.
constexpr int kMaxChainRows = 100000;

// Failed requests (a refused subscription, a failed balance) go out again
// after this delay, doubling per round: a server busy for a moment is asked
// again soon, one that keeps refusing an address is not hammered.
constexpr int kRetryFirstMs = 5000;
constexpr int kRetryMaxMs = 5 * 60 * 1000;

// 21 million BTC in satoshis: a reply above it is garbage, not a balance.
constexpr qint64 kMaxSats = 2100000000000000LL;

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
bool parseBalance(const QJsonValue& v, qint64* confirmed, qint64* unconfirmed)
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

QString chainKey(quint32 wallet, chain::ScriptType type, quint32 account, quint32 change)
{
    return QStringLiteral("%1/%2/%3/%4").arg(wallet).arg(static_cast<int>(type)).arg(account).arg(change);
}

} // namespace

int BtcScanner::Chain::window() const
{
    // lastUsed and floor never pass kMaxChainRows and gap is at most kMaxGap,
    // so this never comes near overflowing an int.
    return std::max(floor, lastUsed + 1 + gap);
}

int BtcScanner::Chain::target() const
{
    return std::min(window(), kMaxChainRows);
}

int BtcScanner::Chain::base() const
{
    // The window as it stands with nothing used: what the gap alone asks for,
    // plus the rows of another wallet to replace.
    return std::min(std::max(floor, gap), kMaxChainRows);
}

BtcScanner::BtcScanner(net::ElectrumClient* client, WalletModel* model, QObject* parent)
    : QObject(parent), client_(client), model_(model)
{
    if (client_) {
        connect(client_, &net::ElectrumClient::ready, this, &BtcScanner::onClientReady);
        connect(client_, &net::ElectrumClient::scripthashChanged, this, &BtcScanner::onScripthashChanged);
        // The owner destroys the client first; a posted pump must then find
        // nothing to call instead of a dangling pointer.
        connect(client_, &QObject::destroyed, this, [this] { client_ = nullptr; });
    }
    if (model_) {
        connect(model_, &WalletModel::cleared, this, &BtcScanner::onModelCleared);
        connect(model_, &QObject::destroyed, this, [this] { model_ = nullptr; });
    }
}

void BtcScanner::setAccounts(const QList<AccountKey>& accounts)
{
    // Takes effect at the next start(). One chain per (wallet, type, account): a
    // repeated account would derive and subscribe every row twice.
    accounts_.clear();
    for (const AccountKey& a : accounts) {
        if (!chain::isBitcoin(a.type)) continue;
        const bool repeated = std::any_of(accounts_.cbegin(), accounts_.cend(), [&a](const AccountKey& b) {
            return b.wallet == a.wallet && b.type == a.type && b.account == a.account;
        });
        if (!repeated) accounts_.append(a);
    }
}

void BtcScanner::setGaps(int receiveGap, int changeGap)
{
    receiveGap_ = std::clamp(receiveGap, 1, kMaxGap);
    changeGap_ = std::clamp(changeGap, 1, kMaxGap);
    // A running scan follows at once. Rows already derived stay (the model
    // never drops rows), so a smaller gap only stops further growth. The row
    // limit moves with the gaps; deriveSlice() reports a window it cuts short.
    for (Chain& c : chains_) c.gap = c.change == 0 ? receiveGap_ : changeGap_;
    if (!running_) return;
    scheduleDerive();
}

void BtcScanner::start()
{
    ++generation_; // replies to anything sent before are dropped from now on
    running_ = true;
    chains_.clear();
    chainByKey_.clear();
    tracked_.clear();
    subQueue_.clear();
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
        const QString where = QString::fromStdString(chain::accountRootPath(acct.type, acct.account));
        chain::HdPubKey accountNode;
        try {
            accountNode = acct.node();
        } catch (const std::exception& e) {
            skipped.append(QStringLiteral("Bitcoin account %1 skipped: %2").arg(where, QString::fromUtf8(e.what())));
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
                skipped.append(QStringLiteral("Bitcoin chain %1/%2 skipped: %3")
                                   .arg(where, QString::number(change), QString::fromUtf8(e.what())));
                continue;
            }
            chainByKey_.insert(chainKey(c.wallet, c.type, c.account, c.change), int(chains_.size()));
            chains_.append(c);
        }
    }
    adoptModelRows();

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
}

void BtcScanner::adoptModelRows()
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
        if (!row.isBitcoin() || row.index >= quint32(kMaxChainRows)) continue; // never derived
        const int ci = findChain(row);
        if (ci < 0) continue;
        const int index = static_cast<int>(row.index);
        const QString sh = row.scripthash.toLower();
        rowsEnd[ci] = std::max(rowsEnd[ci], index + 1);
        if ((row.btcConfirmed != 0 || row.btcUnconfirmed != 0) && !sh.isEmpty()) funded[ci].append(sh);
        if (probeIndex[ci] < 0 || index < probeIndex[ci]) {
            probeIndex[ci] = index;
            probeHash[ci] = sh;
        }
    }

    // One derivation per chain proves the rows came from this xpub. Rows left
    // over from another wallet are all derived again instead (floor), with none
    // of their flags trusted, so no foreign address survives on screen.
    for (int ci = 0; ci < chains_.size(); ++ci) {
        if (probeIndex[ci] < 0) continue;
        Chain& c = chains_[ci];
        try {
            const chain::AddressInfo info = chain::addressFromChainNode(
                c.node, c.type, c.account, c.change, quint32(probeIndex[ci]));
            c.reuse = QString::fromStdString(info.electrumScripthash) == probeHash[ci];
        } catch (const std::exception&) {
            c.reuse = false;
        }
        if (!c.reuse) {
            c.floor = rowsEnd[ci];
            continue;
        }
        // Not through track(): no progress() while start() is half done.
        for (const QString& sh : std::as_const(funded[ci])) {
            if (tracked_.contains(sh)) continue;
            tracked_.insert(sh, Tracked());
            subQueue_.append(sh);
        }
    }
}

void BtcScanner::stop()
{
    running_ = false;
    ++generation_; // replies still on their way are dropped
    subQueue_.clear();
    balanceQueue_.clear();
    balanceQueued_.clear();
    balanceSeq_.clear();
    retry_.clear();
    balanceRetry_.clear();
    retryArmed_ = false; // the posted round sees the old generation and does nothing
    subSent_ = 0;
    balanceSent_ = 0;
    // tracked_ and checked_ stay, so checkedCount() / derivedCount() describe
    // the last run until the next start(). Requests already handed to the
    // client keep counting in outstanding_ until their replies arrive.
}

bool BtcScanner::isScanning() const
{
    return busy();
}

bool BtcScanner::limitReached() const
{
    // A window that wants rows this scan may not derive: the row limit (or, for
    // a gap larger than the limit, the end of a chain's indices) cuts it short.
    // While it does, the scan never finishes (busy()), and the addresses past
    // the cut go unchecked.
    if (!running_) return false;
    return std::any_of(chains_.cbegin(), chains_.cend(),
                       [this](const Chain& c) { return c.next < c.window() && !mayDerive(c); });
}

int BtcScanner::checkedCount() const
{
    return checked_;
}

int BtcScanner::derivedCount() const
{
    return static_cast<int>(tracked_.size());
}

// ------------------------------------------------------------ client events ----

void BtcScanner::onClientReady()
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
    // like any other, a status that moved meanwhile is applied. Subscriptions
    // still on their way (Sent) are re-sent by the client itself.
    retry_.clear();
    QStringList again;
    for (auto it = tracked_.begin(); it != tracked_.end(); ++it) {
        if (it->sub == SubState::Live || it->sub == SubState::Retry) {
            it->sub = SubState::Queued;
            again.append(it.key());
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

void BtcScanner::onScripthashChanged(const QString& scripthash, const QString& status)
{
    if (!running_ || !model_) return;
    const int pos = model_->findByScripthash(scripthash);
    if (pos < 0) return;
    const AddressRow& row = model_->at(pos);
    if (!row.isBitcoin() || findChain(row) < 0) return; // not ours
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
    applyStatus(sh, parsed == 1);
    if (gen != generation_) return;
    // The status changes with every new transaction, confirmation or reorg,
    // so the balance is fetched again even when `used` stayed the same.
    queueBalance(sh);
    markConfirmed(sh);
    schedulePump();
}

void BtcScanner::onModelCleared()
{
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

void BtcScanner::scheduleDerive()
{
    idleReported_ = false;
    if (deriveScheduled_) return;
    deriveScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        deriveScheduled_ = false;
        deriveSlice();
    });
}

void BtcScanner::deriveSlice()
{
    if (!running_ || !model_) return;
    const quint64 gen = generation_;
    int slice = kDeriveSlice;
    for (int ci = 0; ci < chains_.size() && slice > 0; ++ci) {
        while (slice > 0 && mayDerive(chains_.at(ci))) {
            const int index = chains_[ci].next++;
            slice -= adoptOrDerive(ci, index);
            // A model listener may have restarted or stopped the scan.
            if (gen != generation_ || !running_ || !model_) return;
        }
    }
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

int BtcScanner::adoptOrDerive(int chainIndex, int index)
{
    const Chain& c = chains_.at(chainIndex);
    const quint32 i = quint32(index);
    const int pos = model_->find(c.wallet, c.type, c.account, c.change, i);
    if (pos >= 0 && c.reuse) {
        const QString sh = model_->at(pos).scripthash.toLower();
        if (!sh.isEmpty()) {
            if (!tracked_.contains(sh)) track(sh);
            return 0;
        }
    }

    AddressRow row;
    row.wallet = c.wallet;
    row.type = c.type;
    row.account = c.account;
    row.change = c.change;
    row.index = i;
    try {
        const chain::AddressInfo info = chain::addressFromChainNode(c.node, c.type, c.account, c.change, i);
        row.path = QString::fromStdString(info.path);
        row.address = QString::fromStdString(info.address);
        row.scripthash = QString::fromStdString(info.electrumScripthash);
    } catch (const std::exception& e) {
        // CKDpub fails for about one index in 2^127; BIP32 says to skip it, so
        // no address is missing and there is nothing to report as a problem.
        qInfo().noquote() << QStringLiteral("%1 skipped: %2")
                                 .arg(QString::fromStdString(chain::accountPath(c.type, c.account, c.change, i)),
                                      QString::fromUtf8(e.what()));
        return 1;
    }
    const QString sh = row.scripthash;
    const quint64 gen = generation_;
    model_->upsert(row); // also replaces a foreign row at this position
    if (gen == generation_ && running_ && !tracked_.contains(sh)) track(sh);
    return 1;
}

void BtcScanner::track(const QString& scripthash)
{
    tracked_.insert(scripthash, Tracked());
    subQueue_.append(scripthash);
    idleReported_ = false;
    noteProgress();
}

int BtcScanner::rowLimitFor(qint64 gaps)
{
    return static_cast<int>(std::clamp(gaps * kRowsPerGapRow, qint64(kMinWalletRows), qint64(kMaxWalletRows)));
}

int BtcScanner::rowLimit() const
{
    qint64 gaps = 0;
    for (const Chain& c : chains_) gaps += c.gap;
    return rowLimitFor(gaps);
}

qint64 BtcScanner::reservedRows() const
{
    // Kept free inside the limit: a chain that grows first must not crowd
    // another out of the addresses its gap alone asks for.
    qint64 rows = 0;
    for (const Chain& c : chains_) rows += std::max(0, c.base() - c.next);
    return rows;
}

bool BtcScanner::mayDerive(const Chain& c) const
{
    if (c.next >= c.target()) return false;
    // What the gap alone asks for, unless so many wallets add up past the ceiling
    // that a server would refuse the rest anyway.
    if (c.next < c.base()) return tracked_.size() < kMaxWalletRows;
    // Growth a used status asked for: only within the row limit.
    return qint64(tracked_.size()) + reservedRows() < rowLimit();
}

// ----------------------------------------------------------------- requests ----

void BtcScanner::schedulePump()
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

void BtcScanner::pumpRequests()
{
    while (running_ && client_ && client_->isReady() && outstanding_ < kMaxOutstanding) {
        // Balances first: they are few (used addresses only) and they are
        // what the totals wait for.
        if (!balanceQueue_.isEmpty())
            sendBalance(balanceQueue_.takeFirst());
        else if (!subQueue_.isEmpty())
            sendSubscribe(subQueue_.takeFirst());
        else
            break;
    }
    checkIdle();
}

void BtcScanner::sendSubscribe(const QString& scripthash)
{
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end() || it->sub != SubState::Queued) return;
    it->sub = SubState::Sent;
    it->notified = false;
    ++subSent_;
    ++outstanding_;
    const QPointer<BtcScanner> self(this);
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

void BtcScanner::onSubscribeReply(quint64 generation, const QString& scripthash, int status,
                                  const QString& statusText, const QString& error)
{
    if (outstanding_ > 0) --outstanding_;
    schedulePump();
    if (generation != generation_ || !running_) return; // a reply to an earlier start()
    if (subSent_ > 0) --subSent_;
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end()) return;

    if (!error.isEmpty() || status < 0) {
        // Not checked: subscribed again after the next ready() (a reconnect,
        // possibly to another server) or the next retry round, whichever
        // comes first. A refusal is no reason for the client to reconnect, so
        // on a stable connection only the round brings it back.
        it->sub = SubState::Retry;
        retry_.append(scripthash);
        if (droppedByClient(error)) {
            // Nobody refused it: the client was stopped under the scan. The
            // round armed here asks again should no ready() come first.
            armRetry();
            return;
        }
        // get_balance does not need the subscription: coins on an address
        // this server refuses to follow (some refuse very long histories)
        // still reach the total, and mark it used, so the window grows past
        // it. Each round asks again, which also keeps that balance current.
        queueBalance(scripthash);
        armRetry();
        report(Problem::Failure,
               error.isEmpty() ? QStringLiteral("An Electrum server sent an invalid address status.")
                               : QStringLiteral("An Electrum server refused to follow an address."),
               error);
        return;
    }

    it->sub = SubState::Live;
    // A notification that arrived while this reply was on its way is newer.
    // A subscription made again after a reconnect that finds the status it
    // knew changes nothing: the balance that status implies is known, or on
    // its way (a pending request is sent again, a failed one retried).
    if (!it->notified && !(it->confirmed && it->status == statusText)) {
        it->status = statusText;
        applyStatus(scripthash, status == 1);
        if (generation != generation_) return;
    }
    markConfirmed(scripthash);
    checkIdle();
}

bool BtcScanner::droppedByClient(const QString& error) const
{
    // ElectrumClient::stop() fails every pending request with "stopped", after
    // it stopped being ready. A server can send that very text as well, but a
    // server's reply only ever arrives while the client is ready: the state
    // tells the two apart where the text cannot.
    return error == QLatin1String("stopped") && (!client_ || !client_->isReady());
}

void BtcScanner::queueBalance(const QString& scripthash)
{
    idleReported_ = false;
    if (balanceQueued_.contains(scripthash)) return;
    balanceQueued_.insert(scripthash);
    balanceQueue_.append(scripthash);
}

void BtcScanner::sendBalance(const QString& scripthash)
{
    balanceQueued_.remove(scripthash);
    const quint64 seq = ++nextBalanceSeq_;
    balanceSeq_.insert(scripthash, seq);
    ++balanceSent_;
    ++outstanding_;
    const QPointer<BtcScanner> self(this);
    const quint64 gen = generation_;
    client_->request(QStringLiteral("blockchain.scripthash.get_balance"), QJsonArray{scripthash},
                     [self, gen, seq, scripthash](const QJsonValue& result, const QString& error) {
        if (!self) return;
        qint64 confirmed = 0;
        qint64 unconfirmed = 0;
        const bool ok = error.isEmpty() && parseBalance(result, &confirmed, &unconfirmed);
        self->onBalanceReply(gen, seq, scripthash, ok, confirmed, unconfirmed, error);
    });
}

void BtcScanner::onBalanceReply(quint64 generation, quint64 seq, const QString& scripthash, bool ok,
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
            applyBalance(scripthash, confirmed, unconfirmed);
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
            if (!droppedByClient(error))
                report(Problem::Failure,
                       error.isEmpty() ? QStringLiteral("An Electrum server sent an invalid balance.")
                                       : QStringLiteral("An Electrum server refused a balance request."),
                       error);
        }
        if (generation != generation_) return;
    }
    checkIdle();
}

void BtcScanner::armRetry()
{
    idleReported_ = false;
    if (retryArmed_ || !running_) return;
    // After ten quiet minutes, whatever failed before evidently got through:
    // a new failure starts the backoff over.
    if (retryDelayMs_ <= 0 || nowMs() - lastRetryMs_ > 2 * qint64(kRetryMaxMs)) retryDelayMs_ = kRetryFirstMs;
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

void BtcScanner::requeueFailed()
{
    const QStringList again = std::exchange(retry_, QStringList());
    for (const QString& sh : again) {
        const auto it = tracked_.find(sh);
        if (it != tracked_.end() && it->sub == SubState::Retry) {
            it->sub = SubState::Queued;
            subQueue_.append(sh);
        }
    }
    const QSet<QString> balances = std::exchange(balanceRetry_, QSet<QString>());
    for (const QString& sh : balances) queueBalance(sh);
    idleReported_ = false;
}

// ------------------------------------------------------------- model updates ----

void BtcScanner::applyStatus(const QString& scripthash, bool used)
{
    if (!model_) return;
    const int pos = model_->findByScripthash(scripthash);
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
    bool changed = row.checked != checked || row.used != used;
    row.checked = checked;
    row.used = used;
    if (!used && (row.btcConfirmed != 0 || row.btcUnconfirmed != 0)) {
        // No history means no coins: a balance must not outlive a reorg.
        row.btcConfirmed = 0;
        row.btcUnconfirmed = 0;
        changed = true;
    }
    const quint64 gen = generation_;
    if (changed) model_->upsert(row);
    if (gen != generation_ || !running_) return;
    recount(scripthash);
    if (!used) return;
    queueBalance(scripthash);
    if (ci >= 0) noteUsed(ci, index);
}

void BtcScanner::applyBalance(const QString& scripthash, qint64 confirmed, qint64 unconfirmed)
{
    if (!model_) return;
    const int pos = model_->findByScripthash(scripthash);
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
    if (!becameUsed && !becameChecked && row.btcConfirmed == confirmed && row.btcUnconfirmed == unconfirmed)
        return;
    row.checked = row.checked || answered;
    row.used = row.used || coins;
    row.btcConfirmed = confirmed;
    row.btcUnconfirmed = unconfirmed;
    const int ci = findChain(row);
    const int index = static_cast<int>(row.index);
    const quint64 gen = generation_;
    model_->upsert(row);
    if (gen != generation_ || !running_) return;
    recount(scripthash);
    if (becameUsed && ci >= 0) noteUsed(ci, index);
}

void BtcScanner::noteUsed(int chainIndex, int index)
{
    Chain& c = chains_[chainIndex];
    if (index <= c.lastUsed || index >= kMaxChainRows) return;
    c.lastUsed = index;
    // The window always ends `gap` past the last used address, also after the
    // initial scan: a payment to any subscribed address can reveal more.
    // deriveSlice() derives what the row limit allows and reports the rest.
    if (c.next < c.window()) scheduleDerive();
}

void BtcScanner::reportLimits()
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
                     QStringLiteral("Bitcoin scan stopped at %1 addresses, the most GitWallet follows on one "
                                    "connection, shared by all wallets. Lower the gaps in Settings or remove "
                                    "wallets you do not need here.")
                         .arg(count),
                     QString());
        return;
    }
    const bool severalWallets = std::any_of(chains_.cbegin(), chains_.cend(), [this](const Chain& c) {
        return c.wallet != chains_.constFirst().wallet;
    });
    // At the ceiling, several wallets' real use adds up: smaller gaps leave them room,
    // as the limit does not fall until the gaps are small. Below it they would not.
    if (severalWallets && rowLimit() >= kMaxWalletRows) {
        emit problem(Problem::Limit,
                     QStringLiteral("Bitcoin scan stopped at %1 addresses, the most GitWallet follows on one "
                                    "connection, shared by all wallets. Lower the gaps in Settings or remove "
                                    "wallets you do not need here. If these wallets do not use that many, choose "
                                    "another server.")
                         .arg(count),
                     QString());
        return;
    }
    emit problem(Problem::Limit,
                 rowLimit() < kMaxWalletRows
                     ? QStringLiteral("Bitcoin scan stopped at %1 addresses, the limit for these gap settings: the "
                                      "Electrum server reports addresses that far out as used. If %2 that many, "
                                      "raise the gaps in Settings; otherwise choose another server.")
                           .arg(count, severalWallets ? QStringLiteral("these wallets really use")
                                                      : QStringLiteral("this wallet really uses"))
                     : QStringLiteral("Bitcoin scan stopped at %1 addresses, the most GitWallet follows: the "
                                      "Electrum server reports addresses that far out as used. If that is wrong, "
                                      "choose another server in Settings.")
                           .arg(count),
                 QString());
}

void BtcScanner::markConfirmed(const QString& scripthash)
{
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end() || it->confirmed) return;
    it->confirmed = true;
    recount(scripthash);
}

void BtcScanner::recount(const QString& scripthash)
{
    // checked_ counts what the rows say, for this run: a row counts once this
    // run has its status and the row is checked (applyStatus() and
    // applyBalance() leave a used address unchecked until its first balance).
    // It can go back down: a new transaction on an unused address.
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end() || !model_) return;
    const int pos = model_->findByScripthash(scripthash);
    const bool counted = it->confirmed && pos >= 0 && model_->at(pos).checked;
    if (counted == it->counted) return;
    it->counted = counted;
    checked_ += counted ? 1 : -1;
    noteProgress();
}

int BtcScanner::findChain(const AddressRow& row) const
{
    return chainByKey_.value(chainKey(row.wallet, row.type, row.account, row.change), -1);
}

// ------------------------------------------------------------------ progress ----

bool BtcScanner::busy() const
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
    return !subQueue_.isEmpty() || subSent_ > 0 || !retry_.isEmpty()
           || !balanceQueue_.isEmpty() || balanceSent_ > 0 || !balanceRetry_.isEmpty();
}

void BtcScanner::checkIdle()
{
    if (!running_ || idleReported_ || busy()) return;
    idleReported_ = true;
    const quint64 gen = generation_;
    emitProgress(); // the final count, even inside the throttle interval
    if (gen != generation_ || finished_) return;
    finished_ = true; // before emitting: a listener may restart the scan
    emit scanFinished();
}

void BtcScanner::noteProgress()
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

void BtcScanner::emitProgress()
{
    progressDirty_ = false;
    lastProgressMs_ = nowMs();
    emit progress(checkedCount(), derivedCount());
}

void BtcScanner::report(Problem kind, const QString& message, const QString& serverText)
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
