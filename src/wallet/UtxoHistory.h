#pragma once

#include "wallet/ChainScanner.h"
#include "wallet/History.h"

#include <QDateTime>
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <deque>
#include <functional>
#include <memory>

class QTimer;

namespace net {
class ElectrumClient;
class PriceHistory;
}

namespace wallet {

class HeaderChain;
class WalletModel;

// One side of a transaction, as the detail of it shows.
struct TxSideLine {
    QString address;     // empty: a script without one
    qint64 value = -1;   // -1: not read
    bool ours = false;
};

// All there is to say of a transaction past what the book keeps. Read when asked for, and every
// byte under an id hashes to it: the transaction to its id, what it spends to the ids it names.
// The witness is under none of them, so the size a fee rate goes by is a server's word.
struct TxDetail {
    QString why;          // not empty: it could not be read, and this says why
    QVector<TxSideLine> inputs;
    QVector<TxSideLine> outputs;
    qint64 fee = -1;
    int size = 0;
    int vsize = 0;
    bool coinbase = false;
    int inputsLeftOut = 0; // too many to read; the fee is then the book's, if it has one
    int inputsUnread = 0;  // what made them could not be read from the server
};

// How far the history of one network has got, as the page says it.
struct HistoryProgress {
    int addressesLeft = 0;  // waiting for their list of transactions, or for one of those
    int transactionsLeft = 0;
    int proofsLeft = 0;     // in a block on a server's word so far
    int refused = 0;        // addresses no server would give the whole of
    int differing = 0;      // addresses whose transactions do not add up to their balance
    int unproven = 0;       // proofs that do not hold, or that nobody answers
    bool working() const { return addressesLeft > 0 || transactionsLeft > 0 || proofsLeft > 0; }
};

// Reads the history of every used address of one UTXO network over the connection its scanner
// holds. An address is read when its status is not the one its history here is complete up to:
// the server's list of its transactions, then each raw transaction it does not have, then for
// each in a block a Merkle proof against a header the chain vouches for. New payments go ahead
// of what is being caught up on. What a list says is believed only as far as it holds: it has to
// be what the server's own status says, each thing on it has to touch the address, and no list
// takes back what this PC has proven. A server that lies about any of it is left, as is one that
// keeps refusing, each server once. The pace is what public servers took without complaint
// (docs/ENDPOINTS.md), and less while a scanner reads rounds on the same connection. GUI thread only.
class UtxoHistory : public QObject
{
    Q_OBJECT
public:
    UtxoHistory(Network network, net::ElectrumClient* client, WalletModel* model, HistoryBook* book, QObject* parent = nullptr);
    ~UtxoHistory() override;

    void start();
    void stop();
    bool running() const { return running_; }
    bool connected() const; // to a server, this moment

    HistoryBook* book() const { return book_; }
    Network network() const { return network_; }
    int tip() const; // the height confirmations are counted from; -1 while no chain stands
    HistoryProgress progress() const; // nothing while stopped
    // A server has been asked for its proof, and what came back does not hold, or nothing did.
    bool proofFailed(const chain::Hash256& txid) const { return failedProof_.contains(txid); }

    // Whether the scanner sharing the connection reads rounds, which take most of what it can take.
    void setRoundsRunning(const std::function<bool()>& running) { roundsRunning_ = running; }
    // Whether that scanner has a balance of this address on its way: at a size where those queue for
    // a while, nothing can be said yet of whether it adds up.
    void setBalanceOnItsWay(const std::function<bool(const QString&)>& onItsWay) { balanceOnItsWay_ = onItsWay; }

    // The whole of one transaction, from the server. `done` runs once, with `detail.why` set if
    // it cannot be read; a guard makes it safe for what it captures to be gone by then.
    void readDetail(const chain::Hash256& txid, QObject* guard, const std::function<void(const TxDetail&)>& done);

    // For tests.
    void setProofsForTest(const ChainProofs& proofs);
    static void setFirstPauseForTest(int ms);
    static void setMaxPendingForTest(int count);
    net::PriceHistory* prices() const { return prices_; }

signals:
    void progressed(); // counts moved
    void problem(wallet::Problem kind, const QString& message, const QString& serverDetail);

private:
    // One address on its way to having a whole history.
    struct Reading {
        QString status; // what its list stands for
        QVector<chain::Hash256> listed;
        int waiting = 0; // raw transactions it waits for
        bool failed = false;
        bool news = false; // its status moved: it goes ahead of what is being caught up on
    };
    // A raw transaction on its way, and who waits for it.
    struct Wanted {
        int height = 0;
        QSet<QString> addresses;
        bool sent = false;
        bool news = false;
    };

    void consider(const QString& scripthash, const QString& status, bool first);
    void considerAll();
    void lookAgain(const QString& scripthash); // its row may have moved on while it was read
    void onRowChanged(int row);
    void onRowsAppended(int first, int last);
    void onRowsGone();
    void onClientReady();

