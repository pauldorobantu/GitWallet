#include "wallet/Registry.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <cmath>

namespace wallet {

namespace {

constexpr int kMaxDecimals = 36;

QString& registryFile()
{
    static QString path; // from --coins-file; GUI thread, before the first read
    return path;
}

struct Registry {
    QList<NetworkInfo> networks;
    QList<AssetInfo> assets;
    QList<CapturedAccount> captured;
    QHash<QString, int> networkById;
    QHash<QString, int> assetByKey;
    QString problem;
};

bool isContract(const QString& text)
{
    static const QRegularExpression shape(QStringLiteral("^0x[0-9A-Fa-f]{40}$"));
    return shape.match(text).hasMatch();
}

bool isElectrumServer(const QString& text)
{
    static const QRegularExpression shape(QStringLiteral("^[A-Za-z0-9.-]+:[0-9]{1,5}$"));
    return shape.match(text).hasMatch();
}

bool isHttpsUrl(const QString& text)
{
    const QUrl url(text, QUrl::StrictMode);
    return url.isValid() && url.scheme() == QLatin1String("https") && !url.host().isEmpty();
}

bool readBits(const QJsonValue& value, uint32_t* out)
{
    bool ok = false;
    const uint value32 = value.toString().toUInt(&ok, 16);
    *out = value32;
    return ok && value.toString().size() == 8;
}

// Empty when `o` says how a UTXO network's headers are checked, otherwise why not.
QString readProofs(const QJsonObject& o, ChainProofs* out)
{
    if (!readBits(o.value(QLatin1String("powLimit")), &out->powLimit))
        return QStringLiteral("no usable easiest target");
    out->retargetBlocks = o.value(QLatin1String("retargetBlocks")).toInt(-1);
    out->targetSeconds = uint32_t(o.value(QLatin1String("targetSeconds")).toInt(0));
    if (out->retargetBlocks < 0 || (out->retargetBlocks > 0 && out->targetSeconds == 0))
        return QStringLiteral("no usable difficulty rule");
    const QJsonObject checkpoint = o.value(QLatin1String("checkpoint")).toObject();
    out->checkpointHeight = checkpoint.value(QLatin1String("height")).toInt(-1);
    out->periodStart = uint32_t(checkpoint.value(QLatin1String("periodStart")).toDouble(0));
    readBits(checkpoint.value(QLatin1String("nextBits")), &out->nextBits); // for a test, if it is there
    out->checkpointTime = uint32_t(checkpoint.value(QLatin1String("time")).toDouble(0));
    if (out->checkpointHeight < 0
        || !chain::hashFromHex(checkpoint.value(QLatin1String("hash")).toString().toStdString(), &out->checkpointHash)
        || !chain::hashFromHex(checkpoint.value(QLatin1String("headersRoot")).toString().toStdString(), &out->headersRoot)
        || !readBits(checkpoint.value(QLatin1String("bits")), &out->checkpointBits))
        return QStringLiteral("no usable checkpoint");
    if (out->retargetBlocks > 0 && ((out->checkpointHeight + 1) % out->retargetBlocks != 0 || out->periodStart == 0 || out->checkpointTime == 0))
        return QStringLiteral("a checkpoint that does not end a period");
    return {};
}

// One coin or token of `network`. Empty when it is usable, otherwise why not.
QString readAsset(const QJsonObject& o, const NetworkInfo& network, bool token, AssetInfo* out)
{
    const QString id = o.value(QLatin1String("id")).toString();
    if (id.isEmpty())
        return QStringLiteral("%1 has an asset without an id").arg(network.id);
    out->network = network.network;
    out->key = network.id + QLatin1Char('/') + id;
    out->symbol = o.value(QLatin1String("symbol")).toString();
    out->name = o.value(QLatin1String("name")).toString();
    out->standard = o.value(QLatin1String("standard")).toString();
    out->decimals = o.value(QLatin1String("decimals")).toInt(-1);
    out->shownDecimals = o.value(QLatin1String("shown")).toInt(-1);
    out->dollar = o.value(QLatin1String("dollar")).toBool();
    out->contract = o.value(QLatin1String("contract")).toString();
    out->maxSupply = o.value(QLatin1String("maxSupply")).toDouble();
    const QJsonObject prices = o.value(QLatin1String("prices")).toObject();
    out->defillamaId = prices.value(QLatin1String("defillama")).toString();
    out->coingeckoId = prices.value(QLatin1String("coingecko")).toString();
    out->coinbaseSymbol = prices.value(QLatin1String("coinbase")).toString();

    if (out->symbol.isEmpty() || out->name.isEmpty())
        return QStringLiteral("%1 has no symbol or name").arg(out->key);
    if (out->decimals < 0 || out->decimals > kMaxDecimals)
        return QStringLiteral("%1 has no usable decimals").arg(out->key);
    if (out->shownDecimals < 0 || out->shownDecimals > out->decimals)
        return QStringLiteral("%1 shows more digits than it has").arg(out->key);
    if (!std::isfinite(out->maxSupply) || out->maxSupply < 0.0)
        return QStringLiteral("%1 has no usable supply").arg(out->key);
    if (token != !out->contract.isEmpty())
        return QStringLiteral("%1: a token has a contract, and a coin has none").arg(out->key);
    if (token && !isContract(out->contract))
        return QStringLiteral("%1 has no usable contract").arg(out->key);
    return {};
}

// Empty when `o` is a usable network, otherwise why not. Its assets join `r`.
QString readNetwork(const QJsonObject& o, Registry* r)
{
    NetworkInfo n;
    n.network = Network(quint16(r->networks.size()));
    n.id = o.value(QLatin1String("id")).toString();
    n.name = o.value(QLatin1String("name")).toString();
    if (n.id.isEmpty() || n.id.contains(QLatin1Char('/')) || n.name.isEmpty())
        return QStringLiteral("a network has no usable id or name");
    if (r->networkById.contains(n.id))
        return QStringLiteral("%1 is listed twice").arg(n.id);

    const QString family = o.value(QLatin1String("family")).toString();
    if (family == QLatin1String("utxo"))
        n.family = Family::Utxo;
    else if (family == QLatin1String("evm"))
        n.family = Family::Evm;
    else
        return QStringLiteral("%1 is of an unknown family").arg(n.id);

    const QJsonObject address = o.value(QLatin1String("address")).toObject();
    const int coinType = address.value(QLatin1String("coinType")).toInt(-1);
    if (coinType < 0)
        return QStringLiteral("%1 has no coin type").arg(n.id);
    n.format.coinType = uint32_t(coinType);
    if (n.family == Family::Utxo) {
        const int p2pkh = address.value(QLatin1String("p2pkh")).toInt(-1);
        const int p2sh = address.value(QLatin1String("p2sh")).toInt(-1);
        const QString hrp = address.value(QLatin1String("hrp")).toString();
        if (p2pkh < 0 || p2pkh > 255 || p2sh < 0 || p2sh > 255 || hrp.isEmpty())
            return QStringLiteral("%1 has no usable address format").arg(n.id);
        n.format.p2pkhVersion = uint8_t(p2pkh);
        n.format.p2shVersion = uint8_t(p2sh);
        n.format.hrp = hrp.toStdString();
        n.genesis = o.value(QLatin1String("genesis")).toString();
        static const QRegularExpression hash(QStringLiteral("^[0-9a-f]{64}$"));
        if (!hash.match(n.genesis).hasMatch())
            return QStringLiteral("%1 has no usable genesis hash").arg(n.id);
        const QString proofs = readProofs(o.value(QLatin1String("proofs")).toObject(), &n.proofs);
        if (!proofs.isEmpty())
            return QStringLiteral("%1 has %2").arg(n.id, proofs);
    } else {
        const double chainId = o.value(QLatin1String("chainId")).toDouble(-1.0);
        n.multicall = o.value(QLatin1String("multicall3")).toString();
        if (chainId < 1.0 || chainId != std::floor(chainId))
            return QStringLiteral("%1 has no chain id").arg(n.id);
        if (!isContract(n.multicall))
            return QStringLiteral("%1 has no Multicall3 contract").arg(n.id);
        if (n.format.coinType != chain::defaultFormat(chain::ScriptType::EthAccount).coinType)
            return QStringLiteral("%1: every Evm network shares one account key").arg(n.id);
        n.chainId = quint64(chainId);
    }

    const QJsonArray servers = o.value(QLatin1String("servers")).toArray();
    for (const QJsonValue& value : servers) {
        // Its address, or that with what was measured of it.
        const QJsonObject described = value.toObject();
        const QString server = value.isString() ? value.toString() : described.value(QLatin1String("url")).toString();
        const bool usable = n.family == Family::Utxo ? isElectrumServer(server) : isHttpsUrl(server);
        if (!usable)
            return QStringLiteral("%1 lists an unusable server").arg(n.id);
        n.servers.append(server);
        const int pace = described.value(QLatin1String("callsPerSecond")).toInt();
        if (pace > 0)
            n.serverPace.insert(server, pace);
    }
    if (n.servers.isEmpty())
        return QStringLiteral("%1 has no servers").arg(n.id);

    const QJsonObject explorer = o.value(QLatin1String("explorer")).toObject();
    n.txUrl = explorer.value(QLatin1String("tx")).toString();
    n.addressUrl = explorer.value(QLatin1String("address")).toString();
    if (!n.txUrl.contains(QLatin1String("{id}")) || !n.addressUrl.contains(QLatin1String("{id}")))
        return QStringLiteral("%1 has no usable explorer links").arg(n.id);

    const auto addAsset = [&](const QJsonObject& item, bool token, Asset* made) -> QString {
        AssetInfo info;
        QString why = readAsset(item, n, token, &info);
        if (!why.isEmpty())
            return why;
        if (r->assetByKey.contains(info.key))
            return QStringLiteral("%1 is listed twice").arg(info.key);
        info.asset = Asset(quint16(r->assets.size()));
        *made = info.asset;
        r->assetByKey.insert(info.key, int(r->assets.size()));
        r->assets.append(info);
        n.assets.append(info.asset);
        return {};
    };

    QString why = addAsset(o.value(QLatin1String("coin")).toObject(), false, &n.coin);
    if (!why.isEmpty())
        return why;
    const QJsonArray tokens = o.value(QLatin1String("tokens")).toArray();
    if (n.family == Family::Utxo && !tokens.isEmpty())
        return QStringLiteral("%1 cannot hold tokens").arg(n.id);
    for (const QJsonValue& value : tokens) {
        Asset token{};
        why = addAsset(value.toObject(), true, &token);
        if (!why.isEmpty())
            return why;
        n.tokens.append(token);
    }

    r->networkById.insert(n.id, int(r->networks.size()));
    r->networks.append(n);
    return {};
}

Registry load()
{
    Registry r;
    const QString path = registryFile().isEmpty() ? QStringLiteral(":/coins.json") : registryFile();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        r.problem = QStringLiteral("The list of coins could not be read.");
        return r;
    }
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        r.problem = QStringLiteral("The list of coins is not valid JSON.");
        return r;
    }
    const QJsonObject root = doc.object();
    const QJsonArray networks = root.value(QLatin1String("networks")).toArray();
    for (const QJsonValue& value : networks) {
        r.problem = readNetwork(value.toObject(), &r);
        if (!r.problem.isEmpty())
            return r;
    }
    if (r.networks.isEmpty()) {
        r.problem = QStringLiteral("The list of coins names no network.");
        return r;
    }

