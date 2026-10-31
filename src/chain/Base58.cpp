#include "chain/Base58.h"

#include <algorithm>
#include <array>

namespace chain::base58 {

namespace {
const char* kAlphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
}

std::string encode(const Bytes& data)
{
    // Count leading zero bytes -> leading '1's.
    size_t zeros = 0;
    while (zeros < data.size() && data[zeros] == 0) ++zeros;

    // Base-256 -> base-58 by repeated division (big-endian digit buffer).
    std::vector<uint8_t> b58((data.size() - zeros) * 138 / 100 + 1, 0);
    for (size_t i = zeros; i < data.size(); ++i) {
        int carry = data[i];
        for (auto it = b58.rbegin(); it != b58.rend(); ++it) {
            carry += 256 * (*it);
            *it = static_cast<uint8_t>(carry % 58);
            carry /= 58;
        }
    }

    auto it = std::find_if(b58.begin(), b58.end(), [](uint8_t c) { return c != 0; });
    std::string out;
    out.reserve(zeros + (b58.end() - it));
    out.assign(zeros, '1');
    for (; it != b58.end(); ++it) out.push_back(kAlphabet[*it]);
    return out;
}

std::string encodeCheck(const Bytes& payload)
{
    Bytes full = payload;
    const Bytes check = dsha256(payload);
    full.insert(full.end(), check.begin(), check.begin() + 4);
    return encode(full);
}

bool decode(const std::string& text, Bytes* out)
{
    static const std::array<int8_t, 256> map = [] {
        std::array<int8_t, 256> m{};
        m.fill(-1);
        for (int i = 0; kAlphabet[i]; ++i) m[static_cast<uint8_t>(kAlphabet[i])] = static_cast<int8_t>(i);
        return m;
    }();

    size_t zeros = 0;
    while (zeros < text.size() && text[zeros] == '1') ++zeros;

    // Base-58 -> base-256 (big-endian digit buffer).
    std::vector<uint8_t> b256((text.size() - zeros) * 733 / 1000 + 1, 0);
    for (size_t i = zeros; i < text.size(); ++i) {
        const int digit = map[static_cast<uint8_t>(text[i])];
        if (digit < 0) return false;
        int carry = digit;
        for (auto it = b256.rbegin(); it != b256.rend(); ++it) {
            carry += 58 * (*it);
            *it = static_cast<uint8_t>(carry % 256);
            carry /= 256;
        }
        if (carry != 0) return false;
    }

    auto it = std::find_if(b256.begin(), b256.end(), [](uint8_t c) { return c != 0; });
    out->assign(zeros, 0x00);
    out->insert(out->end(), it, b256.end());
    return true;
}

bool decodeCheck(const std::string& text, Bytes* payloadOut)
{
    Bytes full;
    if (!decode(text, &full) || full.size() < 4) return false;
    const Bytes payload(full.begin(), full.end() - 4);
    const Bytes check = dsha256(payload);
    if (!std::equal(check.begin(), check.begin() + 4, full.end() - 4)) return false;
    *payloadOut = payload;
    return true;
}

} // namespace chain::base58
