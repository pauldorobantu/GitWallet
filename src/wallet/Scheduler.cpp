#include "wallet/Scheduler.h"

#include "wallet/EvmScanner.h"
#include "wallet/History.h"
#include "wallet/UtxoHistory.h"
#include "wallet/UtxoScanner.h"

#include <algorithm>

namespace wallet {

Scheduler::Scheduler(WalletModel* model, QObject* parent) : QObject(parent)
{
    for (const NetworkInfo& info : networks()) {
        ChainScanner* scanner = nullptr;
        switch (info.family) {
        case Family::Utxo: {
            auto* utxo = new UtxoScanner(info.network, model, this);
            scanner = utxo;
            auto* book = new HistoryBook(info.network, this);
            auto* history = new UtxoHistory(info.network, utxo->client(), model, book, this);
            history->setRoundsRunning([utxo] { return utxo->tiers().rounds > 0; });
            history->setBalanceOnItsWay([utxo](const QString& scripthash) { return utxo->balanceOnItsWay(scripthash); });
            histories_.append(history);
            connect(history, &UtxoHistory::progressed, this, &Scheduler::historyProgress);
            connect(history, &UtxoHistory::problem, this, [this](Problem kind, const QString& message, const QString& detail) {
                if (running_)
                    emit problem(kind, message, detail);
            });
            break;
        }
        case Family::Evm:
            scanner = new EvmScanner(info.network, &budgets_, model, this);
            break;
        }
        const Network network = info.network;
        connect(scanner, &ChainScanner::scanFinished, this, [this, network] { onScanFinished(network); });
        // Progress is also the cue for the scanners' other states: EvmScanner sends it
        // when a pass gives up on addresses, UtxoScanner when a failure makes it busy again.
        connect(scanner, &ChainScanner::progress, this, &Scheduler::progress);
        // The only signal of a refresh pass that changed no row, failed or not.
        connect(scanner, &ChainScanner::passFinished, this, &Scheduler::progress);
        connect(scanner, &ChainScanner::serverChanged, this, &Scheduler::serverChanged);
        connect(scanner, &ChainScanner::problem, this, [this](Problem kind, const QString& message, const QString& detail) {
            // While stopping, the clients fail every pending request ("stopped"):
            // that is the shutdown itself, not something to show the user.
            if (running_)
                emit problem(kind, message, detail);
        });
        scanners_.append(scanner);
    }
}

Scheduler::~Scheduler()
{
    blockSignals(true);
    stop();
    // They share the scanners' clients, so they go first.
    qDeleteAll(histories_);
    histories_.clear();
    // Here, while budgets_ is still there for their clients.
    qDeleteAll(scanners_);
    scanners_.clear();
}

void Scheduler::configure(const QList<AccountKey>& accounts, const ScanSettings& settings)
{
    for (ChainScanner* scanner : std::as_const(scanners_)) {
        scanner->setAccounts(accounts);
        scanner->setSettings(settings);
    }
}

void Scheduler::start()
{
    // Set before any scanner starts, in case one finishes synchronously. A network
    // without accounts has nothing to scan and counts as finished.
    done_.clear();
    finishedSent_ = false;
    for (const ChainScanner* scanner : std::as_const(scanners_))
        done_.insert(scanner->network(), !scanner->hasAccounts());
    if (!hasWork())
        return; // no wallet: no connections at all
    running_ = true;
    for (ChainScanner* scanner : std::as_const(scanners_)) {
        if (scanner->hasAccounts())
            scanner->start();
    }
    for (UtxoHistory* history : std::as_const(histories_)) {
        const auto scanner = std::find_if(scanners_.cbegin(), scanners_.cend(), [history](const ChainScanner* s) { return s->network() == history->network(); });
        if (scanner != scanners_.cend() && (*scanner)->hasAccounts())
            history->start();
    }
}

void Scheduler::stop()
{
    running_ = false;
    for (UtxoHistory* history : std::as_const(histories_))
        history->stop();
    for (ChainScanner* scanner : std::as_const(scanners_))
        scanner->stop();
}

QList<HistoryBook*> Scheduler::books() const
{
    QList<HistoryBook*> out;
    for (const UtxoHistory* history : histories_)
        out.append(history->book());
    return out;
}

void Scheduler::noteGiven(const AccountKey& key, quint32 index)
{
    for (ChainScanner* scanner : std::as_const(scanners_)) {
        if (key.serves(scanner->network()))
            scanner->noteGiven(key, index);
    }
}

void Scheduler::refreshNow()
{
    if (!running_)
        return;
    for (ChainScanner* scanner : std::as_const(scanners_))
        scanner->refreshNow();
}

void Scheduler::restoreTimes(Network network, const QDateTime& balancesAt, const QDateTime& noncesAt, const NonceSweep& sweep)
{
    for (ChainScanner* scanner : std::as_const(scanners_)) {
        if (scanner->network() == network)
            scanner->restoreTimes(balancesAt, noncesAt, sweep);
    }
}

bool Scheduler::allAnswered() const
{
    return running_ && std::all_of(scanners_.cbegin(), scanners_.cend(), [](const ChainScanner* s) {
        return !s->hasAccounts() || s->answered();
    });
}

bool Scheduler::hasWork() const
{
    return std::any_of(scanners_.cbegin(), scanners_.cend(), [](const ChainScanner* s) { return s->hasAccounts(); });
}

bool Scheduler::isScanning() const
{
    return std::any_of(scanners_.cbegin(), scanners_.cend(), [](const ChainScanner* s) { return s->isScanning(); });
}

bool Scheduler::limitReached() const
{
    return std::any_of(scanners_.cbegin(), scanners_.cend(), [](const ChainScanner* s) { return s->limitReached(); });
}

bool Scheduler::anyConnecting() const
{
    return std::any_of(scanners_.cbegin(), scanners_.cend(), [](const ChainScanner* s) { return s->connecting(); });
}

NetworkStatus Scheduler::status(Network network) const
{
    NetworkStatus out;
    out.network = network;
    for (const ChainScanner* s : scanners_) {
        if (s->network() != network)
            continue;
        out.watched = s->hasAccounts();
        out.scanning = s->isScanning();
        out.limit = s->limitReached();
        out.connecting = s->connecting();
        // What no wallet looks at any more has nothing to say.
        out.wrongChain = out.watched && s->wrongChain();
        out.failing = out.watched && s->refreshFailing();
        out.balancesAt = s->balancesAt();
        out.noncesAt = s->noncesAt();
        out.sweep = s->nonceSweep();
        out.answered = s->answered();
        out.server = s->serverName();
        out.tiers = s->tiers();
    }
    return out;
}

QList<NetworkStatus> Scheduler::statuses() const
{
    QList<NetworkStatus> out;
    for (const ChainScanner* s : scanners_)
        out.append(status(s->network()));
    return out;
}

void Scheduler::onScanFinished(Network network)
{
    if (!running_)
        return; // a late signal from a run that stop() ended
    done_.insert(network, true);
    emit progress();
    const bool all = std::all_of(done_.cbegin(), done_.cend(), [](bool done) { return done; });
    if (all && !finishedSent_) {
        finishedSent_ = true; // before emitting: a listener may restart the scans
        emit scanFinished();
    }
}

} // namespace wallet
