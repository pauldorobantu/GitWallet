#include "wallet/BtcScanner.h"

#include "net/ElectrumClient.h"
#include "wallet/WalletModel.h"

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
// fetch never waits behind thousands of subscriptions.
constexpr int kMaxOutstanding = 100;

// Addresses derived per event-loop turn. Each costs a CKDpub, the script
// hashes and a model insert; slicing keeps the window responsive when a large
// gap or a long used run derives thousands of rows at once.
constexpr int kDeriveSlice = 250;

constexpr qint64 kProgressIntervalMs = 250;

// The controller clamps gaps itself; this only guards another caller.
constexpr int kMaxGap = 100000;

// Addresses derived per chain at most. Every used status pushes the window
// `gap` rows further, and the model never drops a row, so a buggy or hostile
// server that calls every address used would otherwise grow the wallet until
// memory runs out. Far above what a shop seed needs (the controller's largest
// gap is 5,000), and well below 2^31, where non-hardened BIP32 indices end.
constexpr int kMaxChainRows = 100000;

// Failed requests (a refused subscription, a failed balance) go out again
// after this delay, doubling per round: a server busy for a moment is asked
// again soon, one that keeps refusing an address is not hammered.
constexpr int kRetryFirstMs = 5000;
constexpr int kRetryMaxMs = 5 * 60 * 1000;

// 21 million BTC in satoshis: a reply above it is garbage, not a balance.
constexpr qint64 kMaxSats = 2100000000000000LL;

// Distinct error texts emitted per start(). Past that a misbehaving server
// would only bury the user in toasts.
constexpr int kMaxErrorsPerRun = 8;

// The text ElectrumClient gives pending replies when it is stopped. It is a
// local event, not a server message, and it means the client dropped every
// subscription.
bool isStopped(const QString& error)
{
    return error == QLatin1String("stopped");
}

