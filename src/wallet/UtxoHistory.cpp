#include "wallet/UtxoHistory.h"

#include "chain/Transaction.h"
#include "net/ElectrumClient.h"
#include "net/PriceHistory.h"
#include "wallet/HeaderChain.h"
#include "wallet/UtxoScanner.h"
#include "wallet/WalletModel.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <utility>

namespace wallet {

namespace {

Q_STATIC_LOGGING_CATEGORY(lcHistory, "gitwallet.history")

// In flight at once. Less than the connection takes: the balances go first.
constexpr int kMaxInFlight = 24;
// Calls a tick. 120 a second alone is what public servers took without complaint; a scanner
// reading rounds takes 200 a second of the same connection, and 250 is where the one that first
// stopped did (docs/ENDPOINTS.md), so beside it this is what is left.
constexpr int kCallsPerTick = 12;
constexpr int kCallsPerTickBesideRounds = 4;
constexpr int kTickMs = 100;
// One call in so many is a proof, whatever else waits: without one a line has no date, and a
// server's lie about a block comes out early.
constexpr int kProofEvery = 4;
// No more raw transactions waiting than this before the next address is read.
constexpr qsizetype kMaxTxBacklog = 2000;
constexpr int kRefusalsBeforePause = 8;
constexpr int kFirstPauseMs = 15'000;
constexpr int kMaxPauseMs = 10 * 60'000;
// A balance comes a moment after the status that moved it.
constexpr int kCompareAfterMs = 5000;
constexpr int kComparePerTurn = 2000;
constexpr int kProgressMs = 250;
constexpr int kFirstPriceWaitMs = 2000;
constexpr int kMaxPriceBackoffMs = 3600'000;
// Nothing before this has a price anywhere: a batch of those with none says nothing of the source.
constexpr qint64 kFirstPricedTime = 1420070400; // 2015
// What the detail of a transaction reads at most of what it spends.
constexpr int kMaxDetailInputs = 300;
// A Merkle tree of a block has no more levels than this: 2^32 transactions would not fit.
constexpr int kMaxBranch = 32;
// A status can come ahead of the notice of the block that moved it, by this much at most.
constexpr int kStatusAhead = 2;
// Far more than any wallet has waiting. What is pending needs no proof, so this is what keeps a
// server from filling this PC with them.
constexpr int kMaxPending = 100'000;
// What was replaced or dropped stays in the list this long, longer with a note.
constexpr qint64 kKeepGoneSeconds = 90 * 86400;
// One server after another lying, for as long as they all do: that many in this much time at most.
constexpr qint64 kLieMovesLastMs = 10 * 60'000;
// Past this many raw transactions waiting, a list only one call in so many: what it adds has to go somewhere.
constexpr int kListEveryPastBacklog = 8;
// A list that is not what the server's status says may be ahead of a notice on its way: for this many
// first pauses (a minute), it is only asked again.
constexpr int kContradictedPauses = 4;

QString hexOf(const chain::Hash256& hash)
{
    return QString::fromStdString(chain::hashHex(hash));
}

int gFirstPauseMs = kFirstPauseMs;
int gMaxPending = kMaxPending;
int gMaxTxBacklog = int(kMaxTxBacklog);
int gFirstPriceBackoffMs = 60'000;

bool rawOf(const QJsonValue& result, chain::Bytes* out)
{
    if (!result.isString())
        return false;
    const QString hex = result.toString();
    if (hex.isEmpty() || hex.size() % 2 != 0)
        return false;
    const QByteArray raw = QByteArray::fromHex(hex.toLatin1());
    if (raw.size() != hex.size() / 2)
        return false;
    *out = chain::Bytes(raw.cbegin(), raw.cend());
    return true;
}

} // namespace

UtxoHistory::UtxoHistory(Network network, net::ElectrumClient* client, WalletModel* model, HistoryBook* book, QObject* parent)
    : QObject(parent), network_(network), client_(client), model_(model), book_(book)
{
    chain_ = new HeaderChain(networkInfo(network).proofs, client_, book_, this);
    prices_ = new net::PriceHistory(this);

    tokens_ = new QTimer(this);
    tokens_->setInterval(kTickMs);
    connect(tokens_, &QTimer::timeout, this, [this] {
        tokensLeft_ = callsPerTick();
        pump();
    });

    compare_ = new QTimer(this);
    compare_->setSingleShot(true);
    compare_->setInterval(kCompareAfterMs);
    connect(compare_, &QTimer::timeout, this, [this] {
        comparing_.unite(toCompare_);
        toCompare_.clear();
        compareNextPart();
    });

    priceTimer_ = new QTimer(this);
    priceTimer_->setSingleShot(true);
    connect(priceTimer_, &QTimer::timeout, this, &UtxoHistory::askPrices);

    progressTimer_ = new QTimer(this);
    progressTimer_->setSingleShot(true);
    progressTimer_->setInterval(kProgressMs);
    connect(progressTimer_, &QTimer::timeout, this, &UtxoHistory::progressed);

    pause_ = new QTimer(this);
    pause_->setSingleShot(true);
    connect(pause_, &QTimer::timeout, this, [this] {
        paused_ = false;
        askAgainAfterWait();
        pump();
    });

    again_ = new QTimer(this);
    again_->setSingleShot(true);
    connect(again_, &QTimer::timeout, this, &UtxoHistory::askAgainAfterWait);

    tipWait_ = new QTimer(this);
    tipWait_->setSingleShot(true);
    connect(tipWait_, &QTimer::timeout, this, &UtxoHistory::onTipWaitOver);

    connect(model_, &WalletModel::rowChanged, this, &UtxoHistory::onRowChanged);
    connect(model_, &WalletModel::rowsAppended, this, &UtxoHistory::onRowsAppended);
    connect(model_, &WalletModel::rowsDropped, this, &UtxoHistory::onRowsGone);
    connect(model_, &WalletModel::cleared, this, &UtxoHistory::onRowsGone);
    connect(client_, &net::ElectrumClient::ready, this, &UtxoHistory::onClientReady);
    connect(client_, &net::ElectrumClient::tipChanged, this, &UtxoHistory::onServerTip);
    connect(chain_, &HeaderChain::headerReady, this, &UtxoHistory::onHeaderReady);
    connect(chain_, &HeaderChain::headerMoved, this, &UtxoHistory::onHeaderMoved);
    connect(chain_, &HeaderChain::tipMoved, this, &UtxoHistory::onTipMoved);
    connect(chain_, &HeaderChain::reorged, this, &UtxoHistory::onReorg);
    connect(chain_, &HeaderChain::misbehaved, this, &UtxoHistory::onChainMisbehaved);
    connect(chain_, &HeaderChain::headersRefused, this, &UtxoHistory::onHeadersRefused);
}

UtxoHistory::~UtxoHistory() = default;

void UtxoHistory::setFirstPauseForTest(int ms)
{
    gFirstPauseMs = ms > 0 ? ms : kFirstPauseMs;
    gFirstPriceBackoffMs = ms > 0 ? ms : 60'000;
    HeaderChain::setRetryForTest(ms);
}

void UtxoHistory::setMaxPendingForTest(int count)
{
    gMaxPending = count > 0 ? count : kMaxPending;
}

void UtxoHistory::setMaxTxBacklogForTest(int count)
{
    gMaxTxBacklog = count > 0 ? count : int(kMaxTxBacklog);
}

void UtxoHistory::setProofsForTest(const ChainProofs& proofs)
{
    chain_->setProofsForTest(proofs);
}

int UtxoHistory::tip() const
{
    return chain_->tip();
}

bool UtxoHistory::connected() const
{
    return client_->isReady();
}

void UtxoHistory::start()
{
    running_ = true;
    ++generation_;
    inFlight_ = 0;
    paused_ = false;
    pause_->stop();
    again_->stop();
    tipWait_->stop();
    again_->setInterval(gFirstPauseMs);
    tipWait_->setInterval(2 * gFirstPauseMs);
    std::fill(std::begin(refused_), std::end(refused_), 0);
    pausedMs_ = 0;
    moves_ = 0;
    lieMoves_ = 0;
    lastLieMs_ = 0;
    ++priceRun_;
    headerRefusals_ = 0;
    saidDishonest_ = false;
    saidDiffering_ = false;
    retriedHere_ = false;
    movingOn_ = false;
    addressNews_.clear();
    addressQueue_.clear();
    queued_.clear();
    askedFor_.clear();
    reading_.clear();
    wanted_.clear();
    txQueue_.clear();
    refusedAddresses_.clear();
    retrying_.clear();
    namedHere_.clear();
    contradicted_.clear();
    differing_.clear();
    toCompare_.clear();
    comparing_.clear();
    pricesAsked_.clear();
    pricesInFlight_.clear();
    priceBackoffMs_ = 0;
    tokensLeft_ = callsPerTick();

    book_->forgetGoneBefore(QDateTime::currentSecsSinceEpoch() - kKeepGoneSeconds);
    chain_->start();
    // A header that no longer follows ended the chain there: what rested on it rests on nothing.
    book_->unproveFrom(chain_->tip() + 1);
    readListsAgain(addressesReplacedFrom(chain_->tip() + 1, std::numeric_limits<int>::max()));
    considerAll();
    queueAllProofs();
    tokens_->start();
    priceTimer_->start(kFirstPriceWaitMs);
    schedulePump();
    noteProgress();
}

void UtxoHistory::stop()
{
    running_ = false;
    ++generation_;
    ++priceRun_;
    tokens_->stop();
    compare_->stop();
    priceTimer_->stop();
    pause_->stop();
    again_->stop();
    tipWait_->stop();
    chain_->stop();
    inFlight_ = 0;
    noteProgress();
}

HistoryProgress UtxoHistory::progress() const
{
    HistoryProgress p;
    if (!running_)
        return p;
    p.addressesLeft = int(queued_.size() + reading_.size());
    p.transactionsLeft = int(wanted_.size());
    p.proofsLeft = int(proofQueued_.size());
    p.refused = int(refusedAddresses_.size());
    p.differing = int(differing_.size());
    p.unproven = int(failedProof_.size());
    return p;
}

void UtxoHistory::noteProgress()
{
    if (!progressTimer_->isActive())
        progressTimer_->start();
}

// ------------------------------------------------------------ what to read ----

void UtxoHistory::consider(const QString& scripthash, const QString& status, bool first)
{
    if (book_->statusOf(scripthash) == status) {
        // Empty on both sides: no history here, and none there.
        refusedAddresses_.remove(scripthash);
        retrying_.remove(scripthash);
        contradicted_.remove(scripthash);
        askedFor_.remove(scripthash); // what comes next is news, whatever it was once
        return;
    }
    if (reading_.contains(scripthash))
        return; // finishIfDone() looks again
    if (queued_.contains(scripthash)) {
        if (!first)
            addressNews_.push_front(scripthash); // it was waiting behind others; pump() skips the one left there
        return;
    }
    const auto asked = askedFor_.constFind(scripthash);
    if (asked != askedFor_.cend() && *asked == status)
        return; // that is what it was read for, and what came of it stands until a wait, or another server
    askedFor_.insert(scripthash, status);
    queued_.insert(scripthash);
    if (first)
        addressQueue_.push_back(scripthash);
    else
        addressNews_.push_front(scripthash); // news goes ahead of catching up
    schedulePump();
}

void UtxoHistory::considerAll()
{
    for (int row = 0; row < model_->size(); ++row) {
        const AddressRow& r = model_->at(row);
        if (r.network != network_ || r.scripthash.isEmpty())
            continue;
        const bool holds = r.confirmed != 0 || r.unconfirmed != 0;
        if (book_->findAddress(r.scripthash) >= 0 || !r.status.isEmpty())
            book_->describeAddress(r.scripthash, r.wallet, r.address);
        if (!r.status.isEmpty() || !book_->statusOf(r.scripthash).isEmpty())
            consider(r.scripthash, r.status, true);
        // What did not add up last time still does not: nothing else would look again.
        if (!r.status.isEmpty() || holds)
            compareSoon(r.scripthash);
    }
}

void UtxoHistory::lookAgain(const QString& scripthash)
{
    const int row = model_->findByScripthash(network_, scripthash);
    if (row >= 0 && model_->at(row).status != askedFor_.value(scripthash))
        consider(scripthash, model_->at(row).status, false);
}

void UtxoHistory::onRowChanged(int row)
{
    if (!running_ || row < 0 || row >= model_->size())
        return;
    const AddressRow& r = model_->at(row);
    if (r.network != network_ || r.scripthash.isEmpty())
        return;
    if (r.status.isEmpty() && book_->statusOf(r.scripthash).isEmpty()) {
        // A server that reads it for nobody, but says what it holds: a history is missing, and the
        // comparison says so.
        if (r.confirmed != 0 || r.unconfirmed != 0 || differing_.contains(r.scripthash))
            compareSoon(r.scripthash);
        return;
    }
    book_->describeAddress(r.scripthash, r.wallet, r.address);
    consider(r.scripthash, r.status, false);
    compareSoon(r.scripthash);
}

void UtxoHistory::onRowsAppended(int first, int last)
{
    if (!running_)
        return;
    for (int row = first; row <= last && row < model_->size(); ++row)
        onRowChanged(row);
}

void UtxoHistory::onRowsGone()
{
    const auto gone = [this](const QString& scripthash) { return model_->findByScripthash(network_, scripthash) < 0; };
    refusedAddresses_.removeIf(gone);
    retrying_.removeIf(gone);
    differing_.removeIf(gone);
    for (auto it = contradicted_.begin(); it != contradicted_.end();)
        it = gone(it.key()) ? contradicted_.erase(it) : std::next(it);
    noteProgress();
}

void UtxoHistory::onServerTip(int height)
{
    if (!running_ || !client_->isReady())
        return;
    // It has come as far as what was not asked of it for being past it. The chain does not hear of
    // that when it held those blocks already.
    const QSet<chain::Hash256> failed = failedProof_;
    for (const chain::Hash256& txid : failed) {
        const int position = book_->find(txid);
        if (position >= 0 && book_->at(position).height <= height) {
            failedProof_.remove(txid);
            queueProof(position);
        }
    }
    noteProgress();
}

void UtxoHistory::onClientReady()
{
    if (!running_)
        return;
    // Another connection. What the last server was asked and did not answer comes back from this one,
    // whose answers say nothing of that one, and what it listed binds nobody here: all of it is let go.
    ++generation_;
    inFlight_ = 0;
    paused_ = false;
    pause_->stop();
    again_->stop();
    pausedMs_ = 0;
    std::fill(std::begin(refused_), std::end(refused_), 0);
    headerRefusals_ = 0;
    saidDishonest_ = false;
    retriedHere_ = false;
    movingOn_ = false;
    contradicted_.clear();
    namedHere_.clear();
    const QStringList unfinished = reading_.keys();
    reading_.clear();
    wanted_.clear();
    txQueue_.clear();
    for (const QString& scripthash : unfinished)
        refusedAddresses_.insert(scripthash);
    // Another server is another chance for what the last would not give, and a proof that did not
    // hold may have been that server's alone.
    retryRefused();
    queueAllProofs();
    schedulePump();
    noteProgress();
}

// ----------------------------------------------------------------- pace ----

void UtxoHistory::schedulePump()
{
    if (!running_)
        return;
    if (!tokens_->isActive()) {
        tokensLeft_ = callsPerTick();
        tokens_->start();
    }
    if (pumpPosted_)
        return;
    pumpPosted_ = true;
    QTimer::singleShot(0, this, [this] {
        pumpPosted_ = false;
        pump();
    });
}

bool UtxoHistory::takeToken()
{
    if (tokensLeft_ <= 0)
        return false;
    --tokensLeft_;
    return true;
}

int UtxoHistory::callsPerTick() const
{
    return roundsRunning_ && roundsRunning_() ? kCallsPerTickBesideRounds : kCallsPerTick;
}

void UtxoHistory::pump()
{
    if (!running_ || paused_ || !client_->isReady())
        return;
    bool sent = false;
    while (inFlight_ < kMaxInFlight && takeToken()) {
        ++turn_;
        // Lists and raw transactions by turns: what a list names is what makes a line, and is lost
        // with the connection until it is read. News stands first in each of them, whatever the
        // first read of everything has put behind it. A list's turn is never a proof's: it would be none.
        const bool listsWait = !addressNews_.empty() || !addressQueue_.empty();
        const bool backlog = txQueue_.size() >= size_t(gMaxTxBacklog);
        const bool txTurn = !listsWait || (backlog ? turn_ % kListEveryPastBacklog != 1 : turn_ % 2 == 0);
        const auto takeTx = [this] {
            const chain::Hash256 txid = txQueue_.front();
            txQueue_.pop_front();
            sendTransaction(txid);
        };
        const auto takeProof = [this] {
            const chain::Hash256 txid = proofQueue_.front();
            proofQueue_.pop_front();
            proveNext(txid);
        };
        if (!proofQueue_.empty() && turn_ % kProofEvery == 0) {
            takeProof();
        } else if (!txQueue_.empty() && txTurn) {
            takeTx();
        } else if (!addressNews_.empty()) {
            const QString scripthash = addressNews_.front();
            addressNews_.pop_front();
            if (!queued_.contains(scripthash)) {
                ++tokensLeft_; // read already
                continue;
            }
            sendHistory(scripthash, true);
        } else if (!addressQueue_.empty()) {
            const QString scripthash = addressQueue_.front();
            addressQueue_.pop_front();
            if (!queued_.contains(scripthash)) {
                ++tokensLeft_;
                continue;
            }
            sendHistory(scripthash, false);
        } else if (!proofQueue_.empty()) {
            takeProof();
        } else {
            ++tokensLeft_; // nothing used it
            break;
        }
        sent = true;
    }
    if (sent)
        noteProgress();
    // Nothing to do, nothing that ticks until there is.
    if (addressNews_.empty() && addressQueue_.empty() && txQueue_.empty() && proofQueue_.empty())
        tokens_->stop();
}

bool UtxoHistory::moveOn(const QString& why, bool lie)
{
    if (movingOn_)
        return true;
    const int others = client_->serverCount() - 1;
    if (lie) {
        // What no server would give must not keep one that lies from being left, nor a lie that
        // every server tells keep the client going round them.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastLieMs_ > kLieMovesLastMs)
            lieMoves_ = 0;
        if (lieMoves_ >= others || !client_->tryAnotherServer(why))
            return false;
        ++lieMoves_;
        lastLieMs_ = now;
    } else {
        // Each server once: a server's other answers say nothing of what none of them will give, and
        // what a scanner reads there makes the client forget it has asked.
        if (moves_ >= others || !client_->tryAnotherServer(why))
            return false;
        ++moves_;
    }
    movingOn_ = true;
    return true;
}

void UtxoHistory::noteAddressRefused(const QString& scripthash)
{
    reading_.remove(scripthash);
    retrying_.remove(scripthash);
    refusedAddresses_.insert(scripthash);
}

void UtxoHistory::noteRefusal(Asked kind)
{
    const int count = ++refused_[kind];
    if (paused_ || movingOn_)
        return; // the rest of the same burst
    if (count >= kRefusalsBeforePause) {
        // A pause once may be a server catching its breath. Still refusing after it, and the next one is
        // asked instead; if there is none or it is no better, this one goes on being asked, ever more slowly.
        if (pausedMs_ > 0 && moveOn(QStringLiteral("it keeps refusing what a history needs")))
            return;
        pauseFor(pausedMs_ > 0 ? qMin(pausedMs_ * 2, kMaxPauseMs) : gFirstPauseMs);
        return;
    }
    // Few: a moment's trouble, or something that one server has limits on. Asked once more in a while,
    // and then of the next server. If there is none it stays as it is, and the page says so; a proof
    // still gets its second try, which is what ends it.
    if (retriedHere_ && moveOn(QStringLiteral("it refuses what a history needs")))
        return;
    if ((!retriedHere_ || !refusedOnce_.isEmpty()) && !again_->isActive())
        again_->start();
}

void UtxoHistory::noteAnswered(Asked kind)
{
    refused_[kind] = 0;
    if (std::all_of(std::begin(refused_), std::end(refused_), [](int count) { return count == 0; }))
        pausedMs_ = 0;
}

void UtxoHistory::noteAllWell()
{
    if (refusedAddresses_.isEmpty() && retrying_.isEmpty() && failedProof_.isEmpty() && refusedOnce_.isEmpty()) {
        moves_ = 0;
        retriedHere_ = false;
    }
}

void UtxoHistory::askAgainAfterWait()
{
    retriedHere_ = true;
    retryRefused();
    for (const chain::Hash256& txid : std::as_const(refusedOnce_))
        proofQueue_.push_back(txid); // still in proofQueued_
    schedulePump();
}

void UtxoHistory::retryRefused()
{
    const QSet<QString> refused = std::exchange(refusedAddresses_, {});
    for (const QString& scripthash : refused) {
        const int row = model_->findByScripthash(network_, scripthash);
        if (row < 0)
            continue;
        askedFor_.remove(scripthash);
        retrying_.insert(scripthash);
        consider(scripthash, model_->at(row).status, false);
    }
}

void UtxoHistory::pauseFor(int ms)
{
    pausedMs_ = ms;
    paused_ = true;
    pause_->start(ms);
}

void UtxoHistory::noteDishonest(const QString& why)
{
    const QString server = client_->serverName();
    const bool left = moveOn(why, true);
    if (!saidDishonest_) {
        saidDishonest_ = true;
        emit problem(Problem::Failure,
                     left ? QStringLiteral("A server's answer about a transaction did not hold, so it is being left: %1.").arg(why)
                          : QStringLiteral("A server's answer about a transaction did not hold, and no other server is left to ask: %1.").arg(why),
                     server);
    }
    qCWarning(lcHistory).noquote() << (left ? "leaving a server:" : "no server left to go to:") << why;
    noteProgress();
}

// -------------------------------------------------------------- histories ----

void UtxoHistory::sendHistory(const QString& scripthash, bool news)
{
    queued_.remove(scripthash);
    ++inFlight_;
    Reading reading;
    reading.news = news;
    reading_.insert(scripthash, reading);
    client_->request(QStringLiteral("blockchain.scripthash.get_history"), QJsonArray{scripthash},
                     [this, generation = generation_, scripthash](const QJsonValue& result, const QString& error) {
                         onHistory(generation, scripthash, result, error);
                     });
}

void UtxoHistory::onHistory(quint64 generation, const QString& scripthash, const QJsonValue& result, const QString& error)
{
    if (generation != generation_)
        return;
    --inFlight_;
    schedulePump();
    if (!error.isEmpty()) {
        qCInfo(lcHistory).noquote() << "a history was refused:" << error;
        // Some servers refuse a long history. The next one may not.
        noteAddressRefused(scripthash);
        noteRefusal(AskedHistory);
        lookAgain(scripthash);
        noteProgress();
        return;
    }

    QString status;
    bool readable = UtxoScanner::statusOfHistory(result, &status) >= 0;
    struct Listed {
        chain::Hash256 txid{};
        int height = 0;
    };
    QVector<Listed> listed;
    const QJsonArray entries = result.toArray();
    listed.reserve(entries.size());
    QSet<chain::Hash256> named;
    named.reserve(entries.size());
    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        Listed one;
        readable = readable && chain::hashFromHex(entry.value(QLatin1String("tx_hash")).toString().toStdString(), &one.txid);
        one.height = entry.value(QLatin1String("height")).toInt(-2);
        readable = readable && one.height >= -1;
        named.insert(one.txid);
        listed.append(one);
    }
    if (!readable) {
        noteAddressRefused(scripthash);
        noteDishonest(QStringLiteral("a list of transactions cannot be read"));
        return;
    }

