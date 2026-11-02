#pragma once

#include "wallet/Types.h"

#include <QHash>
#include <QObject>
#include <QVector>

#include <functional>

namespace wallet {

// One wallet's share of the model's rows.
struct WalletCounts {
    int rows = 0;
    int used = 0;    // rows with history
    int funded = 0;  // rows with a balance
    int checked = 0;
};

// Every derived address of every wallet with its latest known state, plus totals
// per asset, overall and per wallet. Owned by the controller; the database fills it
// at the start, the scanners write, the pages read. GUI thread only.
class WalletModel : public QObject
{
    Q_OBJECT
public:
    explicit WalletModel(QObject* parent = nullptr);

    void clear(); // emits cleared() and totalsChanged()
    // Drops one wallet's rows. The rest keep their data but move up, so this
    // too emits cleared() (listeners read what is left) and totalsChanged().
    void removeWallet(quint32 wallet);
    // Drops the rows `keep` says no to; the rest keep their data but move up. Emits
    // rowsDropped() and totalsChanged() if any went, nothing otherwise. Unlike cleared(),
    // no scan has to start over for it.
    void dropRows(const std::function<bool(const AddressRow&)>& keep);
    int size() const { return rows_.size(); }
    const AddressRow& at(int row) const { return rows_.at(row); }

    // Adds a row (a new wallet/network/type/account/change/index) or replaces the
    // existing one, and returns its position. Emits rowsAppended() or
    // rowChanged(), then totalsChanged() if any balance changed.
    int upsert(const AddressRow& row);
    // Many rows at once, as the database hands them over: what is already here is
    // kept, the rest appended. One rowsAppended() and one totalsChanged().
    void addSaved(const QVector<AddressRow>& rows);

    int findByScripthash(Network network, const QString& scripthash) const; // -1 if none
    int find(quint32 wallet, Network network, chain::ScriptType type, quint32 account, quint32 change,
             quint32 index) const;

    chain::Amount total(Asset a) const;
    chain::Amount total(Asset a, quint32 wallet) const;
    int usedCount() const;   // rows with history
    int fundedCount() const; // rows with a balance
    int checkedCount() const;
    WalletCounts counts(quint32 wallet) const;
    int rowsOn(Network network) const;      // rows of one network
    int uncheckedOn(Network network) const; // those not checked
    int uncheckedOn(Network network, quint32 wallet) const; // those of one wallet
    int holders(Asset a) const;             // rows that hold some of it
    int holders(Asset a, quint32 wallet) const;
    // Coming in on a Bitcoin kind of network and in no block yet, in smallest units: it counts in the
    // totals, and can still be replaced.
    qint64 incoming(Network network) const;
    qint64 incoming(Network network, quint32 wallet) const;

signals:
    void rowsAppended(int first, int last);
    void rowChanged(int row);
    void totalsChanged();
    void cleared();
    void rowsDropped();

private:
    // A row's identity is its position in the HD tree on its network, never its address.
    struct RowKey {
        quint32 wallet = 0;
        quint32 account = 0;
        quint32 index = 0;
        quint16 network = 0;
        quint8 type = 0;
        quint8 change = 0;
        bool operator==(const RowKey& other) const = default;
    };
    friend size_t qHash(const RowKey& key, size_t seed)
    {
        return qHashMulti(seed, key.wallet, key.account, key.index, key.network, key.type, key.change);
    }
    static RowKey keyOf(const AddressRow& row);

    // --- implementation (WalletModel.cpp may add private members) ---
    void resetCounts();
    void addLookups(const AddressRow& row, int index);
    void dropLookups(const AddressRow& row, int index);
    void countRow(const AddressRow& row, int sign);
    // Moves the totals by what the row holds; true if it holds anything.
    bool addToTotals(const AddressRow& row);
    bool takeFromTotals(const AddressRow& row);

    QVector<AddressRow> rows_;
    QHash<Network, QHash<QString, int>> byScripthash_;
    QHash<RowKey, int> byKey_;
    QHash<Asset, chain::Amount> totals_;
    QHash<quint32, QHash<Asset, chain::Amount>> walletTotals_;
    int usedCount_ = 0;
    int fundedCount_ = 0;
    int checkedCount_ = 0;
    QHash<quint32, WalletCounts> walletCounts_;
    QHash<Network, int> rowsOn_;
    QHash<Network, int> uncheckedOn_;
    QHash<quint32, QHash<Network, int>> walletUnchecked_;
    QHash<Asset, int> holders_;
    QHash<quint32, QHash<Asset, int>> walletHolders_;
    QHash<Network, qint64> incoming_;
    QHash<quint32, QHash<Network, qint64>> walletIncoming_;
};

} // namespace wallet