qint64 nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// blockchain.scripthash.subscribe result: null = no history, otherwise the
// status hash. 0 unused, 1 used, -1 neither (a malformed reply).
int parseStatus(const QJsonValue& v)
{
    if (v.isNull()) return 0;
    if (v.isString()) return v.toString().isEmpty() ? 0 : 1;
    return -1;
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

// Some servers quote the scripthash in their error text ("<hash> is not a valid
// script hash"); with it masked, one failure on 500 addresses is one message.
QString errorKey(const QString& message)
{
    static const QRegularExpression hex(QStringLiteral("[0-9A-Fa-f]{32,}"));
    QString key = message.trimmed();
    key.replace(hex, QStringLiteral("#"));
    return key;
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
    // Takes effect at the next start(). One chain per (type, account): a
    // repeated account would derive and subscribe every row twice.
    accounts_.clear();
    for (const AccountKey& a : accounts) {
        if (!chain::isBitcoin(a.type)) continue;
        const bool repeated = std::any_of(accounts_.cbegin(), accounts_.cend(), [&a](const AccountKey& b) {
            return b.type == a.type && b.account == a.account;
        });
        if (!repeated) accounts_.append(a);
    }
}

void BtcScanner::setGaps(int receiveGap, int changeGap)
{
    receiveGap_ = std::clamp(receiveGap, 1, kMaxGap);
    changeGap_ = std::clamp(changeGap, 1, kMaxGap);
    // A running scan follows at once. Rows already derived stay (the model
    // never drops rows), so a smaller gap only stops further growth.
    for (Chain& c : chains_) c.gap = c.change == 0 ? receiveGap_ : changeGap_;
    if (!running_) return;
    scheduleDerive();
    reportLimits();
}

void BtcScanner::start()
{
    ++generation_; // replies to anything sent before are dropped from now on
    running_ = true;
    chains_.clear();
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
    resubscribeAll_ = false;
    idleReported_ = false;
    finished_ = false;
    if (!client_ || !model_) {
        running_ = false;
        return;
    }

    // Reported once the state below is complete: a listener may react by
    // restarting the scan.
    QStringList problems;
    for (const AccountKey& acct : accounts_) {
        const QString where = QString::fromStdString(chain::accountRootPath(acct.type, acct.account));
        chain::HdPubKey accountNode;
        try {
            accountNode = acct.node();
        } catch (const std::exception& e) {
            problems.append(QStringLiteral("Bitcoin account %1 skipped: %2").arg(where, QString::fromUtf8(e.what())));
            continue;
        }
        for (quint32 change = 0; change <= 1; ++change) {
            Chain c;
            c.type = acct.type;
            c.account = acct.account;
            c.change = change;
            c.gap = change == 0 ? receiveGap_ : changeGap_;
            try {
                c.node = accountNode.deriveChild(change);
            } catch (const std::exception& e) {
                problems.append(QStringLiteral("Bitcoin chain %1/%2 skipped: %3")
                                    .arg(where, QString::number(change), QString::fromUtf8(e.what())));
                continue;
            }
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
    for (const QString& p : problems) {
        reportError(p);
        if (gen != generation_) return;
    }
    reportLimits(); // rows adopted from a run that already reached the limit
}

void BtcScanner::adoptModelRows()
{
    // Rows already in the model (an earlier start(), e.g. after a settings
    // change) are adopted instead of derived again: no duplicates, their last
    // known state stays on screen, and lastUsed is known before any reply.
    QList<int> probeIndex(chains_.size(), -1);
    QList<QString> probeHash(chains_.size());
    for (int r = 0; r < model_->size(); ++r) {
        const AddressRow& row = model_->at(r);
        if (!row.isBitcoin() || row.index >= quint32(kMaxChainRows)) continue; // never derived
        const int ci = findChain(row.type, row.account, row.change);
        if (ci < 0) continue;
        Chain& c = chains_[ci];
        const int index = static_cast<int>(row.index);
        c.floor = std::max(c.floor, index + 1);
        if (row.used) c.lastUsed = std::max(c.lastUsed, index);
        if (probeIndex[ci] < 0 || index < probeIndex[ci]) {
            probeIndex[ci] = index;
            probeHash[ci] = row.scripthash.toLower();
        }
    }

    // One derivation per chain proves the rows came from this xpub. Rows left
    // over from another wallet are derived again instead, with none of their
    // flags trusted, so no foreign address survives on screen.
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
        if (!c.reuse) c.lastUsed = -1;
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
    resubscribeAll_ = false;
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
    if (!running_) return;
    if (resubscribeAll_) {
        // The client was stopped and started again under a running scan,
        // which clears its subscriptions (a plain reconnect restores them
        // inside the client): every followed address is subscribed again.
        resubscribeAll_ = false;
        retry_.clear();
        for (auto it = tracked_.begin(); it != tracked_.end(); ++it) {
            if (it->sub == SubState::Live || it->sub == SubState::Retry) {
                it->sub = SubState::Queued;
                subQueue_.append(it.key());
            }
        }
    }
    // Refused subscriptions and failed balances get another try now, possibly
    // on another server, without waiting for the retry round.
    requeueFailed();
    schedulePump();
}

void BtcScanner::onScripthashChanged(const QString& scripthash, const QString& status)
{
    if (!running_ || !model_) return;
    const int pos = model_->findByScripthash(scripthash);
    if (pos < 0) return;
    const AddressRow& row = model_->at(pos);
    if (!row.isBitcoin() || findChain(row.type, row.account, row.change) < 0) return; // not ours
    const QString sh = row.scripthash.toLower(); // the spelling tracked_ uses

    // A subscribe reply already on its way may predate this status; one sent
    // later (or a row not followed yet in this run) is at least as new.
    const auto it = tracked_.find(sh);
    if (it != tracked_.end() && it->sub == SubState::Sent) it->notified = true;
    const quint64 gen = generation_;
    applyStatus(sh, !status.isEmpty());
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
    int budget = kDeriveSlice;
    for (int ci = 0; ci < chains_.size() && budget > 0; ++ci) {
        while (budget > 0 && chains_.at(ci).next < chains_.at(ci).target()) {
            const int index = chains_[ci].next++;
            budget -= adoptOrDerive(ci, index);
            // A model listener may have restarted or stopped the scan.
            if (gen != generation_ || !running_ || !model_) return;
        }
    }
    const bool more = std::any_of(chains_.cbegin(), chains_.cend(),
                                  [](const Chain& c) { return c.next < c.target(); });
    if (more) scheduleDerive();
    schedulePump();
    checkIdle();
}

int BtcScanner::adoptOrDerive(int chainIndex, int index)
{
    const Chain& c = chains_.at(chainIndex);
    const quint32 i = quint32(index);
    const int pos = model_->find(c.type, c.account, c.change, i);
    if (pos >= 0 && c.reuse) {
        const QString sh = model_->at(pos).scripthash.toLower();
        if (!sh.isEmpty()) {
            if (!tracked_.contains(sh)) track(sh);
            return 0;
        }
    }

    AddressRow row;
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
        // CKDpub fails for about one index in 2^127; BIP32 says to skip it.
        reportError(QStringLiteral("%1 skipped: %2")
                        .arg(QString::fromStdString(chain::accountPath(c.type, c.account, c.change, i)),
                             QString::fromUtf8(e.what())));
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
        if (self) self->onSubscribeReply(gen, scripthash, error.isEmpty() ? parseStatus(result) : -1, error);
    });
}

void BtcScanner::onSubscribeReply(quint64 generation, const QString& scripthash, int status,
                                  const QString& error)
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
        if (isStopped(error)) {
            resubscribeAll_ = true;
            return;
        }
        // get_balance does not need the subscription: coins on an address
        // this server refuses to follow (some refuse very long histories)
        // still reach the total, and mark it used, so the window grows past
        // it. Each round asks again, which also keeps that balance current.
        queueBalance(scripthash);
        armRetry();
        reportError(error.isEmpty() ? QStringLiteral("An Electrum server sent an invalid address status.")
                                    : error);
        return;
    }

    it->sub = SubState::Live;
    // A notification that arrived while this reply was on its way is newer.
    if (!it->notified) {
        applyStatus(scripthash, status == 1);
        if (generation != generation_) return;
    }
    markConfirmed(scripthash);
    checkIdle();
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
        } else if (isStopped(error)) {
            resubscribeAll_ = true; // fetched again after the re-subscription
        } else {
            // The old balance stays meanwhile. Nothing else would ask again: a
            // reconnect restores the subscription without a notification when
            // the status is unchanged, and a shop address that is paid once
            // never changes again. Until the retry gets through, the scan is
            // not finished, so no total is presented as complete.
            balanceRetry_.insert(scripthash);
            armRetry();
            reportError(error.isEmpty() ? QStringLiteral("An Electrum server sent an invalid balance.") : error);
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
    const int ci = findChain(row.type, row.account, row.change);
    const int index = static_cast<int>(row.index);
    bool changed = !row.checked || row.used != used;
    row.checked = true;
    row.used = used;
    if (!used && (row.btcConfirmed != 0 || row.btcUnconfirmed != 0)) {
        // No history means no coins: a balance must not outlive a reorg.
        row.btcConfirmed = 0;
        row.btcUnconfirmed = 0;
        changed = true;
    }
    const quint64 gen = generation_;
    if (changed) model_->upsert(row);
    if (!used || gen != generation_ || !running_) return;
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
    // keeps an unknown status instead of showing up as unused.
    const bool coins = confirmed != 0 || unconfirmed != 0;
    const bool becameUsed = coins && !row.used;
    const bool becameChecked = coins && !row.checked;
    if (!becameUsed && !becameChecked && row.btcConfirmed == confirmed && row.btcUnconfirmed == unconfirmed)
        return;
    row.checked = row.checked || coins;
    row.used = row.used || coins;
    row.btcConfirmed = confirmed;
    row.btcUnconfirmed = unconfirmed;
    const int ci = findChain(row.type, row.account, row.change);
    const int index = static_cast<int>(row.index);
    const quint64 gen = generation_;
    model_->upsert(row);
    if (becameUsed && ci >= 0 && gen == generation_ && running_) noteUsed(ci, index);
}

