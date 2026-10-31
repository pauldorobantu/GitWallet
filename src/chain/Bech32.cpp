#include "chain/Bech32.h"

#include <stdexcept>

// Reference Bech32 / Bech32m implementation (BIP173 / BIP350).
namespace chain::bech32 {

namespace {

const char* kCharset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

uint32_t polymod(const std::vector<uint8_t>& values)
{
    static const uint32_t gen[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
    uint32_t chk = 1;
    for (uint8_t v : values) {
        const uint8_t top = static_cast<uint8_t>(chk >> 25);
        chk = ((chk & 0x1ffffff) << 5) ^ v;
        for (int i = 0; i < 5; ++i)
            if ((top >> i) & 1) chk ^= gen[i];
    }
    return chk;
}

std::vector<uint8_t> hrpExpand(const std::string& hrp)
{
    std::vector<uint8_t> out;
    out.reserve(hrp.size() * 2 + 1);
    for (char c : hrp) out.push_back(static_cast<uint8_t>(c) >> 5);
    out.push_back(0);
    for (char c : hrp) out.push_back(static_cast<uint8_t>(c) & 31);
    return out;
}

// Convert 8-bit bytes to 5-bit groups (pad=true for encoding).
std::vector<uint8_t> convertBits(const Bytes& data, int from, int to, bool pad)
{
    uint32_t acc = 0;
    int bits = 0;
    std::vector<uint8_t> out;
    const uint32_t maxv = (1u << to) - 1;
    for (uint8_t value : data) {
        acc = (acc << from) | value;
        bits += from;
        while (bits >= to) {
            bits -= to;
            out.push_back(static_cast<uint8_t>((acc >> bits) & maxv));
        }
    }
    if (pad && bits > 0)
        out.push_back(static_cast<uint8_t>((acc << (to - bits)) & maxv));
    return out;
}

std::string encode(const std::string& hrp, const std::vector<uint8_t>& data, uint32_t constant)
{
    std::vector<uint8_t> values = hrpExpand(hrp);
    values.insert(values.end(), data.begin(), data.end());
    values.insert(values.end(), 6, 0);
    const uint32_t mod = polymod(values) ^ constant;

    std::string out = hrp + "1";
    for (uint8_t d : data) out.push_back(kCharset[d]);
    for (int i = 0; i < 6; ++i)
        out.push_back(kCharset[(mod >> (5 * (5 - i))) & 31]);
    return out;
}

} // namespace

std::string segwitEncode(const std::string& hrp, int witver, const Bytes& program)
{
    if (witver < 0 || witver > 16) throw std::invalid_argument("bad witness version");
    std::vector<uint8_t> data;
    data.push_back(static_cast<uint8_t>(witver));
    const std::vector<uint8_t> conv = convertBits(program, 8, 5, true);
    data.insert(data.end(), conv.begin(), conv.end());
    const uint32_t constant = (witver == 0) ? 1u : 0x2bc830a3u; // Bech32 vs Bech32m
    return encode(hrp, data, constant);
}

} // namespace chain::bech32
