#include "wallet/Types.h"

#include <QtNumeric>

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace wallet {

namespace {

// confirmed + unconfirmed without signed overflow: both numbers come from an
// Electrum server, so they are not trusted. Overflow is only possible when both
// have the same sign, so it saturates in the direction of `confirmed`.
qint64 utxoNet(qint64 confirmed, qint64 unconfirmed)
{
    qint64 sum = 0;
    if (qAddOverflow(confirmed, unconfirmed, &sum))
        return confirmed > 0 ? std::numeric_limits<qint64>::max() : std::numeric_limits<qint64>::min();
    return sum;
}

} // namespace

bool GivenOut::contains(quint32 index) const
{
    const auto after = std::upper_bound(runs.cbegin(), runs.cend(), index, [](quint32 value, const Run& run) { return value < run.from; });
    return after != runs.cbegin() && index < std::prev(after)->to;
}

void GivenOut::add(quint32 index)
{
    if (contains(index))
        return;
    const auto after = std::upper_bound(runs.begin(), runs.end(), index, [](quint32 value, const Run& run) { return value < run.from; });
    const qsizetype at = after - runs.begin();
    const bool joinsBefore = at > 0 && runs.at(at - 1).to == index;
    const bool joinsAfter = at < runs.size() && runs.at(at).from == index + 1;
    if (joinsBefore && joinsAfter) {
        runs[at - 1].to = runs.at(at).to;
        runs.removeAt(at);
    } else if (joinsBefore) {
        runs[at - 1].to = index + 1;
    } else if (joinsAfter) {
        runs[at].from = index;
    } else {
        runs.insert(at, Run{index, index + 1});
    }
}

quint32 GivenOut::nextFree(quint32 index) const
{
    const auto after = std::upper_bound(runs.cbegin(), runs.cend(), index, [](quint32 value, const Run& run) { return value < run.from; });
    return after != runs.cbegin() && index < std::prev(after)->to ? std::prev(after)->to : index;
}

chain::HdPubKey AccountKey::node() const
{
    chain::HdPubKey key;
    std::string error;
    if (!chain::HdPubKey::parse(xpub.toStdString(), &key, nullptr, &error))
        throw std::runtime_error("invalid extended public key: " + error);
    return key;
}

bool AccountKey::serves(Network other) const
{
    if (chain::isUtxo(type))
        return network == other;
    return networkInfo(other).family == Family::Evm;
}

const chain::AddressFormat& AccountKey::format() const
{
    return chain::isUtxo(type) ? networkInfo(network).format : chain::defaultFormat(type);
}

QString AddressRow::path() const
{
    return QString::fromStdString(chain::accountPath(type, account, change, index, networkInfo(network).format));
}

AddressState AddressRow::state() const
{
    // A pending spend makes unconfirmed negative; only what is left counts.
    const bool funded = isUtxo() ? utxoNet(confirmed, unconfirmed) > 0 : !balances.isEmpty();
    if (funded) return AddressState::Funded;
    if (used) return AddressState::Used;
    if (checked) return AddressState::Unused;
    return AddressState::Unknown;
}

chain::Amount AddressRow::amount(Asset a) const
{
    if (isUtxo()) {
        if (a != networkInfo(network).coin) return {};
        const qint64 net = utxoNet(confirmed, unconfirmed);
        return net > 0 ? chain::Amount::fromU64(static_cast<quint64>(net)) : chain::Amount();
    }
    return balances.value(a);
}

void AddressRow::setBalance(Asset a, const chain::Amount& value)
{
    if (value.isZero())
        balances.remove(a);
    else
        balances.insert(a, value);
}

} // namespace wallet
