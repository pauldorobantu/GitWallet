#pragma once

#include "wallet/History.h"

#include <QAbstractTableModel>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QVector>

class QTimer;

namespace app {
class WalletController;
}

namespace wallet {
class UtxoHistory;
}

namespace app::ui {

// What a line of the history says happened, within what the page is narrowed to: one wallet's
// payment to another is Sent there and Received here, and over all wallets a move.
enum class TxKind { Received, Sent, Moved };
enum class TxState { Pending, Confirming, Confirmed, Unproven, ProofFailed, Replaced, Dropped };

// The history as lines, over every book: one per transaction, with what it did to the addresses
// the page is narrowed to. Everything is worked out here and kept short, so that a table of
// half a million lines can be sorted and searched; the text of a cell only when it is drawn.
class HistoryTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { ColDate, ColType, ColWallet, ColAmount, ColUsdNow, ColUsdThen, ColFee, ColState, ColLabel, ColTx, ColumnCount };
    enum Role {
        TxidRole = Qt::UserRole + 1, // the id in hex
        KindRole,                    // int(TxKind)
        StateRole,                   // int(TxState)
    };

    // What narrows the list. Empty or zero: everything.
    struct Filter {
        quint32 wallet = 0;
        QString scripthash;   // one address
        int network = -1;     // int(wallet::Network) of one network; -1: every one
        int kind = -1;        // int(TxKind)
        int state = -1;       // kStateActive and so on
        qint64 from = 0;      // seconds since the epoch
        qint64 until = 0;
        QString query;
        bool hideDust = false; // of the lines of everything, or of one wallet: one address was asked for
    };
    enum StateFilter { kStatePending, kStateConfirmed, kStateProofFailed, kStateGone };

    struct Line {
        wallet::HistoryBook* book = nullptr;
        int position = 0;
        qint64 received = 0;
        qint64 spent = 0;
        qint64 time = 0;
        TxKind kind = TxKind::Received;
        TxState state = TxState::Pending;
    };

    // One line as an export writes it.
    struct ExportLine {
        QString date, kind, wallet, amount, coin, usdNow, usdThen, priceThenAt, fee, state, height, label, note, txid, addresses;
    };

    explicit HistoryTableModel(WalletController* controller, QObject* parent = nullptr);

    void setFilter(const Filter& filter);
    const Filter& filter() const { return filter_; }
    int totalCount() const; // every line there is, whatever is narrowed
    int totalAtRebuild() const { return totalAtRebuild_; } // the same, as it was when the lines shown were put together
    // What the lines shown came to, of those proven in a block: what is pending, or on a server's
    // word, is not to be counted as paid.
    qint64 receivedShown() const { return receivedShown_; }
    qint64 spentShown() const { return spentShown_; }
    int dustHidden() const { return dustHidden_; } // what the dust filter alone kept out of the lines shown
    const Line* lineAt(int row) const;
    int rowOf(wallet::HistoryBook* book, const chain::Hash256& txid) const; // -1 if it is not shown
    // The line of a transaction as it stands now, under what the page is narrowed to.
    bool lineOf(wallet::HistoryBook* book, const chain::Hash256& txid, Line* out) const;
    // A page out of sight does not have its list kept up, until it is looked at.
    void setWatched(bool watched);
    void setWalletNamesForTest(const QHash<quint32, QString>& names) { labels_ = names; }

    QVector<Line> lines() const { return lines_; } // shared until the next change: costs nothing
    // As an export writes it, under what it was narrowed to when it began.
    ExportLine exportLine(const Line& line, const Filter& filter) const;
    QString stateText(const Line& line) const;
    QList<QString> walletsOf(const Line& line) const;
    int tipOf(const wallet::HistoryBook& book) const; // -1 while no chain stands
    qint64 feeShown(const Line& line) const; // what the wallets shown paid; -1: not theirs, or not known

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void sort(int column, Qt::SortOrder order) override;

    void rebuildNow();

signals:
    void aboutToRebuild();
    void rebuilt();
    void positionsAboutToMove(); // of a book: what a line points at is still there

private:
    void scheduleRebuild();
    void sortLines();
    bool makeLine(wallet::HistoryBook* book, const wallet::UtxoHistory* history, int position, int scope, int tip, const Filter& filter, Line* out) const;
    TxState stateOf(const wallet::TxRecord& tx, int tip, const wallet::UtxoHistory* history) const;
    bool matches(const Line& line, const wallet::HistoryBook& book, const wallet::TxRecord& tx, const QVector<wallet::TxEffect>& effects) const;
    bool isDust(const Line& line, const wallet::HistoryBook& book, qint64 limit) const;
    qint64 netOf(const Line& line) const { return line.received - line.spent; }
    double coinValue(const Line& line) const;
    double priceNow(const wallet::HistoryBook& book) const;
    QString walletText(const Line& line) const;
    QStringList walletNames(const Line& line, const Filter& filter, int atMost) const;
    QStringList addressesOf(const Line& line, const Filter& filter) const;
    void refreshLabels();
    void noteProgress();

    WalletController* controller_ = nullptr;
    QVector<Line> lines_;
    Filter filter_;
    QTimer* rebuild_ = nullptr;
    QHash<quint32, QString> labels_;
    int sortColumn_ = ColDate;
    Qt::SortOrder sortOrder_ = Qt::DescendingOrder;
    qint64 receivedShown_ = 0;
    qint64 spentShown_ = 0;
    int dustHidden_ = 0;
    int lastRebuildMs_ = 0;
    int tipsSeen_ = 0;
    int failedSeen_ = 0;
    int totalAtRebuild_ = 0;
    bool watched_ = false;
    bool stale_ = false;
};

} // namespace app::ui
