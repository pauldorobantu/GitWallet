#include "chain/Bip32.h"

#include "chain/Base58.h"

#include <secp256k1.h>

#include <stdexcept>

namespace chain {

namespace {

// One shared context. pubkey_create, pubkey_parse/serialize and the tweak_add
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

uint32_t read32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint32_t fingerprintOf(const Bytes& compressedPub)
{
    const Bytes h = hash160(compressedPub);
    return read32(h.data());
}

Bytes serializePub(const secp256k1_pubkey& pub, bool compressed)
{
    Bytes out(compressed ? 33 : 65);
    size_t len = out.size();
    secp256k1_ec_pubkey_serialize(ctx(), out.data(), &len, &pub,
                                  compressed ? SECP256K1_EC_COMPRESSED : SECP256K1_EC_UNCOMPRESSED);
    out.resize(len);
    return out;
}

// Scrubs a buffer of key material when it goes out of scope, so it is wiped on
// every way out of a function, the throws included.
struct ScrubOnExit
{
    Bytes& buf;
    ~ScrubOnExit() { secureClear(buf); }
};

} // namespace

// ---------------------------------------------------------------- HdKey ----

HdKey::HdKey(const HdKey& other)
    : priv_(other.priv_), chainCode_(other.chainCode_), depth_(other.depth_),
      parentFp_(other.parentFp_), childNumber_(other.childNumber_)
{
}

HdKey::HdKey(HdKey&& other) noexcept
    : priv_(std::move(other.priv_)), chainCode_(std::move(other.chainCode_)),
      depth_(other.depth_), parentFp_(other.parentFp_), childNumber_(other.childNumber_)
{
}

HdKey& HdKey::operator=(const HdKey& other)
{
    if (this != &other) {
        // Private key and chain code together are this node's xprv: wipe both
        // before their buffers are reused or freed.
        secureClear(priv_);
        secureClear(chainCode_);
        priv_ = other.priv_;
        chainCode_ = other.chainCode_;
        depth_ = other.depth_;
        parentFp_ = other.parentFp_;
        childNumber_ = other.childNumber_;
    }
    return *this;
}

HdKey& HdKey::operator=(HdKey&& other) noexcept
{
    if (this != &other) {
        // Moving in frees the old buffers as they are: wipe both first.
        secureClear(priv_);
        secureClear(chainCode_);
        priv_ = std::move(other.priv_);
        chainCode_ = std::move(other.chainCode_);
        depth_ = other.depth_;
        parentFp_ = other.parentFp_;
        childNumber_ = other.childNumber_;
    }
    return *this;
}

HdKey::~HdKey()
{
    secureClear(priv_);
    secureClear(chainCode_);
}

HdKey HdKey::fromSeed(const Bytes& seed)
{
    const std::string keyStr = "Bitcoin seed";
    const Bytes key(keyStr.begin(), keyStr.end());
    Bytes I = hmacSha512(key, seed); // the master xprv: IL = key, IR = chain code
    const ScrubOnExit wipeI{I};

    HdKey k;
    k.priv_.assign(I.begin(), I.begin() + 32);
    k.chainCode_.assign(I.begin() + 32, I.end());
    secureClear(I);

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
    return serializePub(pub, true);
}

Bytes HdKey::publicKeyUncompressed() const
{
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_create(ctx(), &pub, priv_.data()))
        throw std::runtime_error("pubkey_create failed");
    return serializePub(pub, false);
}

uint32_t HdKey::fingerprint() const
{
    return fingerprintOf(publicKeyCompressed());
}

HdPubKey HdKey::neuter() const
{
    return HdPubKey::fromParts(publicKeyCompressed(), chainCode_, depth_, parentFp_, childNumber_);
}

HdKey HdKey::deriveChild(uint32_t index, bool hardened) const
{
    const uint32_t i = hardened ? (index | 0x80000000u) : index;

    // 37 bytes on either path, reserved up front so the inserts below never
    // reallocate: growing frees the old block without scrubbing it, and on the
    // hardened path that block holds 0x00 || kpar.
    Bytes data;
    const ScrubOnExit wipeData{data};
    data.reserve(37);
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

    Bytes I = hmacSha512(chainCode_, data); // IL = tweak, IR = child chain code
    const ScrubOnExit wipeI{I};             // also on the throw below
    secureClear(data);

    HdKey child;
    child.priv_ = priv_; // child = (IL + kpar) mod n, computed in place
    if (!secp256k1_ec_seckey_tweak_add(ctx(), child.priv_.data(), I.data()))
        throw std::runtime_error("derived key invalid (try next index)");
    child.chainCode_.assign(I.begin() + 32, I.end());
    secureClear(I);
    child.depth_ = static_cast<uint8_t>(depth_ + 1);
    child.parentFp_ = fingerprint();
    child.childNumber_ = i;
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
        uint64_t index = 0;
        bool any = false;
        while (i < n && path[i] >= '0' && path[i] <= '9') {
            index = index * 10 + static_cast<uint64_t>(path[i] - '0');
            if (index >= 0x80000000ull) throw std::invalid_argument("path index too large: " + path);
            ++i;
            any = true;
        }
        if (!any) throw std::invalid_argument("bad derivation path: " + path);
        bool hardened = false;
        if (i < n && (path[i] == '\'' || path[i] == 'h' || path[i] == 'H')) {
            hardened = true;
            ++i;
        }
        node = node.deriveChild(static_cast<uint32_t>(index), hardened);
    }
    return node;
}