    const QJsonArray accounts = root.value(QLatin1String("accounts")).toArray();
    for (const QJsonValue& value : accounts) {
        const QJsonObject o = value.toObject();
        CapturedAccount account;
        if (!chain::scriptTypeFromId(o.value(QLatin1String("type")).toString().toStdString(), &account.type)) {
            r.problem = QStringLiteral("The list of coins names an unknown account type.");
            return r;
        }
        const QString network = o.value(QLatin1String("network")).toString();
        if (chain::isUtxo(account.type)) {
            const auto found = r.networkById.constFind(network);
            if (found == r.networkById.cend() || r.networks.at(*found).family != Family::Utxo) {
                r.problem = QStringLiteral("The list of coins has an account without its network.");
                return r;
            }
            account.network = r.networks.at(*found).network;
        }
        r.captured.append(account);
    }
    if (r.captured.isEmpty())
        r.problem = QStringLiteral("The list of coins names no account to keep.");
    return r;
}

const Registry& registry()
{
    static const Registry r = load();
    return r;
}

} // namespace

void useRegistryFile(const QString& path)
{
    registryFile() = path;
}

QString registryProblem()
{
    return registry().problem;
}

const QList<NetworkInfo>& networks()
{
    return registry().networks;
}

const NetworkInfo& networkInfo(Network network)
{
    const QList<NetworkInfo>& list = registry().networks;
    const qsizetype i = qsizetype(network);
    // Only a position from this registry ever gets here; a bad one is the first entry's.
    return list.at(i < list.size() ? i : 0);
}

const NetworkInfo* findNetwork(const QString& id)
{
    const auto found = registry().networkById.constFind(id);
    return found == registry().networkById.cend() ? nullptr : &registry().networks.at(*found);
}

const QList<AssetInfo>& assets()
{
    return registry().assets;
}

const AssetInfo& assetInfo(Asset asset)
{
    const QList<AssetInfo>& list = registry().assets;
    const qsizetype i = qsizetype(asset);
    return list.at(i < list.size() ? i : 0);
}

const AssetInfo* findAsset(const QString& key)
{
    const auto found = registry().assetByKey.constFind(key);
    return found == registry().assetByKey.cend() ? nullptr : &registry().assets.at(*found);
}

const QList<CapturedAccount>& capturedAccounts()
{
    return registry().captured;
}

const NetworkInfo* firstOf(Family family)
{
    for (const NetworkInfo& n : registry().networks) {
        if (n.family == family)
            return &n;
    }
    return nullptr;
}

} // namespace wallet
