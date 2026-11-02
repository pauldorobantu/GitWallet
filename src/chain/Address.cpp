#include "chain/Address.h"

#include "chain/Base58.h"
#include "chain/Bech32.h"

#include <stdexcept>

namespace chain {

namespace {

// BIP43 purpose of each type.
uint32_t purposeOf(ScriptType t)
{
    switch (t) {
    case ScriptType::P2PKH:       return 44;
    case ScriptType::P2SH_P2WPKH: return 49;
    case ScriptType::P2WPKH:      return 84;
    case ScriptType::EthAccount:  return 44;
    }
    return 44;
}

std::string reverseHex(const Bytes& b)
{
    Bytes r(b.rbegin(), b.rend());
    return toHex(r);
}

constexpr ScriptType kAllTypes[] = {ScriptType::P2PKH, ScriptType::P2SH_P2WPKH, ScriptType::P2WPKH,
                                    ScriptType::EthAccount};

} // namespace

const char* scriptTypeName(ScriptType t)
{
    switch (t) {
    case ScriptType::P2PKH:       return "Legacy (P2PKH)";
    case ScriptType::P2SH_P2WPKH: return "Nested SegWit (P2SH)";
    case ScriptType::P2WPKH:      return "Native SegWit (bech32)";
    case ScriptType::EthAccount:  return "Ethereum";
    }
    return "?";
}

const char* scriptTypeId(ScriptType t)
{
    switch (t) {
    case ScriptType::P2PKH:       return "p2pkh";
    case ScriptType::P2SH_P2WPKH: return "p2sh-p2wpkh";
    case ScriptType::P2WPKH:      return "p2wpkh";
    case ScriptType::EthAccount:  return "ethereum";
    }
    return "unknown";
}

bool scriptTypeFromId(const std::string& id, ScriptType* out)
{
    for (const ScriptType t : kAllTypes) {
        if (id == scriptTypeId(t)) {
            *out = t;
            return true;
        }
    }
    return false;
}

bool isUtxo(ScriptType t)
{
    return t != ScriptType::EthAccount;
}

const AddressFormat& defaultFormat(ScriptType t)
{
    static const AddressFormat bitcoin;
    static const AddressFormat ethereum{60, 0, 0, std::string()};
    return isUtxo(t) ? bitcoin : ethereum;
}

uint32_t xpubVersionFor(ScriptType t)
{
    switch (t) {
    case ScriptType::P2WPKH:      return xpubVersion::kZpub;
    case ScriptType::P2SH_P2WPKH: return xpubVersion::kYpub;
    default:                      return xpubVersion::kXpub;
    }
}

std::string accountRootPath(ScriptType t, uint32_t account, const AddressFormat& format)
{
    return "m/" + std::to_string(purposeOf(t)) + "'/" + std::to_string(format.coinType) + "'/"
           + std::to_string(account) + "'";
}

std::string accountRootPath(ScriptType t, uint32_t account)
{
    return accountRootPath(t, account, defaultFormat(t));
}

std::string accountPath(ScriptType t, uint32_t account, uint32_t change, uint32_t index,
                        const AddressFormat& format)
{
    return accountRootPath(t, account, format) + "/" + std::to_string(change) + "/" + std::to_string(index);
}

std::string accountPath(ScriptType t, uint32_t account, uint32_t change, uint32_t index)
{
    return accountPath(t, account, change, index, defaultFormat(t));
}

std::string ethChecksumAddress(const Bytes& addr20)
{
    std::string lower = toHex(addr20); // 40 lowercase hex chars
    const Bytes hash = keccak256(Bytes(lower.begin(), lower.end()));
    std::string out = "0x";
    for (size_t i = 0; i < lower.size(); ++i) {
        char c = lower[i];
        if (c >= 'a' && c <= 'f') {
            const uint8_t nibble = (hash[i / 2] >> (4 * (1 - (i % 2)))) & 0xF;
            if (nibble >= 8) c = static_cast<char>(c - 'a' + 'A');
        }
        out.push_back(c);
    }
    return out;
}

AddressInfo addressFromPublicKey(ScriptType t, const Bytes& compressedPub, const Bytes& uncompressedPub,
                                 const AddressFormat& format)
{
    AddressInfo info;

    if (t == ScriptType::EthAccount) {
        if (uncompressedPub.size() != 65) throw std::invalid_argument("ETH needs a 65-byte public key");
        const Bytes xy(uncompressedPub.begin() + 1, uncompressedPub.end()); // drop 0x04
        const Bytes h = keccak256(xy);
        const Bytes addr20(h.end() - 20, h.end());
        info.address = ethChecksumAddress(addr20);
        return info;
    }

    if (compressedPub.size() != 33) throw std::invalid_argument("BTC needs a 33-byte public key");
    const Bytes h160 = hash160(compressedPub);

    switch (t) {
    case ScriptType::P2PKH: {
        Bytes payload;
        payload.push_back(format.p2pkhVersion);
        payload.insert(payload.end(), h160.begin(), h160.end());
        info.address = base58::encodeCheck(payload);
        // OP_DUP OP_HASH160 <20> <h160> OP_EQUALVERIFY OP_CHECKSIG
        info.scriptPubKey = {0x76, 0xa9, 0x14};
        info.scriptPubKey.insert(info.scriptPubKey.end(), h160.begin(), h160.end());
        info.scriptPubKey.push_back(0x88);
        info.scriptPubKey.push_back(0xac);
        break;
    }
    case ScriptType::P2SH_P2WPKH: {
        // redeemScript = OP_0 <20> <h160>; the address hashes the redeemScript.
        Bytes redeem = {0x00, 0x14};
        redeem.insert(redeem.end(), h160.begin(), h160.end());
        const Bytes rh = hash160(redeem);
        Bytes payload;
        payload.push_back(format.p2shVersion);
        payload.insert(payload.end(), rh.begin(), rh.end());
        info.address = base58::encodeCheck(payload);
        // OP_HASH160 <20> <rh> OP_EQUAL
        info.scriptPubKey = {0xa9, 0x14};
        info.scriptPubKey.insert(info.scriptPubKey.end(), rh.begin(), rh.end());
        info.scriptPubKey.push_back(0x87);
        break;
    }
    case ScriptType::P2WPKH: {
        info.address = bech32::segwitEncode(format.hrp, 0, h160);
        // OP_0 <20> <h160>
        info.scriptPubKey = {0x00, 0x14};
        info.scriptPubKey.insert(info.scriptPubKey.end(), h160.begin(), h160.end());
        break;
    }
    case ScriptType::EthAccount:
        break; // handled above
    }

    // Electrum subscribes by sha256(scriptPubKey), reversed, as hex.
    info.electrumScripthash = reverseHex(sha256(info.scriptPubKey));
    return info;
}

AddressInfo addressFromPublicKey(ScriptType t, const Bytes& compressedPub, const Bytes& uncompressedPub)
{
    return addressFromPublicKey(t, compressedPub, uncompressedPub, defaultFormat(t));
}

AddressInfo addressAt(const HdKey& master, ScriptType t,
                      uint32_t account, uint32_t change, uint32_t index)
{
    const std::string path = accountPath(t, account, change, index);
    const HdKey node = master.derivePath(path);
    AddressInfo info = (t == ScriptType::EthAccount)
        ? addressFromPublicKey(t, {}, node.publicKeyUncompressed())
        : addressFromPublicKey(t, node.publicKeyCompressed(), {});
    info.path = path;
    return info;
}

AddressInfo addressFromChainNode(const HdPubKey& chainNode, ScriptType t,
                                 uint32_t account, uint32_t change, uint32_t index,
                                 const AddressFormat& format)
{
    const HdPubKey node = chainNode.deriveChild(index);
    AddressInfo info = (t == ScriptType::EthAccount)
        ? addressFromPublicKey(t, {}, node.publicKeyUncompressed(), format)
        : addressFromPublicKey(t, node.publicKey(), {}, format);
    info.path = accountPath(t, account, change, index, format);
    return info;
}

AddressInfo addressFromChainNode(const HdPubKey& chainNode, ScriptType t,
                                 uint32_t account, uint32_t change, uint32_t index)
{
    return addressFromChainNode(chainNode, t, account, change, index, defaultFormat(t));
}

AddressInfo addressFromAccount(const HdPubKey& accountNode, ScriptType t,
                               uint32_t account, uint32_t change, uint32_t index)
{
    return addressFromChainNode(accountNode.deriveChild(change), t, account, change, index);
}

} // namespace chain
