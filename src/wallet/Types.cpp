#include "wallet/Types.h"

#include <QtNumeric>

#include <limits>
#include <stdexcept>
#include <string>

namespace wallet {

namespace {

// confirmed + unconfirmed without signed overflow: both numbers come from an
// Electrum server, so they are not trusted. Overflow is only possible when both
// have the same sign, so it saturates in the direction of `confirmed`.
qint64 btcNet(qint64 confirmed, qint64 unconfirmed)
{
    qint64 sum = 0;
    if (qAddOverflow(confirmed, unconfirmed, &sum))
        return confirmed > 0 ? std::numeric_limits<qint64>::max() : std::numeric_limits<qint64>::min();
    return sum;
}

// ERC-20 assets are the ones with a contract; a stray BTC / ETH key in
// AddressRow::tokens is ignored so state(), amount() and the totals agree.
bool isToken(Asset a)
{
    return !assetInfo(a).contract.isEmpty();
}

} // namespace

const QList<AssetInfo>& assets()
{
    // USDT and USDC both answered decimals() == 6 on mainnet (eth_call,
    // 2026-10-31); the contract addresses are the canonical ones.
    static const QList<AssetInfo> list = {
        {Asset::BTC, QStringLiteral("BTC"), 8, QString(), QStringLiteral("bitcoin")},
        {Asset::ETH, QStringLiteral("ETH"), 18, QString(), QStringLiteral("ethereum")},
        {Asset::USDT, QStringLiteral("USDT"), 6,
         QStringLiteral("0xdAC17F958D2ee523a2206206994597C13D831ec7"), QStringLiteral("tether")},
        {Asset::USDC, QStringLiteral("USDC"), 6,
         QStringLiteral("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"), QStringLiteral("usd-coin")},
    };
    return list;
}

const AssetInfo& assetInfo(Asset a)
{
    const QList<AssetInfo>& list = assets();
    for (const AssetInfo& info : list)
        if (info.asset == a) return info;
    // Every enumerator is in the list; this only guards a value cast from a bad int.
    return list.constFirst();
}

QList<AssetInfo> erc20Assets()
{
    QList<AssetInfo> out;
    for (const AssetInfo& info : assets())
        if (!info.contract.isEmpty()) out.append(info);
    return out;
}

chain::HdPubKey AccountKey::node() const
{
    chain::HdPubKey key;
    std::string error;
    if (!chain::HdPubKey::parse(xpub.toStdString(), &key, nullptr, &error))
        throw std::runtime_error("invalid extended public key: " + error);
    return key;
}

AddressState AddressRow::state() const
{
    bool funded = false;
    if (isBitcoin()) {
        // A pending spend makes unconfirmed negative; only what is left counts.
        funded = btcNet(btcConfirmed, btcUnconfirmed) > 0;
    } else {
        funded = !ethWei.isZero();
        for (auto it = tokens.cbegin(); !funded && it != tokens.cend(); ++it)
            funded = isToken(it.key()) && !it.value().isZero();
    }
    if (funded) return AddressState::Funded;
    if (used) return AddressState::Used;
    if (checked) return AddressState::Unused;
    return AddressState::Unknown;
}

chain::Amount AddressRow::amount(Asset a) const
{
    if (isBitcoin()) {
        if (a != Asset::BTC) return {};
        const qint64 net = btcNet(btcConfirmed, btcUnconfirmed);
        return net > 0 ? chain::Amount::fromU64(static_cast<quint64>(net)) : chain::Amount();
    }
    if (a == Asset::ETH) return ethWei;
    return isToken(a) ? tokens.value(a) : chain::Amount();
}

} // namespace wallet