    const int row = model_->findByScripthash(network_, scripthash);
    if (row < 0) {
        reading_.remove(scripthash);
        retrying_.remove(scripthash);
        return; // no longer one of the wallets'
    }
    // Not what the server's own status says. It moved past that while this was on its way, and its
    // news is coming, or it is not what it says: nothing of it counts either way. Of an address read
    // in rounds there is no such status: the row is only as new as its last round, and this is newer.
    const QString rowStatus = model_->at(row).status;
    if (!rowStatus.isEmpty() && status != rowStatus && client_->isFollowing(scripthash)) {
        noteAddressRefused(scripthash);
        Contradiction& contradiction = contradicted_[scripthash];
        if (contradiction.status != rowStatus) {
            contradiction.status = rowStatus;
            contradiction.since = QDateTime::currentMSecsSinceEpoch();
        }
        compareSoon(scripthash);
        lookAgain(scripthash);
        noteProgress();
        return;
    }

    // Nor does it take back what this PC has proven. Only the chain does that, as blocks give way.
    int against = -1;
    for (const Listed& one : std::as_const(listed)) {
        const int position = book_->find(one.txid);
        if (position >= 0 && book_->at(position).proven && book_->at(position).height != one.height && chain_->holds(book_->at(position).height))
            against = book_->at(position).height;
    }
    const int id = book_->findAddress(scripthash);
    if (id >= 0) {
        for (const int position : book_->addressAt(id).txs) {
            const TxRecord& tx = book_->at(position);
            if (tx.seen == TxSeen::Alive && tx.proven && !named.contains(tx.txid) && chain_->holds(tx.height))
                against = tx.height;
        }
    }
    if (against >= 0) {
        noteAddressRefused(scripthash);
        askServerOf(against, QStringLiteral("a list of transactions leaves out what is proven"));
        noteProgress();
        return;
    }
    noteAnswered(AskedHistory);

