#pragma once

#include <QAbstractTableModel>

namespace app {
class WalletController;
}

namespace wallet {
struct Payment;
struct TxRecord;
}

namespace app::ui {

// What came in, the latest first: when, how much and what it was worth then, for whom, on which wallet
// and address, and how that payment stands now. What was not looked at yet reads bold. The bell's list
// has all of it, the Portfolio the latest few.
class PaymentsModel : public QAbstractTableModel
{
public:
    enum Column { ColWhen, ColAmount, ColUsd, ColLabel, ColWallet, ColAddress, ColState, ColumnCount };

    // `limit` above 0: at most that many.
    PaymentsModel(WalletController* controller, int limit, QObject* parent);

    // The latest first: a row's place moves with each that comes, so it is all told again.
    void refresh();
    // How a payment stands now, and who it is for, can move with nothing new having come.
    void refreshStates();
    void refreshLabels();
    const wallet::Payment* at(int row) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex& index, int role) const override;

private:
    const wallet::TxRecord* recordOf(const wallet::Payment& payment) const;
    bool proofFailed(const wallet::Payment& payment) const;
    QString stateKey(const wallet::Payment& payment) const;
    QString stateText(const wallet::Payment& payment) const;

    WalletController* controller_ = nullptr;
    int limit_ = 0;
    int shown_ = 0;
};

} // namespace app::ui
