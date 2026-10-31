#include "chain/Address.h"

#include "chain/Base58.h"
#include "chain/Bech32.h"

#include <array>
#include <stdexcept>

namespace chain {

namespace {

// purpose', coin' for each type.
void coinParams(ScriptType t, uint32_t& purpose, uint32_t& coin)
{
    switch (t) {
    case ScriptType::P2PKH:       purpose = 44; coin = 0;  break;
    case ScriptType::P2SH_P2WPKH: purpose = 49; coin = 0;  break;
    case ScriptType::P2WPKH:      purpose = 84; coin = 0;  break;
    case ScriptType::EthAccount:  purpose = 44; coin = 60; break;
    }
}

std::string reverseHex(const Bytes& b)
{
    Bytes r(b.rbegin(), b.rend());
    return toHex(r);
}

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

std::string accountPath(ScriptType t, uint32_t account, uint32_t change, uint32_t index)
{
    uint32_t purpose = 0, coin = 0;
    coinParams(t, purpose, coin);
    return "m/" + std::to_string(purpose) + "'/" + std::to_string(coin) + "'/"
           + std::to_string(account) + "'/" + std::to_string(change) + "/"
           + std::to_string(index);
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

AddressInfo addressAt(const HdKey& master, ScriptType t,
                      uint32_t account, uint32_t change, uint32_t index)
{
    AddressInfo info;
    info.path = accountPath(t, account, change, index);
    const HdKey node = master.derivePath(info.path);

    if (t == ScriptType::EthAccount) {
        Bytes pub = node.publicKeyUncompressed();     // 65 bytes, 0x04 prefix
        Bytes xy(pub.begin() + 1, pub.end());          // drop 0x04 -> 64 bytes
        Bytes h = keccak256(xy);
        Bytes addr20(h.end() - 20, h.end());
        info.address = ethChecksumAddress(addr20);
        return info;
    }

    // Bitcoin family.
    const Bytes pub = node.publicKeyCompressed();
    const Bytes h160 = hash160(pub);

    switch (t) {
    case ScriptType::P2PKH: {
        Bytes payload;
        payload.push_back(0x00); // mainnet P2PKH version
        payload.insert(payload.end(), h160.begin(), h160.end());
        info.address = base58::encodeCheck(payload);
        // scriptPubKey: OP_DUP OP_HASH160 <20> <h160> OP_EQUALVERIFY OP_CHECKSIG
        info.scriptPubKey = {0x76, 0xa9, 0x14};
        info.scriptPubKey.insert(info.scriptPubKey.end(), h160.begin(), h160.end());
        info.scriptPubKey.push_back(0x88);
        info.scriptPubKey.push_back(0xac);
        break;
    }
    case ScriptType::P2SH_P2WPKH: {
        // redeemScript = 0x00 0x14 <h160>; address hashes the redeemScript.
        Bytes redeem = {0x00, 0x14};
        redeem.insert(redeem.end(), h160.begin(), h160.end());
        const Bytes rh = hash160(redeem);
        Bytes payload;
        payload.push_back(0x05); // mainnet P2SH version
        payload.insert(payload.end(), rh.begin(), rh.end());
        info.address = base58::encodeCheck(payload);
        // scriptPubKey: OP_HASH160 <20> <rh> OP_EQUAL
        info.scriptPubKey = {0xa9, 0x14};
        info.scriptPubKey.insert(info.scriptPubKey.end(), rh.begin(), rh.end());
        info.scriptPubKey.push_back(0x87);
        break;
    }
    case ScriptType::P2WPKH: {
        info.address = bech32::segwitEncode("bc", 0, h160);
        // scriptPubKey: OP_0 <20> <h160>
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

} // namespace chain
