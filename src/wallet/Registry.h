#pragma once

#include "chain/Address.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

// What GitWallet knows about coins and networks. All of it is data (res/coins.json):
// a network of a family GitWallet already has code for is one entry there, and the
// scanners, the pages, the prices and the database follow from that.
namespace wallet {

// An asset or a network by its position in the registry. A position holds for one run
// only: what outlives the run (the database, wallet.dat) uses AssetInfo::key and
// NetworkInfo::id.
enum class Asset : quint16 {};
enum class Network : quint16 {};

enum class Family {
    Utxo, // addresses hold coins, read through Electrum servers
    Evm,  // accounts hold a balance per asset, read through JSON-RPC endpoints
};

struct AssetInfo {
    Asset asset{};
    Network network{};
    QString key;         // "ethereum/usdt"
    QString symbol;      // "USDT"
    QString name;        // "Tether USD"
    QString standard;    // "ERC-20"; empty for a network's own coin
    int decimals = 8;    // smallest-unit decimals
    int shownDecimals = 8; // fraction digits a figure shows
    bool dollar = false; // a dollar token: always shows its cents
    QString contract;    // the token's contract; empty for a network's own coin
    double maxSupply = 0.0; // whole coins an honest server can report at most; 0: not known
    QString defillamaId; // "coingecko:bitcoin", "ethereum:0xdAC1…"
    QString coingeckoId; // "bitcoin"
    QString coinbaseSymbol; // "BTC"; symbols collide, so it is only ever asked about a listed asset

    bool isToken() const { return !contract.isEmpty(); }
};

struct NetworkInfo {
    Network network{};
    QString id;   // "bitcoin"
    QString name; // "Bitcoin"
    Family family = Family::Utxo;
    QString genesis;         // Utxo: the first block's hash, which tells a server's chain
    quint64 chainId = 0;     // Evm: what eth_chainId must answer
    QString multicall;       // Evm: the Multicall3 contract
    chain::AddressFormat format;
    Asset coin{};            // the network's own coin
    QList<Asset> tokens;
    QList<Asset> assets;     // the coin, then the tokens
    QStringList servers;     // built-in: "host:port" for Utxo, https URLs for Evm
    QHash<QString, int> serverPace; // calls a second a built-in server keeps answering, as measured
    QString txUrl;           // explorer links, "{id}" stands for the transaction or address
    QString addressUrl;
};

// An account every seed adds: its script type, and for a Utxo type its network. An Evm
// account has no network of its own, as its addresses are the same on all of them.
struct CapturedAccount {
    chain::ScriptType type = chain::ScriptType::P2WPKH;
    Network network{};
};

// Before anything else asks: a file to read instead of the built-in list. For tests, on a
// throw-away --data-dir.
void useRegistryFile(const QString& path);
// Empty when the list loaded; otherwise what is wrong with it. Asked first in main(), on
// the GUI thread: from then on the registry only ever gets read, from any thread.
QString registryProblem();

const QList<NetworkInfo>& networks();
const NetworkInfo& networkInfo(Network network);
const NetworkInfo* findNetwork(const QString& id); // nullptr if there is none
const QList<AssetInfo>& assets();                  // display order: each network's coin, then its tokens
const AssetInfo& assetInfo(Asset asset);
const AssetInfo* findAsset(const QString& key);    // nullptr if there is none
const QList<CapturedAccount>& capturedAccounts();  // in the order they are stored and shown

// The first network of a family; nullptr if there is none.
const NetworkInfo* firstOf(Family family);

} // namespace wallet
