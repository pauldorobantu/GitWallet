#pragma once

#include "wallet/Types.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>

namespace net {
class ElectrumClient;
}

namespace wallet {

class WalletModel;

// Walks every Bitcoin account's receive (0) and change (1) chain from index 0,
// deriving addresses publicly from the stored xpub, subscribing each scripthash
// on the Electrum server and fetching the balance of every used address. A
// chain is scanned until `gap` consecutive unused addresses follow the last used
// one. When a later address becomes used (a subscription notification), the
// window grows so the gap holds again — addresses another app creates from the
// same seed therefore show up by themselves. A row counts as checked once its
// status has answered, and for a used address its first balance too; a known
// balance stays on screen, checked, until a newer one replaces it. The wallet
// follows at most a number of addresses tied to the gaps, so a server that
// calls every address used cannot grow it without end: a scan cut short there
// stays unfinished, limitReached() turns true, and problem() says why.
class BtcScanner : public QObject
{
    Q_OBJECT
public:
    BtcScanner(net::ElectrumClient* client, WalletModel* model, QObject* parent = nullptr);

    void setAccounts(const QList<AccountKey>& accounts); // non-Bitcoin accounts are ignored
    void setGaps(int receiveGap, int changeGap);
    // (Re)starts once the client is ready. Rows already in the model are
    // adopted instead of derived again, but every status is checked again
    // from index 0: the window follows this run's server and gaps.
    void start();
    void stop();

    // True until every chain has its full gap checked and every balance has
    // answered; for good while a chain is stopped at the row limit.
    bool isScanning() const;
    bool limitReached() const; // some chain's window reaches past the row limit
    int checkedCount() const;  // Bitcoin rows whose status, and balance if used, have answered
    int derivedCount() const;  // Bitcoin rows derived so far

signals:
    void progress(int checked, int derived);
    void scanFinished(); // initial scan complete; live updates continue
    // `message` is GitWallet's own sentence; `serverDetail` is what an Electrum
    // server wrote, if anything (one line, at most 200 characters, without
    // control or bidi characters), and is never part of `message`.
    void problem(wallet::Problem kind, const QString& message, const QString& serverDetail);

private:
    // --- implementation (BtcScanner.cpp may add private members) ---

    // One chain of one account: receive (change 0) or change (change 1).
    struct Chain {
        chain::ScriptType type = chain::ScriptType::P2WPKH;
        quint32 account = 0;
        quint32 change = 0;
        chain::HdPubKey node; // the account node's child `change`, derived once per start()
        int gap = 0;
        int floor = 0;        // rows [0, floor) are derived whatever the statuses: another wallet's, to replace
        int next = 0;         // next index to derive (or adopt from the model)
        int lastUsed = -1;    // highest index this run has seen used
        bool reuse = true;    // the rows already in the model came from this xpub

        int window() const;   // max(floor, lastUsed + 1 + gap); may exceed what may be derived
        int target() const;   // window() capped at the end of the chain's indices
        int base() const;     // the start of the window derived whatever the row limit: max(floor, gap)
    };

    enum class SubState : quint8 {
        Queued, // waiting in subQueue_
        Sent,   // handed to the client, reply pending
        Retry,  // refused; waits in retry_ for the next ready() or retry round
        Live,   // subscribed; changes arrive as notifications
    };

    // One address this run follows, keyed by its lowercase scripthash.
    struct Tracked {
        SubState sub = SubState::Queued;
        bool confirmed = false; // status known since the last start()
        bool counted = false;   // in checked_: confirmed, and its row checked
        bool notified = false;  // a notification arrived while the subscribe reply was pending
        QString status;         // that status while confirmed: "" no history, else the lowercase hash
    };

    void adoptModelRows();
    void onClientReady();
    void onScripthashChanged(const QString& scripthash, const QString& status);
    void onModelCleared();
    void onSubscribeReply(quint64 generation, const QString& scripthash, int status, const QString& statusText,
                          const QString& error);
    void onBalanceReply(quint64 generation, quint64 seq, const QString& scripthash, bool ok,
                        qint64 confirmed, qint64 unconfirmed, const QString& error);
    bool droppedByClient(const QString& error) const; // the client's own "stopped", never a server's text

    void scheduleDerive();
    void deriveSlice();
    int adoptOrDerive(int chainIndex, int index); // 1 if it derived, 0 if it adopted a model row
    void track(const QString& scripthash);
    int rowLimit() const;                 // Bitcoin rows a scan follows at most, from the gaps
    qint64 reservedRows() const;          // rows the chains' base windows still have to derive
    bool mayDerive(const Chain& c) const; // its next row lies in its window and within the row limit

    void schedulePump();
    void pumpRequests();
    void sendSubscribe(const QString& scripthash);
    void queueBalance(const QString& scripthash);
    void sendBalance(const QString& scripthash);

    void armRetry();
    void requeueFailed();

    void applyStatus(const QString& scripthash, bool used);
    void applyBalance(const QString& scripthash, qint64 confirmed, qint64 unconfirmed);
    void noteUsed(int chainIndex, int index);
    void reportLimits();
    void markConfirmed(const QString& scripthash);
    void recount(const QString& scripthash); // checked_ after its status or row changed
    int findChain(chain::ScriptType type, quint32 account, quint32 change) const;

    bool busy() const;
    void checkIdle();
    void noteProgress();
    void emitProgress();
    // problem(), at most once per sentence and server wording, and at most
    // kMaxErrorsPerRun times per start(). `serverText` is cleaned up here.
    void report(Problem kind, const QString& message, const QString& serverText = QString());

    net::ElectrumClient* client_ = nullptr;
    WalletModel* model_ = nullptr;
    QList<AccountKey> accounts_;
    int receiveGap_ = 200;
    int changeGap_ = 50;
    bool running_ = false;

    QList<Chain> chains_;
    QHash<QString, Tracked> tracked_;    // every row followed since the last start()
    QStringList subQueue_;               // scripthashes to subscribe, in derivation order
    QStringList balanceQueue_;           // scripthashes whose balance to fetch (sent first)
    QSet<QString> balanceQueued_;        // what balanceQueue_ holds
    QHash<QString, quint64> balanceSeq_; // newest get_balance per scripthash
    QStringList retry_;                  // refused subscriptions, queued again on ready() / retry round
    QSet<QString> balanceRetry_;         // failed balance fetches, queued again on ready() / retry round
    QSet<QString> reportedErrors_;       // sentence + normalized server text already emitted this run
    quint64 generation_ = 0;             // bumped by start() / stop(); older replies are dropped
    quint64 nextBalanceSeq_ = 0;
    qint64 lastProgressMs_ = 0;
    qint64 lastRetryMs_ = 0;             // when the last retry round ran
    int retryDelayMs_ = 0;               // delay of the next retry round; 0 = the first delay
    int outstanding_ = 0;                // requests of any run the client has not answered yet
    int subSent_ = 0;                    // this run's subscriptions awaiting a reply
    int balanceSent_ = 0;                // this run's balance requests awaiting a reply
    int checked_ = 0;                    // tracked rows counted (Tracked::counted)
    bool deriveScheduled_ = false;
    bool pumpScheduled_ = false;
    bool retryArmed_ = false;            // a retry round of this generation is posted
    bool progressArmed_ = false;         // a throttled progress() is posted
    bool progressDirty_ = false;
    bool idleReported_ = false;          // the final progress() of this busy spell is out
    bool finished_ = false;              // scanFinished() already emitted for this start()
    bool limitReported_ = false;         // reportLimits() already spoke for this start()
};

} // namespace wallet