    Reading& reading = reading_[scripthash];
    reading.status = status;
    reading.listed.reserve(listed.size());
    for (const Listed& one : std::as_const(listed)) {
        reading.listed.append(one.txid);
        namedHere_.insert(one.txid);
        const int position = book_->find(one.txid);
        if (position >= 0) {
            // Nothing that was waiting on where it was has anything left to wait for.
            if (book_->at(position).height != one.height) {
                waitingTip_.remove(one.txid);
                proofQueued_.remove(one.txid);
                refusedOnce_.remove(one.txid);
                if (failedProof_.remove(one.txid))
                    compareTouching(position);
            }
            book_->setHeight(position, one.height);
            // A transaction read before this address was known to be one of the wallets'.
            if (!book_->hasEffectOn(position, scripthash))
                want(one.txid, one.height, scripthash);
        } else {
            want(one.txid, one.height, scripthash);
        }
    }
    // What it no longer lists. Only what touches this address can be judged from its list.
    if (id >= 0) {
        const QVector<int> touching = book_->addressAt(id).txs;
        for (const int position : touching) {
            const TxRecord& tx = book_->at(position);
            if (tx.seen == TxSeen::Alive && !named.contains(tx.txid)) {
                if (failedProof_.remove(tx.txid))
                    compareTouching(position);
                book_->markGone(position);
            }
        }
    }
    finishIfDone(scripthash);
    schedulePump();
}

