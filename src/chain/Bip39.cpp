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
    // Reserve up front: a vector or string that grows reallocates, and the old
    // buffer — holding seed words — would be freed without being scrubbed.
    std::vector<std::string> out;
    out.reserve(sentence.size() / 2 + 1);
    std::string cur;
    cur.reserve(sentence.size());
    for (char c : sentence) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (!cur.empty()) out.push_back(cur);
    secureClear(cur);
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

    // The bit string and the entropy below ARE the seed: scrub them on every
    // way out of this function.
    Bytes bits;
    Bytes entropy;
    Bytes hash;
    struct Scrub {
        Bytes* a; Bytes* b; Bytes* c;
        ~Scrub() { secureClear(*a); secureClear(*b); secureClear(*c); }
    } scrub{&bits, &entropy, &hash};

    // Pack the 11-bit indices into a bitstring of ENT+CS bits.
    bits.reserve(n * 11);
    for (const auto& w : words) {
        auto it = idx.find(w);
        if (it == idx.end()) return fail("a word is not in the BIP39 list");
        const int v = it->second;
        for (int b = 10; b >= 0; --b) bits.push_back(static_cast<uint8_t>((v >> b) & 1));
    }

    const size_t totalBits = n * 11;
    const size_t csBits = totalBits / 33; // ENT = 32*CS, total = ENT+CS = 33*CS
    const size_t entBits = totalBits - csBits;

    // Entropy bytes.
    entropy.assign(entBits / 8, 0);
    for (size_t i = 0; i < entBits; ++i)
        if (bits[i]) entropy[i / 8] |= static_cast<uint8_t>(1 << (7 - (i % 8)));

    // Expected checksum = first csBits bits of SHA256(entropy).
    hash = sha256(entropy);
    for (size_t i = 0; i < csBits; ++i) {
        const uint8_t expected = (hash[i / 8] >> (7 - (i % 8))) & 1;
        if (bits[entBits + i] != expected) return fail("checksum mismatch");
    }
    return true;
}

bool isWord(const std::string& word)
{
    return wordIndex().count(word) != 0;
}

Bytes toSeed(const std::vector<std::string>& words, const std::string& passphrase)
{
    // Sized up front so neither string reallocates (a reallocation would leave
    // an unscrubbed copy of the mnemonic in freed memory).
    size_t length = words.empty() ? 0 : words.size() - 1;
    for (const auto& w : words) length += w.size();
    std::string mnemonic;
    mnemonic.reserve(length);
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) mnemonic.push_back(' ');
        mnemonic += words[i];
    }
    std::string saltStr;
    saltStr.reserve(8 + passphrase.size());
    saltStr += "mnemonic";
    saltStr += passphrase;
    Bytes salt(saltStr.begin(), saltStr.end());

    Bytes seed = pbkdf2HmacSha512(mnemonic, salt, 2048);
    secureClear(mnemonic);
    secureClear(saltStr);
    secureClear(salt);
    return seed;
}

} // namespace chain::bip39
