#include "chain/Bip32.h"

#include <secp256k1.h>

#include <cstring>
#include <stdexcept>

namespace chain {

namespace {

// One shared verify/sign-less context. pubkey_create and the tweak_add
// functions only read from it, so sharing it across calls is safe.
secp256k1_context* ctx()
{
    static secp256k1_context* c = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    return c;
}

void ser32(uint32_t i, uint8_t out[4])
{
    out[0] = static_cast<uint8_t>(i >> 24);
    out[1] = static_cast<uint8_t>(i >> 16);
    out[2] = static_cast<uint8_t>(i >> 8);
    out[3] = static_cast<uint8_t>(i);
}

} // namespace

HdKey HdKey::fromSeed(const Bytes& seed)
{
    const std::string keyStr = "Bitcoin seed";
    const Bytes key(keyStr.begin(), keyStr.end());
    const Bytes I = hmacSha512(key, seed);

    HdKey k;
    k.priv_.assign(I.begin(), I.begin() + 32);
    k.chainCode_.assign(I.begin() + 32, I.end());

    // The master private key must be a valid scalar.
    if (!secp256k1_ec_seckey_verify(ctx(), k.priv_.data()))
        throw std::runtime_error("invalid master key from seed");
    return k;
}

Bytes HdKey::publicKeyCompressed() const
{
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_create(ctx(), &pub, priv_.data()))
        throw std::runtime_error("pubkey_create failed");
    Bytes out(33);
    size_t len = out.size();
    secp256k1_ec_pubkey_serialize(ctx(), out.data(), &len, &pub, SECP256K1_EC_COMPRESSED);
    out.resize(len);
    return out;
}

Bytes HdKey::publicKeyUncompressed() const
{
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_create(ctx(), &pub, priv_.data()))
        throw std::runtime_error("pubkey_create failed");
    Bytes out(65);
    size_t len = out.size();
    secp256k1_ec_pubkey_serialize(ctx(), out.data(), &len, &pub, SECP256K1_EC_UNCOMPRESSED);
    out.resize(len);
    return out;
}

HdKey HdKey::deriveChild(uint32_t index, bool hardened) const
{
    const uint32_t i = hardened ? (index | 0x80000000u) : index;

    Bytes data;
    if (hardened) {
        // 0x00 || ser256(kpar) || ser32(i)
        data.push_back(0x00);
        data.insert(data.end(), priv_.begin(), priv_.end());
    } else {
        // serP(point(kpar)) || ser32(i)
        const Bytes pub = publicKeyCompressed();
        data.insert(data.end(), pub.begin(), pub.end());
    }
    uint8_t ib[4];
    ser32(i, ib);
    data.insert(data.end(), ib, ib + 4);

    const Bytes I = hmacSha512(chainCode_, data);
    const Bytes IL(I.begin(), I.begin() + 32);

    HdKey child;
    child.priv_ = priv_; // child = (IL + kpar) mod n, computed in place
    if (!secp256k1_ec_seckey_tweak_add(ctx(), child.priv_.data(), IL.data()))
        throw std::runtime_error("derived key invalid (try next index)");
    child.chainCode_.assign(I.begin() + 32, I.end());
    return child;
}

HdKey HdKey::derivePath(const std::string& path) const
{
    HdKey node = *this;
    size_t i = 0;
    const size_t n = path.size();
    // Skip an optional leading "m" / "M" and "/".
    if (i < n && (path[i] == 'm' || path[i] == 'M')) ++i;
    while (i < n) {
        if (path[i] == '/') { ++i; continue; }
        uint32_t index = 0;
        bool any = false;
        while (i < n && path[i] >= '0' && path[i] <= '9') {
            index = index * 10 + static_cast<uint32_t>(path[i] - '0');
            ++i;
            any = true;
        }
        if (!any) throw std::invalid_argument("bad derivation path: " + path);
        bool hardened = false;
        if (i < n && (path[i] == '\'' || path[i] == 'h' || path[i] == 'H')) {
            hardened = true;
            ++i;
        }
        node = node.deriveChild(index, hardened);
    }
    return node;
}

} // namespace chain