void UtxoHistory::want(const chain::Hash256& txid, int height, const QString& scripthash)
{
    Reading& reading = reading_[scripthash];
    Wanted& wanted = wanted_[txid];
    wanted.height = height;
    wanted.news = wanted.news || reading.news;
    if (!wanted.addresses.contains(scripthash)) {
        wanted.addresses.insert(scripthash);
        ++reading.waiting;
    }
    if (!wanted.sent) {
        wanted.sent = true;
        if (reading.news)
            txQueue_.push_front(txid);
        else
            txQueue_.push_back(txid);
    }
}

void UtxoHistory::sendTransaction(const chain::Hash256& txid)
{
    ++inFlight_;
    client_->request(QStringLiteral("blockchain.transaction.get"), QJsonArray{hexOf(txid)},
                     [this, generation = generation_, txid](const QJsonValue& result, const QString& error) {
                         onTransaction(generation, txid, result, error);
                     });
}

void UtxoHistory::failWaiting(const Wanted& wanted)
{
    for (const QString& scripthash : wanted.addresses) {
        const auto reading = reading_.find(scripthash);
        if (reading == reading_.end())
            continue;
        reading->failed = true;
        --reading->waiting;
        finishIfDone(scripthash);
    }
}

void UtxoHistory::onTransaction(quint64 generation, const chain::Hash256& txid, const QJsonValue& result, const QString& error)
{
    if (generation != generation_)
        return;
    --inFlight_;
    schedulePump();
    const auto found = wanted_.constFind(txid);
    if (found == wanted_.cend())
        return;
    const Wanted wanted = *found;
    wanted_.erase(found);
    if (!error.isEmpty()) {
        qCInfo(lcHistory).noquote() << "a transaction was refused:" << error;
        // A pending one can be gone by the time it is asked for; the status will say so.
        noteRefusal(AskedTransaction);
        failWaiting(wanted);
        return;
    }

    chain::Bytes raw;
    chain::Transaction tx;
    if (!rawOf(result, &raw) || !chain::parseTransaction(raw, &tx) || tx.txid != txid) {
        noteDishonest(QStringLiteral("a transaction does not hash to its id"));
        failWaiting(wanted);
        return;
    }
    noteAnswered(AskedTransaction);
    if (wanted.height <= 0 && book_->find(txid) < 0 && book_->pendingCount() >= gMaxPending) {
        noteDishonest(QStringLiteral("it lists more pending transactions than any wallet has"));
        failWaiting(wanted);
        return;
    }
    const int position = book_->add(tx, wanted.height, QDateTime::currentSecsSinceEpoch(), [this](const QString& scripthash) {
        const int row = model_->findByScripthash(network_, scripthash);
        if (row < 0)
            return false;
        // Known from here on, whatever a scan has said of it yet: going by wallet needs it.
        book_->describeAddress(scripthash, model_->at(row).wallet, model_->at(row).address);
        return true;
    });
    book_->setHeight(position, wanted.height);
    if (wanted.height > 0) {
        if (const chain::BlockHeader* header = chain_->at(wanted.height))
            book_->setBlockTime(position, header->time);
    }
    for (const QString& scripthash : wanted.addresses) {
        const auto reading = reading_.find(scripthash);
        if (reading == reading_.end())
            continue;
        --reading->waiting;
        finishIfDone(scripthash);
    }
    queueProof(position, wanted.news);
}

