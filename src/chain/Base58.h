#pragma once

#include "chain/Hash.h"

#include <string>

// Base58 and Base58Check (Bitcoin legacy addresses, WIF, extended keys).
namespace chain::base58 {

std::string encode(const Bytes& data);
// data || first 4 bytes of double-SHA256(data), then Base58.
std::string encodeCheck(const Bytes& payload);

// Inverse of encode(); false on a character outside the alphabet.
bool decode(const std::string& text, Bytes* out);
// Inverse of encodeCheck(): verifies and strips the 4-byte checksum.
bool decodeCheck(const std::string& text, Bytes* payloadOut);

} // namespace chain::base58
