#pragma once

#include "chain/Hash.h"

#include <cstdint>
#include <string>
#include <vector>

// BIP32 hierarchical-deterministic key derivation over secp256k1.
// A node here always carries a private key (we derive from the seed, so the
// whole path is private); the public key is computed on demand for addresses
// and, later, signing.
namespace chain {

class HdKey
{
public:
    // Master node from a BIP39/BIP32 seed (HMAC-SHA512 "Bitcoin seed").
    static HdKey fromSeed(const Bytes& seed);

    // Child at a single index. Set `hardened` for i' (adds 0x80000000).
    HdKey deriveChild(uint32_t index, bool hardened) const;

    // Full path from this node, e.g. "m/84'/0'/0'/0/0" or "84'/0'/0'/0/0".
    HdKey derivePath(const std::string& path) const;

    const Bytes& privateKey() const { return priv_; }   // 32 bytes
    const Bytes& chainCode() const { return chainCode_; } // 32 bytes

    Bytes publicKeyCompressed() const;   // 33 bytes (0x02/0x03 prefix)
    Bytes publicKeyUncompressed() const; // 65 bytes (0x04 prefix)

private:
    HdKey() = default;
    Bytes priv_;      // 32 bytes
    Bytes chainCode_; // 32 bytes
};

} // namespace chain