void UtxoHistory::finishIfDone(const QString& scripthash)
{
    const auto found = reading_.constFind(scripthash);
    if (found == reading_.cend() || found->waiting > 0)
        return;
    const Reading reading = *found;
    reading_.erase(found);
    // No longer one of the wallets': nothing it listed could have counted for it.
    if (model_->findByScripthash(network_, scripthash) < 0) {
        retrying_.remove(scripthash);
        noteProgress();
        return;
    }
    // A list holds what touches the address, and what made what spends from it: by now each has
    // something to do with it, or the server named what it wished to.
    bool lied = false;
    if (!reading.failed) {
        for (const chain::Hash256& txid : reading.listed) {
            const int position = book_->find(txid);
            if (position < 0 || book_->hasEffectOn(position, scripthash))
                continue;
            lied = true;
            if (book_->effectsOf(position).isEmpty())
                book_->markGone(position);
        }
    }
    retrying_.remove(scripthash);
    if (reading.failed || lied) {
        refusedAddresses_.insert(scripthash);
        if (lied)
            noteDishonest(QStringLiteral("a list of transactions names what does not touch the address"));
        lookAgain(scripthash);
        noteProgress();
        return;
    }
    refusedAddresses_.remove(scripthash);
    contradicted_.remove(scripthash);
    book_->setStatus(scripthash, reading.status);
    queueProofsOf(scripthash, reading.news);
    compareSoon(scripthash);
    // It moved again while this was being read.
    lookAgain(scripthash);
    noteAllWell();
    noteProgress();
    if (priceTimer_ && !priceTimer_->isActive() && running_)
        priceTimer_->start(kFirstPriceWaitMs);
}

// ----------------------------------------------------------------- proofs ----

void UtxoHistory::queueProofsOf(const QString& scripthash, bool news)
{
    const int id = book_->findAddress(scripthash);
    if (id < 0)
        return;
    const QVector<int>& touching = book_->addressAt(id).txs;
    for (const int position : touching)
        queueProof(position, news);
}

void UtxoHistory::queueProof(int position, bool news)
{
    const TxRecord& tx = book_->at(position);
    // One that touches nothing has no line to prove. If it comes to, what finishes picks it up.
    if (tx.height <= 0 || tx.proven || tx.seen != TxSeen::Alive || proofQueued_.contains(tx.txid) || book_->effectsOf(position).isEmpty())
        return;
    proofQueued_.insert(tx.txid);
    if (news)
        proofQueue_.push_front(tx.txid);
    else
        proofQueue_.push_back(tx.txid);
    schedulePump();
}

void UtxoHistory::queueAllProofs()
{
    proofQueue_.clear();
    proofQueued_.clear();
    waitingHeader_.clear();
    waitingTip_.clear();
    refusedOnce_.clear();
    failedProof_.clear();
    doubtedHeights_.clear();
    tipWait_->stop();
    for (int position = 0; position < book_->size(); ++position)
        queueProof(position);
}

void UtxoHistory::proveNext(const chain::Hash256& txid)
{
    if (!proofQueued_.contains(txid))
        return; // let go of since
    const int position = book_->find(txid);
    if (position < 0) {
        proofQueued_.remove(txid);
        return;
    }
    const TxRecord& tx = book_->at(position);
    if (tx.height <= 0 || tx.proven || tx.seen != TxSeen::Alive) {
        proofQueued_.remove(txid);
        refusedOnce_.remove(txid);
        return;
    }
    const int height = tx.height;
    // In a block this server does not say it has. Nothing can be asked of it. That leaves it unproven,
    // whoever said so, until the server gets there or a list says otherwise; if this one listed that
    // itself, it is also its lie. A status can be ahead of the notice of its block, by a little.
    const bool named = namedHere_.contains(txid);
    if (height > client_->tipHeight() + (named ? kStatusAhead : 0)) {
        proofQueued_.remove(txid);
        refusedOnce_.remove(txid);
        failedProof_.insert(txid);
        compareTouching(position);
        if (named && height > client_->tipHeight() + kStatusAhead)
            noteDishonest(QStringLiteral("it lists a transaction in a block it does not have"));
        noteProgress();
        return;
    }
    if (chain_->at(height)) {
        book_->setBlockTime(position, chain_->at(height)->time);
        sendProof(txid, height);
        return;
    }
    // Nothing comes from a chain this server has lost, until it has its second chance.
    if (chain_->givenUp()) {
        proofQueued_.remove(txid);
        refusedOnce_.remove(txid);
        failedProof_.insert(txid);
        compareTouching(position);
        noteProgress();
        return;
    }
    // Past the tip this chain holds, or below the checkpoint and not asked for yet.
    if (height > chain_->tip()) {
        waitingTip_.insert(txid);
        if (!tipWait_->isActive()) {
            tipAtWait_ = chain_->tip();
            tipWait_->start();
        }
        chain_->sync();
        return;
    }
    waitingHeader_.insert(height, txid);
    chain_->ask(height);
}

