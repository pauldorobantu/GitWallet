#pragma once

#include <QAbstractTableModel>

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
    enum Column { ColChain, ColType, ColPath, ColAddress, ColBalance, ColUsd, ColState, ColumnCount };
    enum Role {
        SortRole = Qt::UserRole + 1, // numeric value for balance / USD / path index columns
        IsBitcoinRole,               // bool
        StateRole,                   // int(wallet::AddressState)
        AddressRole,                 // the full address (copy)
    };

    explicit AddressTableModel(WalletController* controller, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    WalletController* controller_ = nullptr;
};

} // namespace app::ui
