#include "wallet/WalletModel.h"

#include <utility>

namespace wallet {

namespace {

// A row's identity is its position in the HD tree, never its address.
QString keyOf(quint32 wallet, chain::ScriptType type, quint32 account, quint32 change, quint32 index)
{
    return QString::number(wallet) + QLatin1Char('/') + QString::number(static_cast<int>(type))
           + QLatin1Char('/') + QString::number(account) + QLatin1Char('/') + QString::number(change)
           + QLatin1Char('/') + QString::number(index);
}

} // namespace

WalletModel::WalletModel(QObject* parent)
    : QObject(parent)
{
}

void WalletModel::clear()
{
    rows_.clear();
    byScripthash_.clear();
    byAddress_.clear();
    byKey_.clear();
    totals_.clear();
    walletTotals_.clear();
    usedCount_ = 0;
    fundedCount_ = 0;
    checkedCount_ = 0;
    walletCounts_.clear();
    emit cleared();
    emit totalsChanged();
}

void WalletModel::removeWallet(quint32 wallet)
{
    const QVector<AddressRow> before = std::exchange(rows_, {});
    byScripthash_.clear();
    byAddress_.clear();
    byKey_.clear();
    totals_.clear();
    walletTotals_.clear();
    usedCount_ = 0;
    fundedCount_ = 0;
    checkedCount_ = 0;
    walletCounts_.clear();
    rows_.reserve(before.size());
    for (const AddressRow& row : before) {
        if (row.wallet == wallet) continue;
        const int i = static_cast<int>(rows_.size());
        rows_.append(row);
        byKey_.insert(keyOf(row.wallet, row.type, row.account, row.change, row.index), i);
        addLookups(row, i);
        countRow(row, +1);
    }
    emit cleared();
    emit totalsChanged();
}

int WalletModel::upsert(const AddressRow& row)
{
    // Every member is brought up to date before the first signal, so a slot
    // that reads the model (or upserts again) sees a consistent state.
    const QString key = keyOf(row.wallet, row.type, row.account, row.change, row.index);
    const auto found = byKey_.constFind(key);
    if (found == byKey_.cend()) {
        const int i = static_cast<int>(rows_.size());
        rows_.append(row);
        byKey_.insert(key, i);
        addLookups(row, i);
        countRow(row, +1);
        // A new row moves each total by its own balance (an empty row has none).
        const bool balanceChanged = dropChangedTotals(AddressRow(), row);
        emit rowsAppended(i, i);
        if (balanceChanged) emit totalsChanged();
        return i;
    }

    const int i = found.value();
    const AddressRow old = rows_.at(i); // a copy: `row` may be a reference to rows_[i]
    rows_[i] = row;
    const AddressRow& now = rows_.at(i);
    // Dropping and re-adding also covers a row whose address or scripthash changed.
    dropLookups(old, i);
    addLookups(now, i);
    countRow(old, -1);
    countRow(now, +1);
    const bool balanceChanged = dropChangedTotals(old, now);
    emit rowChanged(i);
    if (balanceChanged) emit totalsChanged();
    return i;
}

int WalletModel::findByScripthash(const QString& scripthash) const
{
    // Hex, so lowercasing is harmless and tolerates a server that echoes uppercase.
    return scripthash.isEmpty() ? -1 : byScripthash_.value(scripthash.toLower(), -1);
}

int WalletModel::findByAddress(const QString& address) const
{
    // Ethereum addresses arrive in EIP-55 mixed case or all lowercase.
    return address.isEmpty() ? -1 : byAddress_.value(address.toLower(), -1);
}

int WalletModel::find(quint32 wallet, chain::ScriptType type, quint32 account, quint32 change,
                      quint32 index) const
{
    return byKey_.value(keyOf(wallet, type, account, change, index), -1);
}

chain::Amount WalletModel::total(Asset a) const
{
    const auto cached = totals_.constFind(a);
    if (cached != totals_.cend()) return cached.value();

    chain::Amount sum;
    for (const AddressRow& r : rows_) {
        const chain::Amount v = r.amount(a);
        if (!v.isZero()) sum += v; // most rows of a long gap scan are empty
    }
    totals_.insert(a, sum);
    return sum;
}

chain::Amount WalletModel::total(Asset a, quint32 wallet) const
{
    QMap<Asset, chain::Amount>& cache = walletTotals_[wallet];
    const auto cached = cache.constFind(a);
    if (cached != cache.cend()) return cached.value();

    chain::Amount sum;
    for (const AddressRow& r : rows_) {
        if (r.wallet != wallet) continue;
        const chain::Amount v = r.amount(a);
        if (!v.isZero()) sum += v;
    }
    cache.insert(a, sum);
    return sum;
}

WalletCounts WalletModel::counts(quint32 wallet) const
{
    return walletCounts_.value(wallet);
}

int WalletModel::usedCount() const
{
    return usedCount_;
}

int WalletModel::fundedCount() const
{
    return fundedCount_;
}

int WalletModel::checkedCount() const
{
    return checkedCount_;
}

void WalletModel::addLookups(const AddressRow& row, int index)
{
    // Ethereum rows have no scripthash; an empty key would make every one of
    // them collide.
    if (!row.scripthash.isEmpty()) byScripthash_.insert(row.scripthash.toLower(), index);
    if (!row.address.isEmpty()) byAddress_.insert(row.address.toLower(), index);
}

void WalletModel::dropLookups(const AddressRow& row, int index)
{
    // Only remove entries that still point at this row, so a duplicate address
    // claimed by another row keeps its lookup.
    const auto drop = [index](QHash<QString, int>& map, const QString& key) {
        if (key.isEmpty()) return;
        const auto it = map.find(key.toLower());
        if (it != map.end() && it.value() == index) map.erase(it);
    };
    drop(byScripthash_, row.scripthash);
    drop(byAddress_, row.address);
}

void WalletModel::countRow(const AddressRow& row, int sign)
{
    const bool funded = row.state() == AddressState::Funded;
    WalletCounts& own = walletCounts_[row.wallet];
    own.rows += sign;
    // A balance implies history even if a scanner left `used` unset.
    if (row.used || funded) {
        usedCount_ += sign;
        own.used += sign;
    }
    if (funded) {
        fundedCount_ += sign;
        own.funded += sign;
    }
    if (row.checked) {
        checkedCount_ += sign;
        own.checked += sign;
    }
}

bool WalletModel::dropChangedTotals(const AddressRow& before, const AddressRow& after)
{
    bool changed = false;
    for (const AssetInfo& info : assets()) {
        if (before.amount(info.asset) != after.amount(info.asset)) {
            totals_.remove(info.asset);
            const auto own = walletTotals_.find(after.wallet);
            if (own != walletTotals_.end()) own->remove(info.asset);
            changed = true;
        }
    }
    return changed;
}

} // namespace wallet