void UtxoHistory::onHeaderReady(int height)
{
    const QList<chain::Hash256> waiting = waitingHeader_.values(height);
    waitingHeader_.remove(height);
    for (const chain::Hash256& txid : waiting) {
        const int position = book_->find(txid);
        if (position >= 0 && book_->at(position).height == height)
            book_->setBlockTime(position, chain_->at(height)->time);
        proofQueue_.push_front(txid);
    }
    schedulePump();
}

void UtxoHistory::onHeaderMoved(int height)
{
    // Kept from before the checkpoint moved over it, and another block stands there.
    for (int position = 0; position < book_->size(); ++position) {
        const TxRecord& tx = book_->at(position);
        if (tx.height != height)
            continue;
        const chain::Hash256 txid = tx.txid;
        book_->setProven(position, false);
        failedProof_.remove(txid);
        queueProof(position);
    }
    readListsAgain(addressesReplacedFrom(height, height));
    retryRefused();
    schedulePump();
}

void UtxoHistory::onTipMoved()
{
    const QSet<chain::Hash256> waiting = std::exchange(waitingTip_, {});
    for (const chain::Hash256& txid : waiting)
        proofQueue_.push_back(txid);
    const QSet<chain::Hash256> failed = std::exchange(failedProof_, {});
    for (const chain::Hash256& txid : failed) {
        const int position = book_->find(txid);
        if (position >= 0)
            queueProof(position);
    }
    // A new block is as good a moment as any to ask again for what was refused: with no other answer
    // between them that is no burst, however many blocks it takes, and no reason to stop reading.
    std::fill(std::begin(refused_), std::end(refused_), 0);
    retryRefused();
    schedulePump();
    noteProgress();
}

void UtxoHistory::onTipWaitOver()
{
    if (waitingTip_.isEmpty() || !client_->isReady())
        return;
    if (chain_->tip() != tipAtWait_) {
        tipAtWait_ = chain_->tip();
        tipWait_->start();
        chain_->sync();
        return;
    }
    // A block was said to hold them, and no such block has come: they stay unproven. If it was this
    // server that said so, that is also its lie. What another said of them is not its to answer for.
    bool itSaidSo = false;
    const QSet<chain::Hash256> waiting = std::exchange(waitingTip_, {});
    for (const chain::Hash256& txid : waiting) {
        proofQueued_.remove(txid);
        refusedOnce_.remove(txid);
        failedProof_.insert(txid);
        itSaidSo = itSaidSo || namedHere_.contains(txid);
        const int position = book_->find(txid);
        if (position >= 0)
            compareTouching(position);
    }
    noteProgress();
    if (itSaidSo)
        noteDishonest(QStringLiteral("it lists transactions in a block it does not give"));
}

void UtxoHistory::onReorg(int fromHeight)
{
    book_->unproveFrom(fromHeight);
    // An address a server follows hears of this by itself. One read in rounds would not until its next
    // round, and until then its transactions would be asked for where they no longer are, of servers
    // that rightly refuse: read now, and its list says where they are.
    QSet<QString> readAgain = addressesReplacedFrom(fromHeight, std::numeric_limits<int>::max());
    QVector<int> waitingOnAList;
    for (int position = 0; position < book_->size(); ++position) {
        if (book_->at(position).seen != TxSeen::Alive || book_->at(position).height < fromHeight)
            continue;
        bool followed = true;
        for (const TxEffect& effect : book_->effectsOf(position)) {
            const QString& scripthash = book_->addressAt(effect.address).scripthash;
            if (!client_->isFollowing(scripthash)) {
                readAgain.insert(scripthash);
                followed = false;
            }
        }
        if (followed)
            queueProof(position);
        else
            waitingOnAList.append(position);
    }
    readListsAgain(readAgain);
    // Unless no list is on its way: nobody will read that address, or its row has gone.
    for (const int position : std::as_const(waitingOnAList)) {
        const QVector<TxEffect>& effects = book_->effectsOf(position);
        const bool comes = std::any_of(effects.cbegin(), effects.cend(), [this](const TxEffect& effect) {
            const QString& scripthash = book_->addressAt(effect.address).scripthash;
            return queued_.contains(scripthash) || reading_.contains(scripthash);
        });
        if (!comes)
            queueProof(position);
    }
    // A list that left out what was proven was right after all.
    retryRefused();
    schedulePump();
}

// What a proof took down has that proof alone against it. With the proof gone, nothing else would
// look: the status of those addresses has not moved.
QSet<QString> UtxoHistory::addressesReplacedFrom(int fromHeight, int toHeight) const
{
    QSet<QString> out;
    for (int position = 0; position < book_->size(); ++position) {
        if (book_->at(position).seen != TxSeen::Replaced)
            continue;
        const int by = book_->find(book_->at(position).replacedBy);
        if (by < 0 || book_->at(by).proven || book_->at(by).height < fromHeight || book_->at(by).height > toHeight)
            continue;
        for (const TxEffect& effect : book_->effectsOf(position))
            out.insert(book_->addressAt(effect.address).scripthash);
    }
    return out;
}

void UtxoHistory::readListsAgain(const QSet<QString>& addresses)
{
    for (const QString& scripthash : addresses) {
        const int row = model_->findByScripthash(network_, scripthash);
        if (row < 0)
            continue;
        askedFor_.remove(scripthash);
        book_->setStatus(scripthash, QString());
        consider(scripthash, model_->at(row).status, false);
    }
}

void UtxoHistory::giveUpWaitingProofs()
{
    // What waited for this chain cannot come from this server.
    const QSet<chain::Hash256> waiting = std::exchange(waitingTip_, {});
    for (const chain::Hash256& txid : waiting) {
        proofQueued_.remove(txid);
        refusedOnce_.remove(txid);
        failedProof_.insert(txid);
    }
    const QList<chain::Hash256> waitingBelow = waitingHeader_.values();
    waitingHeader_.clear();
    for (const chain::Hash256& txid : waitingBelow) {
        proofQueued_.remove(txid);
        refusedOnce_.remove(txid);
        failedProof_.insert(txid);
    }
    tipWait_->stop();
    for (const chain::Hash256& txid : std::as_const(failedProof_)) {
        const int position = book_->find(txid);
        if (position >= 0)
            compareTouching(position);
    }
    noteProgress();
}

void UtxoHistory::onChainMisbehaved(const QString& why)
{
    giveUpWaitingProofs();
    noteDishonest(why);
}

void UtxoHistory::onHeadersRefused()
{
    // A try at a time, with a wait between them: once may be a busy moment. Twice, and no proof that
    // rests on those headers comes from this server.
    if (++headerRefusals_ < 2)
        return;
    if (!moveOn(QStringLiteral("it refuses the block headers a proof rests on")))
        giveUpWaitingProofs();
}

void UtxoHistory::onBadProof(const chain::Hash256& txid, int height)
{
    failedProof_.insert(txid);
    const int position = book_->find(txid);
    if (position >= 0)
        compareTouching(position);
    noteProgress();
    askServerOf(height, QStringLiteral("a Merkle proof does not lead to its block"));
}