    void schedulePump();
    void pump();
    bool takeToken();
    int callsPerTick() const;
    void sendHistory(const QString& scripthash, bool news);
    void onHistory(quint64 generation, const QString& scripthash, const QJsonValue& result, const QString& error);
    void want(const chain::Hash256& txid, int height, const QString& scripthash);
    void sendTransaction(const chain::Hash256& txid);
    void onTransaction(quint64 generation, const chain::Hash256& txid, const QJsonValue& result, const QString& error);
    void failWaiting(const Wanted& wanted);
    void finishIfDone(const QString& scripthash);
    void queueProofsOf(const QString& scripthash, bool news);
    void queueProof(int position, bool news = false);
    void queueAllProofs();
    void proveNext(const chain::Hash256& txid);
    void sendProof(const chain::Hash256& txid, int height);
    void onProof(quint64 generation, const chain::Hash256& txid, int height, const QJsonValue& result, const QString& error);
    void onHeaderReady(int height);
    void onHeaderMoved(int height);
    void onTipMoved();
    void onTipWaitOver();
    void onReorg(int fromHeight);
    void onChainMisbehaved(const QString& why);
    void onHeadersRefused();
    void giveUpWaitingProofs();
    void onBadProof(const chain::Hash256& txid, int height);
    void askServerOf(int height, const QString& lie);
    void noteDishonest(const QString& why);
    bool moveOn(const QString& why);
    enum Asked { AskedHistory, AskedTransaction, AskedProof, AskedKinds };
    void noteRefusal(Asked kind);
    void noteAnswered(Asked kind);
    void noteAllWell();
    void pauseFor(int ms);
    void askAgainAfterWait();
    void retryRefused();
    void compareBalance(const QString& scripthash);
    void compareSoon(const QString& scripthash);
    void compareTouching(int position);
    void compareNextPart();
    void askPrices();
    void onPrices(const QHash<qint64, QPair<double, qint64>>& answers, const QList<qint64>& asked, bool failed);
    void noteProgress();

    Network network_{};
    net::ElectrumClient* client_ = nullptr;
    WalletModel* model_ = nullptr;
    HistoryBook* book_ = nullptr;
    HeaderChain* chain_ = nullptr;
    net::PriceHistory* prices_ = nullptr;
    std::function<bool()> roundsRunning_;
    std::function<bool(const QString&)> balanceOnItsWay_;
    QTimer* tokens_ = nullptr;
    QTimer* compare_ = nullptr;
    QTimer* priceTimer_ = nullptr;
    QTimer* progressTimer_ = nullptr;
    QTimer* pause_ = nullptr;
    QTimer* again_ = nullptr;   // what was refused is asked once more in a while
    QTimer* tipWait_ = nullptr; // a block named ahead of the chain has to arrive
    QSet<QString> toCompare_;
    QSet<QString> comparing_;   // those whose time has come, a part per event-loop turn

    std::deque<QString> addressNews_;
    std::deque<QString> addressQueue_;
    QSet<QString> queued_;
    QHash<QString, QString> askedFor_;    // scripthash -> the row's status when it was last queued
    QHash<QString, Reading> reading_;
    QHash<chain::Hash256, Wanted> wanted_;
    std::deque<chain::Hash256> txQueue_;
    std::deque<chain::Hash256> proofQueue_;
    QSet<chain::Hash256> proofQueued_;
    QMultiHash<int, chain::Hash256> waitingHeader_;
    QSet<chain::Hash256> waitingTip_;
    QSet<chain::Hash256> refusedOnce_;     // still queued: asked once more in a while
    QSet<chain::Hash256> failedProof_;     // did not hold, or refused twice: asked again when the chain or server moves
    QSet<int> doubtedHeights_;             // being looked into: is the server wrong, or the chain behind
    QSet<QString> refusedAddresses_;       // no answer whole on this connection: asked again on the next
    struct Contradiction {
        QString status;     // what the status said then
        bool again = false; // and asked once more, in case its news was late
    };
    QHash<QString, Contradiction> contradicted_; // a list that was not what the status said
    QSet<QString> differing_;
    QSet<qint64> pricesAsked_;             // this run: an answer without a price is not asked for again
    QSet<qint64> pricesInFlight_;
    quint64 generation_ = 0;               // moves on with a start, a stop and a connection: late answers go by it
    int inFlight_ = 0;
    int tokensLeft_ = 0;
    int turn_ = 0;
    int refused_[AskedKinds] = {}; // of each kind, since one last answered: one that does not say much of another
    int pausedMs_ = 0;
    int priceBackoffMs_ = 0;
    int moves_ = 0;           // servers left for what a history needs, since all was well
    int headerRefusals_ = 0;  // tries the chain's asks were refused on, on this connection
    int tipAtWait_ = -1;
    bool running_ = false;
    bool paused_ = false;
    bool pumpPosted_ = false;
    bool saidDishonest_ = false;
    bool saidDiffering_ = false;
    bool retriedHere_ = false; // what was refused has had its second chance on this connection
    bool movingOn_ = false;
};

} // namespace wallet
