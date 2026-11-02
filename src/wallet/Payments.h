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

// What came in, each told of once. On a Bitcoin kind of network a payment is a transaction read because
// an address's status moved on from what its history here was complete up to, that spends nothing of
// the wallets': so whatever is read for the first time, of whatever age, is no news, and neither is a
// wallet's own change or a move between its addresses. A network with no history yet has only the
// balance to go by: more of an asset on a row that had answered, unless that is what it held before a
// source behind the others said less for a moment. Less than counts as dust goes untold while the lists
// hide it, unless the address was given out or has a label. GUI thread only.
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
        std::function<bool()> starting; // the rows on screen are saved ones, still being checked
        std::function<bool()> hideDust;
    };
    void setHooks(const Hooks& hooks) { hooks_ = hooks; }

    const QList<Payment>& all() const { return payments_; } // the oldest first
    int unread() const { return unread_; }
    void markAllRead();
    // What the database kept, which is not told of again.
    void addSaved(const QList<Payment>& saved);
    void forgetWallet(quint32 wallet);
    void clear();
    static void setPauseForTest(int ms);

signals:
    void recorded(const wallet::Payment& payment); // new, or another transaction took its place
    void readUpTo(qint64 id);
    void walletForgotten(quint32 wallet);
    void changed();
    // What came in over the last moment, together: many at once are one thing to say.
    void told(const QList<wallet::Payment>& payments);

private:
    void onRowMoved(int row, const AddressRow& before);
    void onAdded(HistoryBook* book, int position);
    bool isDust(Asset asset, const chain::Amount& amount, const QList<int>& rows) const;
    void record(Asset asset, const chain::Amount& amount, quint32 wallet, const QString& address, int addresses, const chain::Hash256* txid, bool whileClosed);
    void tellNow();

    struct Fall {
        chain::Amount from;
        qint64 at = 0;
    };

    WalletModel* model_ = nullptr;
    QList<HistoryBook*> books_;
    Hooks hooks_;
    QList<Payment> payments_;
    QList<Payment> toTell_;
    QSet<chain::Hash256> told_;
    QHash<Network, QHash<QString, bool>> expecting_; // addresses whose status moved on from a history complete here, and whether that was at the start
    QHash<QString, Fall> falls_;              // by asset and address
    QTimer* pause_ = nullptr;
    qint64 lastId_ = 0;
    int unread_ = 0;
};

} // namespace wallet
