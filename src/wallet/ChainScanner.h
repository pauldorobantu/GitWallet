#pragma once

#include "wallet/Types.h"

#include <QDateTime>
#include <QObject>

namespace wallet {

class WalletModel;

// Follows one network: derives the addresses of the accounts that serve it, asks its
// servers about them and writes what they answer into the model. One per network, of
// the kind its family needs; Scheduler owns them all.
class ChainScanner : public QObject
{
    Q_OBJECT
public:
    ChainScanner(Network network, WalletModel* model, QObject* parent)
        : QObject(parent), network_(network), model_(model)
    {
    }

    Network network() const { return network_; }
    const NetworkInfo& info() const { return networkInfo(network_); }

    // Take effect at the next start(). Accounts of other networks are ignored.
    virtual void setAccounts(const QList<AccountKey>& accounts) = 0;
    virtual void setSettings(const ScanSettings& settings) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void refreshNow() {}
    // The receive address at `index` of this account was given out, just now: a payment comes to those,
    // so they count as used for how far the window reaches, and those not paid yet are followed first.
    virtual void noteGiven(const AccountKey& key, quint32 index)
    {
        Q_UNUSED(key);
        Q_UNUSED(index);
    }
    // What the database kept of the last run, before the next start().
    virtual void restoreTimes(const QDateTime& balancesAt, const QDateTime& noncesAt, const NonceSweep& sweep)
    {
        Q_UNUSED(balancesAt);
        Q_UNUSED(noncesAt);
        Q_UNUSED(sweep);
    }

    virtual bool hasAccounts() const = 0;
    // True until every address the scan should reach has its first answer.
    virtual bool isScanning() const = 0;
    // Everything it follows has answered in this run, even if a limit keeps the scan from
    // reaching all it should: no figure left is only what the database kept.
    virtual bool answered() const = 0;
    virtual bool limitReached() const = 0;      // a window is cut at the row limit
    virtual bool connecting() const { return false; } // nothing can be asked yet
    virtual bool wrongChain() const { return false; } // the server last tried answers for another network
    // A round left addresses without an answer, or the next is long overdue: figures are as of
    // balancesAt() at best.
    virtual bool refreshFailing() const { return false; }
    // When every address last had a full answer (local time); invalid before the first time.
    virtual QDateTime balancesAt() const = 0;
    // When every nonce was last read; invalid where there are none, or before the first time.
    virtual QDateTime noncesAt() const { return {}; }
    virtual NonceSweep nonceSweep() const { return {}; } // the one under way, if any
    virtual QString serverName() const = 0;     // host only: an endpoint's URL may carry an API key
    virtual ScanTiers tiers() const = 0;

signals:
    void progress(int checked, int derived);
    void scanFinished(); // the first answers are all in; updates continue
    void passFinished(); // a round of polling ended, whether or not anything moved
    void serverChanged();
    // `message` is GitWallet's own sentence; `serverDetail` is what a server wrote, if
    // anything (one line, cleaned up), and is never part of `message`.
    void problem(wallet::Problem kind, const QString& message, const QString& serverDetail);

protected:
    Network network_{};
    WalletModel* model_ = nullptr;
};

} // namespace wallet
