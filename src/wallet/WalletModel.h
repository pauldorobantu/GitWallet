#pragma once

#include "wallet/Types.h"

#include <QHash>
#include <QObject>
#include <QVector>

namespace wallet {

// Every derived address with its latest scan result, plus per-asset totals.
// Owned by the controller; the scanners write, the pages read. GUI thread only.
class WalletModel : public QObject
{
    Q_OBJECT
public:
    explicit WalletModel(QObject* parent = nullptr);

    void clear(); // emits cleared() and totalsChanged()
    int size() const { return rows_.size(); }
    const AddressRow& at(int row) const { return rows_.at(row); }

    // Adds a row (a new type/account/change/index) or replaces the existing one,
    // and returns its position. Emits rowsAppended() or rowChanged(), then
    // totalsChanged() if any balance changed.
    int upsert(const AddressRow& row);

    int findByScripthash(const QString& scripthash) const; // -1 if none
    int findByAddress(const QString& address) const;       // case-insensitive; -1 if none
    int find(chain::ScriptType type, quint32 account, quint32 change, quint32 index) const;

    chain::Amount total(Asset a) const;
    int usedCount() const;   // rows with history
    int fundedCount() const; // rows with a balance
    int checkedCount() const;

signals:
    void rowsAppended(int first, int last);
    void rowChanged(int row);
    void totalsChanged();
    void cleared();

private:
    // --- implementation (WalletModel.cpp may add private members) ---
    QVector<AddressRow> rows_;
    QHash<QString, int> byScripthash_;
    QHash<QString, int> byAddress_; // lowercase address -> row
    QHash<QString, int> byKey_;     // "type/account/change/index" -> row
};

} // namespace wallet
