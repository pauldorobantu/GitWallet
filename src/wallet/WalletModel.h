#pragma once

#include "wallet/Types.h"

#include <QHash>
#include <QObject>
#include <QVector>

namespace wallet {

// One wallet's share of the model's rows.
struct WalletCounts {
    int rows = 0;
    int used = 0;    // rows with history
    int funded = 0;  // rows with a balance
    int checked = 0;
};

// Every derived address of every wallet with its latest scan result, plus totals
// per asset, overall and per wallet. Owned by the controller; the scanners write,
// the pages read. GUI thread only.
class WalletModel : public QObject
{
    Q_OBJECT
public:
    explicit WalletModel(QObject* parent = nullptr);

    void clear(); // emits cleared() and totalsChanged()
    // Drops one wallet's rows. The rest keep their data but move up, so this
    // too emits cleared() (listeners read what is left) and totalsChanged().
    void removeWallet(quint32 wallet);
    int size() const { return rows_.size(); }
    const AddressRow& at(int row) const { return rows_.at(row); }

    // Adds a row (a new wallet/type/account/change/index) or replaces the
    // existing one, and returns its position. Emits rowsAppended() or
    // rowChanged(), then totalsChanged() if any balance changed.
    int upsert(const AddressRow& row);

    int findByScripthash(const QString& scripthash) const; // -1 if none
    int findByAddress(const QString& address) const;       // case-insensitive; -1 if none
    int find(quint32 wallet, chain::ScriptType type, quint32 account, quint32 change, quint32 index) const;

    chain::Amount total(Asset a) const;
    chain::Amount total(Asset a, quint32 wallet) const;
    int usedCount() const;   // rows with history
    int fundedCount() const; // rows with a balance
    int checkedCount() const;
    WalletCounts counts(quint32 wallet) const;

signals:
    void rowsAppended(int first, int last);
    void rowChanged(int row);
    void totalsChanged();
    void cleared();

private:
    // --- implementation (WalletModel.cpp may add private members) ---
    void addLookups(const AddressRow& row, int index);
    void dropLookups(const AddressRow& row, int index);
    void countRow(const AddressRow& row, int sign);
    // Drops the cached total of every asset whose balance differs between the
    // two versions of a row; true if any did.
    bool dropChangedTotals(const AddressRow& before, const AddressRow& after);

    QVector<AddressRow> rows_;
    QHash<QString, int> byScripthash_;
    QHash<QString, int> byAddress_; // lowercase address -> row
    QHash<QString, int> byKey_;     // "wallet/type/account/change/index" -> row
    // Sums computed by total(); an asset is missing until it is asked for
    // again after one of its balances changed. Amount cannot subtract, so a
    // changed row invalidates instead of adjusting.
    mutable QMap<Asset, chain::Amount> totals_;
    mutable QHash<quint32, QMap<Asset, chain::Amount>> walletTotals_;
    int usedCount_ = 0;
    int fundedCount_ = 0;
    int checkedCount_ = 0;
    QHash<quint32, WalletCounts> walletCounts_;
};

} // namespace wallet
