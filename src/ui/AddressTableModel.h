#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QList>
#include <QString>

namespace app {
class WalletController;
}

namespace app::ui {

// Table adapter over wallet::WalletModel for the Addresses page. Follows the
// model's rowsAppended / rowChanged / cleared signals and the price feed.
class AddressTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { ColWallet, ColChain, ColType, ColPath, ColAddress, ColBalance, ColUsd, ColState, ColumnCount };
    enum Role {
        SortRole = Qt::UserRole + 1, // numeric value for balance / USD / path index columns
        IsBitcoinRole,               // bool
        StateRole,                   // int(wallet::AddressState)
        AddressRole,                 // the full address (copy)
        WalletRole,                  // wallet::AddressRow::wallet
    };

    explicit AddressTableModel(WalletController* controller, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    WalletController* controller_ = nullptr;

    // --- implementation (AddressTableModel.cpp) ---
    void appendRows();      // the wallet model added rows: announces them on the next event-loop turn
    void announceRows();    // announces every row the wallet model added since the last announcement
    void changeRow(int row);
    void resetRows();
    void refreshPrices();   // repaints Balance / USD when a price changed
    void refreshLabels();   // the wallets' names, read per cell and per sort comparison

    QHash<quint32, QString> labels_;
    int rows_ = 0;                // the row count the views know (moves only inside begin/endInsertRows)
    bool announcePosted_ = false; // an announceRows() waits for the next event-loop turn
    QList<double> prices_;        // the last prices seen, in wallet::assets() order
};

} // namespace app::ui