void BtcScanner::noteUsed(int chainIndex, int index)
{
    Chain& c = chains_[chainIndex];
    if (index <= c.lastUsed || index >= kMaxChainRows) return;
    c.lastUsed = index;
    // The window always ends `gap` past the last used address, also after the
    // initial scan: a payment to any subscribed address can reveal more.
    if (c.next < c.target()) scheduleDerive();
    reportLimits(); // last: an error() listener may restart the scan
}

void BtcScanner::reportLimits()
{
    // Once per chain and start(): the window wants rows past kMaxChainRows.
    // The scan still completes up to the limit, so the rows shown stay right.
    static const QLocale english(QLocale::English, QLocale::UnitedStates); // "100,000", as the UI groups
    const quint64 gen = generation_;
    for (int ci = 0; ci < chains_.size(); ++ci) {
        Chain& c = chains_[ci];
        if (c.limited || c.window() <= kMaxChainRows) continue;
        c.limited = true;
        const QString where = QString::fromStdString(chain::accountRootPath(c.type, c.account))
                              + QLatin1Char('/') + QString::number(c.change);
        reportError(QStringLiteral("Stopped at %1 addresses on %2, the limit per chain: the Electrum server "
                                   "reports addresses that far out as used.")
                        .arg(english.toString(kMaxChainRows), where));
        if (gen != generation_) return; // a listener restarted or stopped the scan
    }
}

void BtcScanner::markConfirmed(const QString& scripthash)
{
    const auto it = tracked_.find(scripthash);
    if (it == tracked_.end() || it->confirmed) return;
    it->confirmed = true;
    ++checked_;
    noteProgress();
}

int BtcScanner::findChain(chain::ScriptType type, quint32 account, quint32 change) const
{
    for (int i = 0; i < chains_.size(); ++i) {
        const Chain& c = chains_.at(i);
        if (c.type == type && c.account == account && c.change == change) return i;
    }
    return -1;
}

// ------------------------------------------------------------------ progress ----

bool BtcScanner::busy() const
{
    if (!running_) return false;
    if (deriveScheduled_ || checked_ < tracked_.size()) return true;
    for (const Chain& c : chains_) {
        if (c.next < c.target()) return true;
    }
    // A balance waiting for its retry counts too: until it arrives, the total
    // may be missing coins.
    return !subQueue_.isEmpty() || subSent_ > 0 || !retry_.isEmpty() || resubscribeAll_
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

void BtcScanner::reportError(const QString& message)
{
    const QString key = errorKey(message);
    if (key.isEmpty() || reportedErrors_.contains(key) || reportedErrors_.size() >= kMaxErrorsPerRun) return;
    reportedErrors_.insert(key);
    emit error(message.trimmed());
}

} // namespace wallet
