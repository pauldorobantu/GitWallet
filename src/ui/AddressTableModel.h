#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QList>
#include <QString>

#include "wallet/Types.h"

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
    enum Column { ColWallet, ColChain, ColType, ColPath, ColAddress, ColLabel, ColBalance, ColUsd, ColState, ColumnCount };
    enum Role {
        SortRole = Qt::UserRole + 1, // numeric value for balance / USD / path index columns
        NetworkRole,                 // int(wallet::Network)
        StateRole,                   // int(wallet::AddressState)
        AddressRole,                 // the full address (copy)
        WalletRole,                  // wallet::AddressRow::wallet
        GivenOutRole,                // handed to someone, with no history yet
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
    bool waitingForPayment(const wallet::AddressRow& row) const; // given out, with no history yet

    // A key by what tells it apart, cheap to hash: a lookup per row when the list sorts or filters by state.
    struct KeyId {
        quint32 wallet = 0;
        quint32 account = 0;
        quint8 type = 0;
        quint16 network = 0; // of a Bitcoin kind of key; an account's address is the same on all networks
        bool operator==(const KeyId& other) const = default;
    };
    friend size_t qHash(const KeyId& key, size_t seed)
    {
        return qHashMulti(seed, key.wallet, key.account, key.type, key.network);
    }
    mutable QHash<KeyId, wallet::GivenOut> given_; // read from the store once something was given out
    mutable bool givenStale_ = true;

    QHash<quint32, QString> labels_;
    int rows_ = 0;                // the row count the views know (moves only inside begin/endInsertRows)
    bool announcePosted_ = false; // an announceRows() waits for the next event-loop turn
    QList<double> prices_;        // the last prices seen, in wallet::assets() order
};

} // namespace app::ui
