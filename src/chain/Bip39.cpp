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

// Calls f(first, last) for every run of non-whitespace characters in `text`.
// split() walks the sentence twice through here, so the number of words it
// reserves room for and the words it then builds can never disagree.
template <class F>
void forEachWord(const std::string& text, F&& f)
{
    const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    const char* p = text.data();
    const char* const end = p + text.size();
    while (p != end) {
        if (isSpace(*p)) { ++p; continue; }
        const char* const first = p;
        while (p != end && !isSpace(*p)) ++p;
        f(first, p);
    }
}

} // namespace

std::vector<std::string> split(const std::string& sentence)
{
    // Every word is built once, straight from its slice of the sentence, and
    // lowercased in place, so its buffer holds no seed bytes beyond its own
    // size() characters and secureClear() on it leaves nothing behind. Words
    // cut from one reused buffer would not: clear() keeps the tails of
    // earlier, longer words past the new end, and copying a short word copies
    // its whole small-string buffer, those tails included.
    // The words are counted first so the vector is reserved exactly: a vector
    // that grows reallocates, and the old buffer would be freed unscrubbed.
    size_t count = 0;
    forEachWord(sentence, [&count](const char*, const char*) { ++count; });

    std::vector<std::string> out;
    // If building a word throws (no memory for a long one), the words built so
    // far are scrubbed before the vector frees them.
    struct Scrub {
        std::vector<std::string>* words; bool done;
        ~Scrub() { if (!done) for (std::string& w : *words) secureClear(w); }
    } scrub{&out, false};
    out.reserve(count);
    forEachWord(sentence, [&out](const char* first, const char* last) {
        std::string& word = out.emplace_back(first, static_cast<size_t>(last - first));
        for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    scrub.done = true;
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
