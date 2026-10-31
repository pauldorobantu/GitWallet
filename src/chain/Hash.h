#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Thin wrappers over Botan for the hash primitives an HD wallet needs.
// Keeping them in one place means the derivation / address code never touches
// Botan directly and can be read against the BIP specs line by line.
namespace chain {

using Bytes = std::vector<uint8_t>;

Bytes sha256(const uint8_t* data, size_t len);
Bytes sha256(const Bytes& data);
Bytes dsha256(const Bytes& data);               // SHA256(SHA256(x)) — Bitcoin
Bytes sha512(const Bytes& data);
Bytes ripemd160(const Bytes& data);
Bytes hash160(const Bytes& data);               // RIPEMD160(SHA256(x))

// HMAC-SHA512 (BIP32 master key + child derivation).
Bytes hmacSha512(const Bytes& key, const Bytes& data);

// PBKDF2-HMAC-SHA512, `iterations` rounds, 64-byte output (BIP39 seed).
Bytes pbkdf2HmacSha512(const std::string& password, const Bytes& salt, size_t iterations);

// Keccak-256 (the original Keccak padding, as Ethereum uses — NOT NIST SHA3).
Bytes keccak256(const Bytes& data);

// Lowercase hex, no prefix.
std::string toHex(const Bytes& data);
Bytes fromHex(const std::string& hex);

} // namespace chain
