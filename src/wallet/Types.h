#pragma once

#include "chain/Address.h"
#include "chain/Amount.h"
#include "chain/Bip32.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// Shared value types of the wallet layer.
namespace wallet {

enum class Asset { BTC, ETH, USDT, USDC };

struct AssetInfo {
    Asset asset = Asset::BTC;
    QString symbol;      // "BTC"
    int decimals = 8;    // smallest-unit decimals
    QString contract;    // ERC-20 contract address; empty for native coins
    QString coingeckoId; // "bitcoin"
};

const QList<AssetInfo>& assets(); // BTC, ETH, USDT, USDC — display order
const AssetInfo& assetInfo(Asset a);
QList<AssetInfo> erc20Assets();   // USDT, USDC

// One stored account: the public node at m/purpose'/coin'/account'.
struct AccountKey {
    quint32 wallet = 0; // WalletEntry::id of its wallet; the entry's position in the file says it
    chain::ScriptType type = chain::ScriptType::P2WPKH;
    quint32 account = 0;
    QString xpub; // serialized with chain::xpubVersionFor(type)

    // Parses `xpub`; throws std::runtime_error if it is not a valid extended key.
    chain::HdPubKey node() const;
};

// One wallet of the file: the accounts of a seed, or public keys added on their
// own. Either way only public keys are kept, so no wallet can spend from here.
struct WalletEntry {
    quint32 id = 0;        // stable and never 0; keys and rows refer to it
    QString label;         // the name the user gave it
    bool fromKeys = false; // added as public keys, without a seed's words
    QList<AccountKey> accounts;
};

// A wallet to add, as a batch asks for it. `refused` says why it was not added, if it
// was not.
struct WalletRequest {
    QList<AccountKey> accounts;
    QString label;         // empty: "Wallet N"
    bool fromKeys = false;
    int line = 0;          // the caller's: which line of its list this came from
    QString refused;
};

enum class AddressState { Unknown, Unused, Used, Funded };

// What a scanner's problem report is about, so the UI reacts to the kind and
// never has to parse the wording.
enum class Problem {
    Failure,        // a lookup failed and is retried; balances may be incomplete meanwhile
    Limit,          // a chain or account reached the row limit; later addresses are not scanned
    WrongChain,     // the Ethereum endpoint does not answer for mainnet (chain id, no Multicall3, ...)
    BuiltInServers, // none of the Electrum servers in Settings works; the built-in list is in use
    Skipped,        // an account or chain cannot be scanned at all
    FileSetAside,   // a wallet.dat that could not be read was kept under another name
};

struct AddressRow {
    quint32 wallet = 0; // WalletEntry::id
    chain::ScriptType type = chain::ScriptType::P2WPKH;
    quint32 account = 0;
    quint32 change = 0; // 0 receive, 1 change (Bitcoin); always 0 for Ethereum
    quint32 index = 0;
    QString path;       // "m/84'/0'/0'/0/5"
    QString address;
    QString scripthash; // Bitcoin: the Electrum scripthash; empty for Ethereum

    bool checked = false; // every lookup has answered: BTC the status, plus the balance
                          // of a used address; ETH the nonce and every balance
    bool used = false;    // BTC: has history; ETH: nonce > 0 or any balance > 0

    // Balances in smallest units.
    qint64 btcConfirmed = 0;
    qint64 btcUnconfirmed = 0; // negative while an outgoing tx is unconfirmed
    chain::Amount ethWei;
    QMap<Asset, chain::Amount> tokens; // USDT / USDC base units
    quint64 nonce = 0;                 // Ethereum: outgoing transaction count

    bool isBitcoin() const { return chain::isBitcoin(type); }
    // Funded if any balance > 0; else Used if it has history (checked or not);
    // else Unused once checked; else Unknown.
    AddressState state() const;
    // This row's balance in `a` (BTC: confirmed + unconfirmed, floored at 0;
    // zero for assets of the other chain).
    chain::Amount amount(Asset a) const;
};

struct ScanSettings {
    int btcReceiveGap = 200; // unused receive addresses checked past the last used one
    int btcChangeGap = 50;
    int ethGap = 100;
    int ethRefreshSeconds = 60;
    QStringList electrumServers; // "host:port"; empty = net::defaultElectrumServers()
    QStringList ethEndpoints;    // https URLs; empty = net::defaultEthEndpoints()
};

} // namespace wallet
