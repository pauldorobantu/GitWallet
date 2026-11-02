#include "wallet/WalletModel.h"

#include <algorithm>
#include <utility>

namespace wallet {

WalletModel::WalletModel(QObject* parent)
    : QObject(parent)
{
}

WalletModel::RowKey WalletModel::keyOf(const AddressRow& row)
{
    RowKey key;
    key.wallet = row.wallet;
    key.account = row.account;
    key.index = row.index;
    key.network = quint16(row.network);
    key.type = quint8(row.type);
    key.change = quint8(row.change);
    return key;
}

void WalletModel::resetCounts()
{
    byScripthash_.clear();
    byKey_.clear();
    totals_.clear();
    walletTotals_.clear();
    usedCount_ = 0;
    fundedCount_ = 0;
    checkedCount_ = 0;
    walletCounts_.clear();
    rowsOn_.clear();
    uncheckedOn_.clear();
    walletUnchecked_.clear();
    holders_.clear();
    walletHolders_.clear();
    incoming_.clear();
    walletIncoming_.clear();
}

void WalletModel::clear()
{
    rows_.clear();
    resetCounts();
    emit cleared();
    emit totalsChanged();
}

void WalletModel::removeWallet(quint32 wallet)
{
    const QVector<AddressRow> before = std::exchange(rows_, {});
    resetCounts();
    rows_.reserve(before.size());
    for (const AddressRow& row : before) {
        if (row.wallet == wallet) continue;
        const int i = static_cast<int>(rows_.size());
        rows_.append(row);
        byKey_.insert(keyOf(row), i);
        addLookups(row, i);
        countRow(row, +1);
        addToTotals(row);
    }
    emit cleared();
    emit totalsChanged();
}

void WalletModel::dropRows(const std::function<bool(const AddressRow&)>& keep)
{
    if (std::all_of(rows_.cbegin(), rows_.cend(), keep))
        return;
    const QVector<AddressRow> before = std::exchange(rows_, {});
    resetCounts();
    rows_.reserve(before.size());
    for (const AddressRow& row : before) {
        if (!keep(row)) continue;
        const int i = static_cast<int>(rows_.size());
        rows_.append(row);
        byKey_.insert(keyOf(row), i);
        addLookups(row, i);
        countRow(row, +1);
        addToTotals(row);
    }
    emit rowsDropped();
    emit totalsChanged();
}

int WalletModel::upsert(const AddressRow& row)
{
    // Every member is brought up to date before the first signal, so a slot
    // that reads the model (or upserts again) sees a consistent state.
    const RowKey key = keyOf(row);
    const auto found = byKey_.constFind(key);
    if (found == byKey_.cend()) {
        const int i = static_cast<int>(rows_.size());
        rows_.append(row);
        byKey_.insert(key, i);
        addLookups(row, i);
        countRow(row, +1);
        // A new row moves each total by its own balance (an empty row has none).
        const bool balanceChanged = addToTotals(row);
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
    const bool balanceChanged =
        old.confirmed != now.confirmed || old.unconfirmed != now.unconfirmed || old.balances != now.balances;
    if (balanceChanged) {
        takeFromTotals(old);
        addToTotals(now);
    }
    emit rowChanged(i);
    if (balanceChanged) emit totalsChanged();
    return i;
}

void WalletModel::addSaved(const QVector<AddressRow>& rows)
{
    const int first = static_cast<int>(rows_.size());
    bool balanceChanged = false;
    QList<int> filledIn;
    for (const AddressRow& row : rows) {
        const RowKey key = keyOf(row);
        const auto found = byKey_.constFind(key);
        if (found != byKey_.cend()) {
            // What a scanner already wrote is newer, unless all it wrote is the address.
            const int at = found.value();
            const AddressRow& now = rows_.at(at);
            if (now.checked || now.used || now.confirmed != 0 || now.unconfirmed != 0 || !now.balances.isEmpty()) continue;
            dropLookups(now, at);
            countRow(now, -1);
            rows_[at] = row;
            addLookups(row, at);
            countRow(row, +1);
            balanceChanged = addToTotals(row) || balanceChanged;
            filledIn.append(at);
            continue;
        }
        const int i = static_cast<int>(rows_.size());
        rows_.append(row);
        byKey_.insert(key, i);
        addLookups(row, i);
        countRow(row, +1);
        balanceChanged = addToTotals(row) || balanceChanged;
    }
    const int last = static_cast<int>(rows_.size()) - 1;
    if (last >= first) emit rowsAppended(first, last);
    for (const int at : std::as_const(filledIn)) emit rowChanged(at);
    if (balanceChanged) emit totalsChanged();
}

int WalletModel::findByScripthash(Network network, const QString& scripthash) const
{
    if (scripthash.isEmpty()) return -1;
    const auto own = byScripthash_.constFind(network);
    // Hex, so lowercasing is harmless and tolerates a server that echoes uppercase.
    return own == byScripthash_.cend() ? -1 : own->value(scripthash.toLower(), -1);
}

int WalletModel::find(quint32 wallet, Network network, chain::ScriptType type, quint32 account, quint32 change,
                      quint32 index) const
{
    AddressRow probe;
    probe.wallet = wallet;
    probe.network = network;
    probe.type = type;
    probe.account = account;
    probe.change = change;
    probe.index = index;
    return byKey_.value(keyOf(probe), -1);
}

chain::Amount WalletModel::total(Asset a) const
{
    return totals_.value(a);
}

chain::Amount WalletModel::total(Asset a, quint32 wallet) const
{
    const auto own = walletTotals_.constFind(wallet);
    return own == walletTotals_.cend() ? chain::Amount() : own->value(a);
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

int WalletModel::rowsOn(Network network) const
{
    return rowsOn_.value(network);
}

int WalletModel::uncheckedOn(Network network) const
{
    return uncheckedOn_.value(network);
}

int WalletModel::uncheckedOn(Network network, quint32 wallet) const
{
    const auto own = walletUnchecked_.constFind(wallet);
    return own == walletUnchecked_.cend() ? 0 : own->value(network);
}

int WalletModel::holders(Asset a) const
{
    return holders_.value(a);
}

int WalletModel::holders(Asset a, quint32 wallet) const
{
    const auto own = walletHolders_.constFind(wallet);
    return own == walletHolders_.cend() ? 0 : own->value(a);
}

qint64 WalletModel::incoming(Network network) const
{
    return incoming_.value(network);
}

qint64 WalletModel::incoming(Network network, quint32 wallet) const
{
    const auto own = walletIncoming_.constFind(wallet);
    return own == walletIncoming_.cend() ? 0 : own->value(network);
}

void WalletModel::addLookups(const AddressRow& row, int index)
{
    // Account rows have no scripthash; an empty key would make every one of them collide.
    if (!row.scripthash.isEmpty()) byScripthash_[row.network].insert(row.scripthash.toLower(), index);
}

void WalletModel::dropLookups(const AddressRow& row, int index)
{
    // Only remove an entry that still points at this row, so a duplicate address
    // claimed by another row keeps its lookup.
    if (row.scripthash.isEmpty()) return;
    const auto own = byScripthash_.find(row.network);
    if (own == byScripthash_.end()) return;
    const auto it = own->find(row.scripthash.toLower());
    if (it != own->end() && it.value() == index) own->erase(it);
}

void WalletModel::countRow(const AddressRow& row, int sign)
{
    const bool funded = row.state() == AddressState::Funded;
    WalletCounts& own = walletCounts_[row.wallet];
    own.rows += sign;
    rowsOn_[row.network] += sign;
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
    } else {
        uncheckedOn_[row.network] += sign;
        walletUnchecked_[row.wallet][row.network] += sign;
    }
}

bool WalletModel::addToTotals(const AddressRow& row)
{
    if (row.isUtxo() && row.unconfirmed > 0) {
        incoming_[row.network] += row.unconfirmed;
        walletIncoming_[row.wallet][row.network] += row.unconfirmed;
    }
    bool holds = false;
    row.forEachBalance([&](Asset asset, const chain::Amount& amount) {
        holds = true;
        totals_[asset] += amount;
        walletTotals_[row.wallet][asset] += amount;
        ++holders_[asset];
        ++walletHolders_[row.wallet][asset];
    });
    return holds;
}

bool WalletModel::takeFromTotals(const AddressRow& row)
{
    if (row.isUtxo() && row.unconfirmed > 0) {
        incoming_[row.network] -= row.unconfirmed;
        walletIncoming_[row.wallet][row.network] -= row.unconfirmed;
    }
    bool holds = false;
    row.forEachBalance([&](Asset asset, const chain::Amount& amount) {
        holds = true;
        totals_[asset] -= amount;
        walletTotals_[row.wallet][asset] -= amount;
        --holders_[asset];
        --walletHolders_[row.wallet][asset];
    });
    return holds;
}

} // namespace wallet
