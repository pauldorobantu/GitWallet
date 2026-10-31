#include "chain/Hash.h"

#include <botan/hash.h>
#include <botan/mac.h>
#include <botan/pwdhash.h>

#include <stdexcept>

namespace chain {

namespace {
Bytes digest(const char* name, const uint8_t* data, size_t len)
{
    auto h = Botan::HashFunction::create_or_throw(name);
    h->update(data, len);
    auto out = h->final();
    return Bytes(out.begin(), out.end());
}
} // namespace

Bytes sha256(const uint8_t* data, size_t len) { return digest("SHA-256", data, len); }
Bytes sha256(const Bytes& data) { return digest("SHA-256", data.data(), data.size()); }

Bytes dsha256(const Bytes& data) { return sha256(sha256(data)); }

Bytes sha512(const Bytes& data) { return digest("SHA-512", data.data(), data.size()); }

Bytes ripemd160(const Bytes& data) { return digest("RIPEMD-160", data.data(), data.size()); }

Bytes hash160(const Bytes& data) { return ripemd160(sha256(data)); }

Bytes hmacSha512(const Bytes& key, const Bytes& data)
{
    auto mac = Botan::MessageAuthenticationCode::create_or_throw("HMAC(SHA-512)");
    mac->set_key(key.data(), key.size());
    mac->update(data.data(), data.size());
    auto out = mac->final();
    return Bytes(out.begin(), out.end());
}

Bytes pbkdf2HmacSha512(const std::string& password, const Bytes& salt, size_t iterations)
{
    auto fam = Botan::PasswordHashFamily::create_or_throw("PBKDF2(SHA-512)");
    auto kdf = fam->from_params(iterations);
    Bytes out(64, 0);
    kdf->derive_key(out.data(), out.size(),
                    password.data(), password.size(),
                    salt.data(), salt.size());
    return out;
}

Bytes keccak256(const Bytes& data) { return digest("Keccak-1600(256)", data.data(), data.size()); }

std::string toHex(const Bytes& data)
{
    static const char* k = "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (uint8_t b : data) {
        s.push_back(k[b >> 4]);
        s.push_back(k[b & 0xF]);
    }
    return s;
}

Bytes fromHex(const std::string& hex)
{
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string h = hex;
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    if (h.size() % 2 != 0) throw std::invalid_argument("hex length odd");
    Bytes out;
    out.reserve(h.size() / 2);
    for (size_t i = 0; i < h.size(); i += 2) {
        int hi = nib(h[i]), lo = nib(h[i + 1]);
        if (hi < 0 || lo < 0) throw std::invalid_argument("bad hex digit");
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

} // namespace chain
