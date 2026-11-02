#pragma once

#include "chain/Hash.h"

#include <cstdint>
#include <string>

// An unsigned integer amount of any size, in the asset's smallest unit
// (satoshi, wei, token base unit). Wei balances do not fit in 64 bits, so the
// value is kept as minimal big-endian bytes; arithmetic and decimal conversion
// use Botan's BigInt inside the .cpp.
namespace chain {

class Amount
{
public:
    Amount() = default;

    static Amount fromU64(uint64_t value);
    static Amount fromBigEndian(const Bytes& bigEndian); // e.g. a 32-byte uint256 ABI word
    // An Ethereum JSON-RPC quantity ("0x0", "0x1bc16d674ec80000"). False on bad input.
    static bool fromHexQuantity(const std::string& hex, Amount* out);
    // A non-negative decimal integer ("1234567890"). False on bad input.
    static bool fromDecimal(const std::string& digits, Amount* out);

    Amount& operator+=(const Amount& other);
    friend Amount operator+(Amount a, const Amount& b) { a += b; return a; }
    // Stops at zero: an amount is never negative.
    Amount& operator-=(const Amount& other);

    bool isZero() const { return be_.empty(); }
    int compare(const Amount& other) const; // -1, 0, 1
    bool operator==(const Amount& other) const { return compare(other) == 0; }
    bool operator!=(const Amount& other) const { return compare(other) != 0; }

    std::string toDecimalString() const; // integer, smallest units

    // value / 10^decimals, at most `maxFraction` fraction digits (truncated,
    // never rounded up), trailing zeros trimmed: format(18, 6) of
    // 1234567890123456789012 wei -> "1234.56789".
    std::string format(int decimals, int maxFraction) const;

    // value / 10^decimals as a double — for USD estimates and sorting only.
    double toDouble(int decimals) const;

    const Bytes& bigEndian() const { return be_; } // minimal; empty means zero

private:
    Bytes be_;
};

} // namespace chain