// ------------------------------------------------------------- HdPubKey ----

HdPubKey HdPubKey::fromParts(const Bytes& compressedPublicKey, const Bytes& chainCode,
                             uint8_t depth, uint32_t parentFingerprint, uint32_t childNumber)
{
    if (compressedPublicKey.size() != 33 || chainCode.size() != 32)
        throw std::invalid_argument("HdPubKey: need a 33-byte key and a 32-byte chain code");
    secp256k1_pubkey check;
    if (!secp256k1_ec_pubkey_parse(ctx(), &check, compressedPublicKey.data(), compressedPublicKey.size()))
        throw std::invalid_argument("HdPubKey: not a valid secp256k1 point");
    HdPubKey k;
    k.pub_ = compressedPublicKey;
    k.chainCode_ = chainCode;
    k.depth_ = depth;
    k.parentFp_ = parentFingerprint;
    k.childNumber_ = childNumber;
    return k;
}

bool HdPubKey::parse(const std::string& text, HdPubKey* out, uint32_t* versionOut, std::string* error)
{
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    Bytes raw;
    if (!base58::decodeCheck(text, &raw)) return fail("not valid Base58Check");
    if (raw.size() != 78) return fail("extended key must be 78 bytes");
    const uint32_t version = read32(raw.data());
    if (version != xpubVersion::kXpub && version != xpubVersion::kYpub && version != xpubVersion::kZpub)
        return fail("unsupported version (expected xpub, ypub or zpub)");
    const Bytes chain(raw.begin() + 13, raw.begin() + 45);
    const Bytes key(raw.begin() + 45, raw.end());
    if (key[0] != 0x02 && key[0] != 0x03) return fail("not a public extended key");
    try {
        *out = fromParts(key, chain, raw[4], read32(raw.data() + 5), read32(raw.data() + 9));
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    if (versionOut) *versionOut = version;
    return true;
}

HdPubKey HdPubKey::deriveChild(uint32_t index) const
{
    if (index & 0x80000000u) throw std::invalid_argument("hardened derivation needs the private key");
    if (!isValid()) throw std::logic_error("HdPubKey not initialised");

    Bytes data = pub_;
    uint8_t ib[4];
    ser32(index, ib);
    data.insert(data.end(), ib, ib + 4);
    const Bytes I = hmacSha512(chainCode_, data);

    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_parse(ctx(), &pub, pub_.data(), pub_.size()))
        throw std::runtime_error("pubkey_parse failed");
    // Ki = point(IL) + Kpar
    if (!secp256k1_ec_pubkey_tweak_add(ctx(), &pub, I.data()))
        throw std::runtime_error("derived key invalid (try next index)");

    HdPubKey child;
    child.pub_ = serializePub(pub, true);
    child.chainCode_.assign(I.begin() + 32, I.end());
    child.depth_ = static_cast<uint8_t>(depth_ + 1);
    child.parentFp_ = fingerprint();
    child.childNumber_ = index;
    return child;
}

Bytes HdPubKey::publicKeyUncompressed() const
{
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_parse(ctx(), &pub, pub_.data(), pub_.size()))
        throw std::runtime_error("pubkey_parse failed");
    return serializePub(pub, false);
}

uint32_t HdPubKey::fingerprint() const
{
    return fingerprintOf(pub_);
}

std::string HdPubKey::serialize(uint32_t version) const
{
    Bytes raw;
    raw.reserve(78);
    uint8_t b[4];
    ser32(version, b);
    raw.insert(raw.end(), b, b + 4);
    raw.push_back(depth_);
    ser32(parentFp_, b);
    raw.insert(raw.end(), b, b + 4);
    ser32(childNumber_, b);
    raw.insert(raw.end(), b, b + 4);
    raw.insert(raw.end(), chainCode_.begin(), chainCode_.end());
    raw.insert(raw.end(), pub_.begin(), pub_.end());
    return base58::encodeCheck(raw);
}

} // namespace chain