// What a server says goes against a block this PC goes by. It may be this chain that is behind, or
// holds a block that has given way: the server's own header for that height tells. The same one, or
// none at all from one that says it has it, and the server is what is wrong.
void UtxoHistory::askServerOf(int height, const QString& lie)
{
    // It has not got that far. It will.
    if (client_->tipHeight() < height || doubtedHeights_.contains(height))
        return;
    doubtedHeights_.insert(height);
    client_->request(QStringLiteral("blockchain.block.header"), QJsonArray{height},
                     [this, generation = generation_, height, lie](const QJsonValue& result, const QString& error) {
        if (generation != generation_)
            return; // another server's answer now, and says nothing of the one that said it
        doubtedHeights_.remove(height);
        const chain::BlockHeader* held = chain_->at(height);
        chain::Bytes raw;
        chain::BlockHeader header;
        if (!error.isEmpty() || !rawOf(result, &raw) || !chain::parseHeader(raw, &header) || (held && header.hash == held->hash)) {
            noteDishonest(lie);
            return;
        }
        if (height <= chain_->proofs().checkpointHeight) {
            // Which one stands there is not its word: a branch to the checkpoint says.
            if (held)
                chain_->askAgain(height);
            return;
        }
        // Another block there is one more thing to prove, with the work of a real one.
        if (held && (header.bits != held->bits || !chain::headerHasWork(header, chain_->proofs().powLimit))) {
            noteDishonest(lie);
            return;
        }
        chain_->sync();
    });
}

void UtxoHistory::sendProof(const chain::Hash256& txid, int height)
{
    ++inFlight_;
    client_->request(QStringLiteral("blockchain.transaction.get_merkle"), QJsonArray{hexOf(txid), height},
                     [this, generation = generation_, txid, height](const QJsonValue& result, const QString& error) {
                         onProof(generation, txid, height, result, error);
                     });
}

void UtxoHistory::onProof(quint64 generation, const chain::Hash256& txid, int height, const QJsonValue& result, const QString& error)
{
    if (generation != generation_)
        return;
    --inFlight_;
    schedulePump();
    const int position = book_->find(txid);
    if (position < 0 || book_->at(position).height != height) {
        proofQueued_.remove(txid);
        refusedOnce_.remove(txid);
        if (position >= 0)
            queueProof(position); // it moved to another block meanwhile
        return;
    }
    if (!error.isEmpty()) {
        qCInfo(lcHistory).noquote() << "a proof was refused:" << error;
        // Once may be a busy moment. It stays on the list, and is asked once more in a while.
        if (!refusedOnce_.contains(txid)) {
            refusedOnce_.insert(txid);
        } else {
            refusedOnce_.remove(txid);
            proofQueued_.remove(txid);
            failedProof_.insert(txid);
            compareTouching(position);
        }
        noteRefusal(AskedProof);
        noteProgress();
        return;
    }
    proofQueued_.remove(txid);
    refusedOnce_.remove(txid);

    const QJsonObject answer = result.toObject();
    const QJsonArray steps = answer.value(QLatin1String("merkle")).toArray();
    const double pos = answer.value(QLatin1String("pos")).toDouble(-1.0);
    bool whole = answer.value(QLatin1String("block_height")).toInt(-1) == height && pos >= 0.0 && steps.size() <= kMaxBranch
                 && pos < double(quint64(1) << steps.size());
    std::vector<chain::Hash256> branch;
    branch.reserve(size_t(steps.size()));
    for (const QJsonValue& step : steps) {
        chain::Hash256 hash{};
        whole = whole && chain::hashFromHex(step.toString().toStdString(), &hash);
        branch.push_back(hash);
    }
    const chain::BlockHeader* header = chain_->at(height);
    if (!header) {
        // It went while this was on its way.
        queueProof(position);
        return;
    }
    if (!whole || !chain::proofLeadsTo(txid, branch, quint64(pos), header->merkleRoot)) {
        onBadProof(txid, height);
        return;
    }
    noteAnswered(AskedProof);
    const bool hadFailed = failedProof_.remove(txid);
    book_->setBlockTime(position, header->time);
    book_->setProven(position, true, int(pos));
    // Failed before this connection or block, it may have been left out of what did not add up.
    if (hadFailed || !differing_.isEmpty())
        compareTouching(position);
    noteAllWell();
    if (!priceTimer_->isActive() && running_)
        priceTimer_->start(kFirstPriceWaitMs);
    noteProgress();
}

// ------------------------------------------------- what the history adds up to ----

void UtxoHistory::compareSoon(const QString& scripthash)
{
    toCompare_.insert(scripthash);
    if (!compare_->isActive())
        compare_->start();
}

void UtxoHistory::compareTouching(int position)
{
    const QVector<TxEffect>& effects = book_->effectsOf(position);
    for (const TxEffect& effect : effects)
        compareSoon(book_->addressAt(effect.address).scripthash);
}

void UtxoHistory::compareNextPart()
{
    if (!running_)
        return;
    int done = 0;
    for (auto it = comparing_.begin(); it != comparing_.end() && done < kComparePerTurn; ++done) {
        const QString scripthash = *it;
        it = comparing_.erase(it);
        compareBalance(scripthash);
    }
    if (!comparing_.isEmpty())
        QTimer::singleShot(0, this, &UtxoHistory::compareNextPart);
    noteProgress();
}

void UtxoHistory::compareBalance(const QString& scripthash)
{
    const int row = model_->findByScripthash(network_, scripthash);
    if (row < 0) {
        differing_.remove(scripthash);
        return;
    }
    const AddressRow& r = model_->at(row);
    const bool onItsWay = queued_.contains(scripthash) || reading_.contains(scripthash);
    // A list that said otherwise than the status, and the status has not moved: asked once more, and
    // if that is what it still says, it is not what it says.
    const auto contradicted = contradicted_.constFind(scripthash);
    if (contradicted != contradicted_.cend() && contradicted->status == r.status && !onItsWay) {
        differing_.remove(scripthash);
        if (QDateTime::currentMSecsSinceEpoch() - contradicted->since >= qint64(kContradictedPauses) * gFirstPauseMs) {
            noteDishonest(QStringLiteral("a list of transactions is not what its status says"));
        } else {
            refusedAddresses_.remove(scripthash);
            askedFor_.remove(scripthash);
            consider(scripthash, r.status, false);
        }
        return;
    }
    // An address read in rounds has its balance from the last one, and its history may be newer.
    const bool roundBehind = r.status != book_->statusOf(scripthash) && !client_->isFollowing(scripthash);
    if (!r.checked || onItsWay || roundBehind || refusedAddresses_.contains(scripthash)) {
        differing_.remove(scripthash); // what there is to compare with is on its way, or is not all there
        return;
    }
    if (balanceOnItsWay_ && balanceOnItsWay_(scripthash)) {
        differing_.remove(scripthash);
        compareSoon(scripthash);
        return;
    }
    // A transaction whose proof failed is nothing to go by. One on its way is, for now.
    if (book_->netOf(scripthash, &failedProof_) == r.confirmed + r.unconfirmed) {
        differing_.remove(scripthash);
        return;
    }
    // It may be a balance late on its way, the first time. Looked at again, it is what it is.
    if (!differing_.contains(scripthash)) {
        differing_.insert(scripthash);
        compareSoon(scripthash);
    } else if (!saidDiffering_) {
        saidDiffering_ = true;
        emit problem(Problem::Failure, QStringLiteral("What a server says an address holds is not what its transactions add up to."), client_->serverName());
    }
}

