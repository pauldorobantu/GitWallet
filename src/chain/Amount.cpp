#include "chain/Amount.h"

#include <botan/bigint.h>

#include <cctype>
#include <cmath>
#include <span>

namespace chain {

namespace {

Botan::BigInt toBig(const Bytes& be)
{
    return be.empty() ? Botan::BigInt() : Botan::BigInt(std::span<const uint8_t>(be.data(), be.size()));
}

Bytes fromBig(const Botan::BigInt& v)
{
    if (v.is_zero()) return {};
    return v.serialize<std::vector<uint8_t>>(); // minimal big-endian
}

Bytes stripLeadingZeros(const Bytes& be)
{
    size_t i = 0;
    while (i < be.size() && be[i] == 0) ++i;
    return Bytes(be.begin() + static_cast<std::ptrdiff_t>(i), be.end());
}

} // namespace

Amount Amount::fromU64(uint64_t value)
{
    Amount a;
    a.be_ = fromBig(Botan::BigInt::from_u64(value));
    return a;
}

Amount Amount::fromBigEndian(const Bytes& bigEndian)
{
    Amount a;
    a.be_ = stripLeadingZeros(bigEndian);
    return a;
}

bool Amount::fromHexQuantity(const std::string& hex, Amount* out)
{
    if (hex.size() < 3 || hex[0] != '0' || (hex[1] != 'x' && hex[1] != 'X')) return false;
    for (size_t i = 2; i < hex.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(hex[i]))) return false;
    try {
        out->be_ = fromBig(Botan::BigInt::from_string(hex));
    } catch (...) {
        return false;
    }
    return true;
}

bool Amount::fromDecimal(const std::string& digits, Amount* out)
{
    if (digits.empty()) return false;
    for (char c : digits)
        if (c < '0' || c > '9') return false;
    try {
        out->be_ = fromBig(Botan::BigInt::from_string(digits));
    } catch (...) {
        return false;
    }
    return true;
}

Amount& Amount::operator+=(const Amount& other)
{
    Botan::BigInt sum = toBig(be_);
    sum += toBig(other.be_);
    be_ = fromBig(sum);
    return *this;
}

int Amount::compare(const Amount& other) const
{
    const int32_t c = toBig(be_).cmp(toBig(other.be_));
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

std::string Amount::toDecimalString() const
{
    return be_.empty() ? std::string("0") : toBig(be_).to_dec_string();
}

std::string Amount::format(int decimals, int maxFraction) const
{
    std::string s = toDecimalString();
    if (decimals <= 0) return s;
    if (static_cast<int>(s.size()) <= decimals)
        s.insert(0, static_cast<size_t>(decimals) + 1 - s.size(), '0');
    const std::string intPart = s.substr(0, s.size() - static_cast<size_t>(decimals));
    std::string frac = s.substr(s.size() - static_cast<size_t>(decimals));
    if (maxFraction >= 0 && static_cast<int>(frac.size()) > maxFraction)
        frac.resize(static_cast<size_t>(maxFraction));
    while (!frac.empty() && frac.back() == '0') frac.pop_back();
    return frac.empty() ? intPart : intPart + "." + frac;
}

double Amount::toDouble(int decimals) const
{
    if (be_.empty()) return 0.0;
    const double v = std::stod(toDecimalString());
    return decimals > 0 ? v / std::pow(10.0, decimals) : v;
}

} // namespace chain
