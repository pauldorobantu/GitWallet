#pragma once

#include "chain/Hash.h"

#include <cstdint>
#include <string>
#include <vector>

// BIP32 hierarchical-deterministic keys over secp256k1.
//
// HdKey is a private node: it is only alive while a seed is being imported (or,
// later, while signing) and scrubs its private key when destroyed. HdPubKey is
// the public half of a node: an account-level HdPubKey (m/purpose'/coin'/account')
// is all GitWallet stores, and every address below it is derived publicly, so
// the seed never has to be kept.
namespace chain {

// Version prefixes of serialized extended public keys (mainnet).
namespace xpubVersion {
constexpr uint32_t kXpub = 0x0488B21E; // BIP44 legacy (and Ethereum accounts)
constexpr uint32_t kYpub = 0x049D7CB2; // BIP49 nested SegWit
constexpr uint32_t kZpub = 0x04B24746; // BIP84 native SegWit
} // namespace xpubVersion

class HdPubKey;

class HdKey
{
public:
    // Master node from a BIP39/BIP32 seed (HMAC-SHA512 "Bitcoin seed").
    static HdKey fromSeed(const Bytes& seed);

    // Child at a single index. Set `hardened` for i' (adds 0x80000000).
    HdKey deriveChild(uint32_t index, bool hardened) const;

    // Full path from this node, e.g. "m/84'/0'/0'/0/0" or "84'/0'/0'/0/0".
    HdKey derivePath(const std::string& path) const;

    const Bytes& privateKey() const { return priv_; }     // 32 bytes
    const Bytes& chainCode() const { return chainCode_; } // 32 bytes

    Bytes publicKeyCompressed() const;   // 33 bytes (0x02/0x03 prefix)
    Bytes publicKeyUncompressed() const; // 65 bytes (0x04 prefix)

    uint8_t depth() const { return depth_; }
    uint32_t parentFingerprint() const { return parentFp_; }
    uint32_t childNumber() const { return childNumber_; }
    uint32_t fingerprint() const; // first 4 bytes of HASH160(public key), big-endian

    // The public node at the same position (xpub).
    HdPubKey neuter() const;

    HdKey(const HdKey& other);
    HdKey(HdKey&& other) noexcept;
    HdKey& operator=(const HdKey& other);
    HdKey& operator=(HdKey&& other) noexcept;
    ~HdKey(); // scrubs the private key

private:
    HdKey() = default;

    Bytes priv_;      // 32 bytes
    Bytes chainCode_; // 32 bytes
    uint8_t depth_ = 0;
    uint32_t parentFp_ = 0;
    uint32_t childNumber_ = 0;
};

class HdPubKey
{
public:
    HdPubKey() = default;

    static HdPubKey fromParts(const Bytes& compressedPublicKey, const Bytes& chainCode,
                              uint8_t depth = 0, uint32_t parentFingerprint = 0,
                              uint32_t childNumber = 0);

    // Parses a serialized extended public key (xpub / ypub / zpub, Base58Check,
    // 78 bytes). On success fills `out` and, if given, the 4-byte version.
    static bool parse(const std::string& text, HdPubKey* out, uint32_t* versionOut = nullptr,
                      std::string* error = nullptr);

    bool isValid() const { return pub_.size() == 33 && chainCode_.size() == 32; }

    // Non-hardened child (CKDpub). Hardened children need the private key.
    HdPubKey deriveChild(uint32_t index) const;

    const Bytes& publicKey() const { return pub_; }       // 33 bytes, compressed
    const Bytes& chainCode() const { return chainCode_; } // 32 bytes
    Bytes publicKeyUncompressed() const;                  // 65 bytes (Ethereum)

    uint8_t depth() const { return depth_; }
    uint32_t parentFingerprint() const { return parentFp_; }
    uint32_t childNumber() const { return childNumber_; }
    uint32_t fingerprint() const;

    // Base58Check serialization with the given version prefix (xpubVersion::k*).
    std::string serialize(uint32_t version) const;

private:
    Bytes pub_;
    Bytes chainCode_;
    uint8_t depth_ = 0;
    uint32_t parentFp_ = 0;
    uint32_t childNumber_ = 0;
};

} // namespace chain
