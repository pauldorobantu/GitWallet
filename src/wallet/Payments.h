#pragma once

#include "chain/Amount.h"
#include "wallet/Types.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

class QTimer;

namespace wallet {

class HistoryBook;
class WalletModel;

// A payment that came in, as it stood when it was noticed.
struct Payment {
    qint64 id = 0;            // in the order they came, over every start
    qint64 at = 0;            // when it was noticed, ms since the epoch
    QString asset;            // AssetInfo::key: a position holds for one run only
    chain::Amount amount;
    double usd = 0.0;         // at the price then; 0: none was known
    quint32 wallet = 0;
    QString address;
    int addresses = 1;        // how many it paid, this one first
    chain::Hash256 txid{};    // what paid it, when there is a history to say so
    bool hasTx = false;
    bool read = false;
    bool whileClosed = false; // it came before this run had its first answers
};

// What became of payments told of before: all that came to light over one moment.
struct PaymentNews {
    QList<Payment> inABlock;    // proven in one
    QList<Payment> gone;        // replaced or dropped for a while now: not paid
    QList<Payment> listedAgain; // said to be gone, and a server lists it again
    QList<Payment> leftABlock;  // said to be in a block, and that block gave way
    QList<Payment> noProof;     // a block was claimed for it, and the proof does not hold
    QList<Payment> wentBack;    // a balance that went up went back down to what it was
    bool isEmpty() const { return inABlock.isEmpty() && gone.isEmpty() && listedAgain.isEmpty() && leftABlock.isEmpty() && noProof.isEmpty() && wentBack.isEmpty(); }
    bool worrying() const { return !gone.isEmpty() || !leftABlock.isEmpty() || !noProof.isEmpty() || !wentBack.isEmpty(); }
    void add(const PaymentNews& more);
};

// What came in, each told of once. On a Bitcoin kind of network a payment is a transaction read because
// an address's status moved on from what its history here was complete up to, that spends nothing of
// the wallets': so whatever is read for the first time, of whatever age, is no news, and neither is a
// wallet's own change or a move between its addresses. A row that first answers after its wallet's first
// whole scan counts too, for what is newer than that scan. A network with no history yet has only the
// balance to go by: more of an asset on a row that had answered, unless that is what it held before a
// source behind the others said less for a moment. Less than counts as dust goes untold while the lists
// hide it, unless the address was given out or has a label. Every payment told of is watched until it is
// deep enough, and what becomes of it is told of too, whichever way that goes. GUI thread only.
class PaymentBook : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMaxKept = 5000;

    PaymentBook(WalletModel* model, const QList<HistoryBook*>& books, QObject* parent = nullptr);

    struct Hooks {
        std::function<double(Asset)> price;
        std::function<bool(const AddressRow&)> givenOut;
        std::function<bool(const QString&)> labelled;
        std::function<bool(const AddressRow&)> starting; // this row's saved figures are still being checked
        std::function<bool()> hideDust;
        std::function<int(Network)> tip; // -1: no chain stands
        std::function<bool(Network, const chain::Hash256&)> proofFailed;
        std::function<qint64(Network)> fallWindowMs; // by when a figure that fell has been read again
    };
    void setHooks(const Hooks& hooks) { hooks_ = hooks; }
    void setProofFailedForTest(const std::function<bool(Network, const chain::Hash256&)>& failed) { hooks_.proofFailed = failed; }

    const QList<Payment>& all() const { return payments_; } // the oldest first
    int unread() const { return unread_; }
    void markAllRead();
    void markReadUpTo(qint64 id); // what was on screen, with the rest still to be looked at
    qint64 lastId() const { return lastId_; }
    // What the database kept, which is not told of again.
    void addSaved(const QList<Payment>& saved);
    void restoreState(const QHash<QString, QString>& state);
    // Once the books are in: what was told of last time and still has to turn out one way or the other.
    void watchSaved();
    // A network has answered for all it follows, these wallets and these assets of it included.
    void noteAnswered(Network network, int tip, const QList<quint32>& wallets, const QStringList& assets);
    // Rows that come after this are no window grown by what was paid, until the next noteAnswered().
    void noteGapsChanged();
    void flushState();
    void forgetWallet(quint32 wallet);
    void clear();
    static void setPauseForTest(int ms);

signals:
    void recorded(const wallet::Payment& payment); // new, or another transaction took its place
    void removed(qint64 id);
    void readUpTo(qint64 id);
    void walletForgotten(quint32 wallet);
    void changed();
    // What came in over the last moment, together: many at once are one thing to say.
    void told(const QList<wallet::Payment>& payments);
    // What became of those told of before.
    void settled(const wallet::PaymentNews& news);
    // What has to be known at the next start: an empty value takes it away.
    void stateToSave(const QHash<QString, QString>& changes);

private:
    // Where an address is expected to have news, and of what.
    struct Expected {
        bool whileClosed = false;
        int above = 0; // a first look at a row: only what is newer than this height is news
    };
    enum class Said : quint8 { Coming, InABlock, Gone };
    struct Watched {
        qint64 payment = 0;
        Said said = Said::Coming;
        qint64 goneSince = 0;   // ms since the epoch; 0: not gone
        qint64 leftSince = 0;   // back in no block, after it was said to be in one
        qint64 failedSince = 0; // the proof of its block does not hold
        bool noProofSaid = false;
    };
    struct Fall {
        chain::Amount from;
        qint64 at = 0;
    };
    struct Gain {
        chain::Amount before;
        qint64 payment = 0;
        qint64 at = 0;
    };

    void onRowMoved(int row, const AddressRow& before);
    void onRowsAppended(int first, int last);
    void onAdded(HistoryBook* book, int position);
    void onSpendFound(HistoryBook* book, int position);
    void onStatusSet(HistoryBook* book, const QString& scripthash);
    void expect(Network network, const QString& scripthash, const Expected& expected);
    void stopExpecting(Network network, const QString& scripthash);
    void saveLater(const QString& key, const QString& value);
    bool isDust(Asset asset, const chain::Amount& amount, const QList<int>& rows) const;
    void record(Asset asset, const chain::Amount& amount, quint32 wallet, const QString& address, int addresses, const chain::Hash256* txid, bool whileClosed);
    void tellNow();
    void lookAtWaiting();
    HistoryBook* bookOf(Network network) const;
    const Payment* findPayment(qint64 id) const;
    qint64 fallWindow(Network network) const;

    WalletModel* model_ = nullptr;
    QList<HistoryBook*> books_;
    Hooks hooks_;
    QList<Payment> payments_;
    QList<Payment> toTell_;
    QSet<chain::Hash256> told_;
    QHash<Network, QHash<QString, Expected>> expecting_; // addresses whose status moved on from a history complete here
    QHash<Network, QHash<quint32, int>> firstScan_; // the tip when a wallet's first whole scan ended
    QHash<Network, QSet<QString>> assetsRead_;      // the assets a network's rows have been read with
    QSet<QString> grown_;                     // Ethereum's rows the window reached this run, after the first scan
    bool gapsChanged_ = false;
    QHash<QString, Fall> falls_;              // by asset and address
    QHash<QString, Gain> gains_;              // by asset and address: told of, not long ago
    QHash<chain::Hash256, Watched> waiting_;  // told of, and not settled yet
    PaymentNews held_;                        // what was noticed outside the look at the watch
    QHash<QString, QString> stateChanges_;
    QTimer* pause_ = nullptr;
    QTimer* lookAgain_ = nullptr;
    QTimer* stateSave_ = nullptr;
    qint64 pauseSince_ = 0;
    qint64 lastId_ = 0;
    int unread_ = 0;
};

} // namespace wallet
