#pragma once

#include "chain/Hash.h"

#include <string>

// Bech32 (BIP173) and Bech32m (BIP350) — SegWit v0 and v1+ addresses.
namespace chain::bech32 {

// Encodes a SegWit output: witness version + program, with the right constant
// (Bech32 for v0, Bech32m for v1+). hrp is "bc" (mainnet) / "tb" (testnet).
std::string segwitEncode(const std::string& hrp, int witver, const Bytes& program);

} // namespace chain::bech32