// ----------------------------------------------------------------- prices ----

void UtxoHistory::askPrices()
{
    if (!running_ || !pricesInFlight_.isEmpty())
        return;
    QList<qint64> times;
    QSet<qint64> seen;
    for (int position = 0; position < book_->size() && times.size() < net::PriceHistory::kMaxTimesPerCall; ++position) {
        const TxRecord& tx = book_->at(position);
        if (!tx.proven || tx.time <= 0 || tx.usdThen > 0.0 || pricesAsked_.contains(tx.time) || seen.contains(tx.time))
            continue;
        seen.insert(tx.time);
        times.append(tx.time);
    }
    if (times.isEmpty())
        return;
    pricesInFlight_ = seen;
    const AssetInfo& coin = assetInfo(networkInfo(network_).coin);
    prices_->ask(coin, times, [this, run = priceRun_, times](const net::PriceHistory::Answers& answers, bool failed) {
        if (run == priceRun_)
            onPrices(answers, times, failed);
    });
}

void UtxoHistory::onPrices(const QHash<qint64, QPair<double, qint64>>& answers, const QList<qint64>& asked, bool failed)
{
    pricesInFlight_.clear();
    // Nothing at all for moments it has prices for is a bad hour at the source, and no answer.
    const bool couldHave = std::any_of(asked.cbegin(), asked.cend(), [](qint64 time) { return time >= kFirstPricedTime; });
    if (failed || (answers.isEmpty() && couldHave)) {
        priceBackoffMs_ = priceBackoffMs_ > 0 ? qMin(priceBackoffMs_ * 2, kMaxPriceBackoffMs) : gFirstPriceBackoffMs;
        priceTimer_->start(priceBackoffMs_);
        return;
    }
    priceBackoffMs_ = 0;
    // With no answer the source has no more to say this run. With one, what is proven later at that
    // moment asks for it again.
    for (const qint64 time : asked) {
        if (!answers.contains(time))
            pricesAsked_.insert(time);
    }
    if (!answers.isEmpty()) {
        for (int position = 0; position < book_->size(); ++position) {
            const TxRecord& tx = book_->at(position);
            if (!tx.proven || tx.usdThen > 0.0)
                continue;
            const auto found = answers.constFind(tx.time);
            if (found != answers.cend())
                book_->setUsdThen(position, found->first, found->second);
        }
    }
    priceTimer_->start(1000);
}

// ----------------------------------------------------------------- detail ----

void UtxoHistory::readDetail(const chain::Hash256& txid, QObject* guard, const std::function<void(const TxDetail&)>& done)
{
    QPointer<QObject> alive(guard);
    const QPointer<UtxoHistory> self(this);
    client_->request(QStringLiteral("blockchain.transaction.get"), QJsonArray{hexOf(txid)},
                     [this, self, alive, txid, done](const QJsonValue& result, const QString& error) {
        if (!self || !alive)
            return;
        TxDetail detail;
        chain::Bytes raw;
        chain::Transaction tx;
        if (!error.isEmpty()) {
            detail.why = QStringLiteral("The server did not give this transaction.");
            done(detail);
            return;
        }
        if (!rawOf(result, &raw) || !chain::parseTransaction(raw, &tx) || tx.txid != txid) {
            detail.why = QStringLiteral("What the server gave does not hash to this transaction.");
            noteDishonest(QStringLiteral("a transaction does not hash to its id"));
            done(detail);
            return;
        }
        const chain::AddressFormat format = networkInfo(network_).format;
        detail.size = tx.size;
        detail.vsize = tx.vsize;
        detail.coinbase = tx.coinbase();
        // Only an id's bytes are proven. What this server says the rest comes to is as good as another's.
        const int here = book_->find(txid);
        if (here >= 0)
            book_->setVsize(here, tx.vsize);
        for (const chain::TxOutput& output : tx.outputs) {
            TxSideLine line;
            line.address = QString::fromStdString(chain::addressFromScript(output.script, format));
            line.value = qint64(output.value);
            line.ours = model_->findByScripthash(network_, QString::fromStdString(chain::scripthashOfScript(output.script))) >= 0;
            detail.outputs.append(line);
        }
        if (tx.coinbase()) {
            done(detail);
            return;
        }
        // What it spends is known only from the transactions that made those outputs.
        struct Shared {
            TxDetail detail;
            QVector<chain::OutPoint> points;
            QHash<chain::Hash256, chain::Transaction> parents;
            qint64 outputs = 0;
            int left = 0;
        };
        auto shared = std::make_shared<Shared>();
        shared->detail = detail;
        shared->outputs = qint64(tx.outputTotal());
        const int reading = qMin(int(tx.inputs.size()), kMaxDetailInputs);
        shared->detail.inputsLeftOut = int(tx.inputs.size()) - reading;
        QSet<chain::Hash256> parents;
        for (int i = 0; i < reading; ++i) {
            shared->points.append(tx.inputs[size_t(i)].previous);
            parents.insert(tx.inputs[size_t(i)].previous.txid);
        }
        shared->left = int(parents.size());
        const auto finish = [this, self, alive, shared, done] {
            if (!self || !alive)
                return;
            qint64 in = 0;
            bool whole = shared->detail.inputsLeftOut == 0;
            const chain::AddressFormat format = networkInfo(network_).format;
            for (const chain::OutPoint& point : std::as_const(shared->points)) {
                TxSideLine line;
                const auto parent = shared->parents.constFind(point.txid);
                if (parent != shared->parents.cend() && point.n < parent->outputs.size()) {
                    const chain::TxOutput& output = parent->outputs[point.n];
                    line.address = QString::fromStdString(chain::addressFromScript(output.script, format));
                    line.value = qint64(output.value);
                    line.ours = model_->findByScripthash(network_, QString::fromStdString(chain::scripthashOfScript(output.script))) >= 0;
                    in += line.value;
                } else {
                    whole = false;
                    ++shared->detail.inputsUnread;
                }
                shared->detail.inputs.append(line);
            }
            // Shown there and only there: in the list a fee is what the wallets paid.
            if (whole && in >= shared->outputs)
                shared->detail.fee = in - shared->outputs;
            done(shared->detail);
        };
        if (shared->left == 0) {
            finish();
            return;
        }
        for (const chain::Hash256& parent : std::as_const(parents)) {
            client_->request(QStringLiteral("blockchain.transaction.get"), QJsonArray{hexOf(parent)},
                             [this, self, alive, shared, parent, finish](const QJsonValue& parentResult, const QString& parentError) {
                if (!self || !alive)
                    return;
                chain::Bytes parentRaw;
                chain::Transaction parentTx;
                if (parentError.isEmpty() && rawOf(parentResult, &parentRaw) && chain::parseTransaction(parentRaw, &parentTx)
                    && parentTx.txid == parent)
                    shared->parents.insert(parent, std::move(parentTx));
                if (--shared->left == 0)
                    finish();
            });
        }
    });
}

} // namespace wallet
