#pragma once

#include "net/Budget.h"
#include "wallet/ChainScanner.h"

#include <QHash>
#include <QList>
#include <QObject>

namespace wallet {

class HistoryBook;
class UtxoHistory;
class WalletModel;

// What one network's scan is doing, as the pages read it.
struct NetworkStatus {
    Network network{};
    bool watched = false;     // some wallet has a key that serves it
    bool scanning = false;    // first answers still on their way
    bool limit = false;       // a window is cut at the row limit
    bool connecting = false;  // no server to ask yet
    bool wrongChain = false;  // the server in use answers for another network
    bool failing = false;     // the last round left addresses without an answer
    QDateTime balancesAt;     // when every address last had a full answer; invalid before the first time
    QDateTime noncesAt;       // Evm: when every nonce was last read
    NonceSweep sweep;         // Evm: how far the next such reading has got
    bool answered = false;    // everything it follows has answered in this run
    QString server;           // host only
    ScanTiers tiers;
};

// Runs the scans: one ChainScanner per network the list of coins names, of the kind its
// family needs, and one budget per provider, which all of them share, so a host serving
// several networks is asked no more than one that serves one. GUI thread only.
class Scheduler : public QObject
{
    Q_OBJECT
public:
    explicit Scheduler(WalletModel* model, QObject* parent = nullptr);
    ~Scheduler() override;

    // Every network whose history is read, as the pages and the database want it.
    QList<UtxoHistory*> histories() const { return histories_; }
    QList<HistoryBook*> books() const;

    void configure(const QList<AccountKey>& accounts, const ScanSettings& settings);
    void start(); // the networks with accounts; none: no connections at all
    void stop();
    void refreshNow();
    void noteIssued(const AccountKey& key); // see ChainScanner::noteIssued()
    // What the database kept of the last run, before start().
    void restoreTimes(Network network, const QDateTime& balancesAt, const QDateTime& noncesAt, const NonceSweep& sweep);
    bool running() const { return running_; }

    bool hasWork() const;    // some network has accounts
    bool isScanning() const; // any network
    bool allAnswered() const; // every network with accounts: see ChainScanner::answered()
    bool limitReached() const;
    bool anyConnecting() const;
    NetworkStatus status(Network network) const;
    QList<NetworkStatus> statuses() const;

signals:
    void historyProgress(); // how far a history has got moved
    void scanFinished(); // every network with accounts has its first answers in
    void progress();     // counts or states moved
    void serverChanged();
    void problem(wallet::Problem kind, const QString& message, const QString& serverDetail);

private:
    void onScanFinished(Network network);

    net::BudgetPool budgets_; // before the scanners: their clients hold a pointer to it
    QList<ChainScanner*> scanners_;
    QList<UtxoHistory*> histories_;
    QHash<Network, bool> done_;
    bool running_ = false;
    bool finishedSent_ = false; // scanFinished() already emitted for this start
};

} // namespace wallet
