#include "chain/Bip39.h"

#include <QFile>
#include <QString>

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace chain::bip39 {

namespace {

const std::vector<std::string>& wordlist()
{
    static const std::vector<std::string> words = [] {
        std::vector<std::string> w;
        QFile f(QStringLiteral(":/bip39/english.txt"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!f.atEnd()) {
                QByteArray line = f.readLine().trimmed();
                if (!line.isEmpty()) w.push_back(line.toStdString());
            }
        }
        return w;
    }();
    return words;
}

const std::unordered_map<std::string, int>& wordIndex()
{
    static const std::unordered_map<std::string, int> idx = [] {
        std::unordered_map<std::string, int> m;
        const auto& w = wordlist();
        for (int i = 0; i < static_cast<int>(w.size()); ++i) m[w[i]] = i;
        return m;
    }();
    return idx;
}

} // namespace

std::vector<std::string> split(const std::string& sentence)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : sentence) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool validate(const std::vector<std::string>& words, std::string* errorOut)
{
    auto fail = [&](const char* m) { if (errorOut) *errorOut = m; return false; };

    const size_t n = words.size();
    if (n != 12 && n != 15 && n != 18 && n != 21 && n != 24)
        return fail("mnemonic must be 12, 15, 18, 21 or 24 words");

    const auto& idx = wordIndex();
    if (idx.empty()) return fail("wordlist not loaded");

    // Pack the 11-bit indices into a bitstring of ENT+CS bits.
    std::vector<uint8_t> bits;
    bits.reserve(n * 11);
    for (const auto& w : words) {
        auto it = idx.find(w);
        if (it == idx.end()) return fail((std::string("word not in list: ") + w).c_str());
        const int v = it->second;
        for (int b = 10; b >= 0; --b) bits.push_back((v >> b) & 1);
    }

    const size_t totalBits = n * 11;
    const size_t csBits = totalBits / 33; // ENT = 32*CS, total = ENT+CS = 33*CS
    const size_t entBits = totalBits - csBits;

    // Entropy bytes.
    Bytes entropy(entBits / 8, 0);
    for (size_t i = 0; i < entBits; ++i)
        if (bits[i]) entropy[i / 8] |= (1 << (7 - (i % 8)));

    // Expected checksum = first csBits bits of SHA256(entropy).
    const Bytes hash = sha256(entropy);
    for (size_t i = 0; i < csBits; ++i) {
        const uint8_t expected = (hash[i / 8] >> (7 - (i % 8))) & 1;
        if (bits[entBits + i] != expected) return fail("checksum mismatch");
    }
    return true;
}

Bytes toSeed(const std::vector<std::string>& words, const std::string& passphrase)
{
    std::string mnemonic;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) mnemonic.push_back(' ');
        mnemonic += words[i];
    }
    std::string saltStr = "mnemonic" + passphrase;
    Bytes salt(saltStr.begin(), saltStr.end());
    return pbkdf2HmacSha512(mnemonic, salt, 2048);
}

} // namespace chain::bip39
