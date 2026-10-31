#include "chain/Base58.h"

#include <algorithm>

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

} // namespace chain::base58
