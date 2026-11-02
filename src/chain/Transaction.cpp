#include "chain/Transaction.h"

#include "chain/Base58.h"
#include "chain/Bech32.h"

#include <botan/bigint.h>

#include <algorithm>
#include <cstring>
#include <span>

namespace chain {

namespace {

// A block holds 4 MB at most, witness included: no transaction is longer.
constexpr size_t kMaxTransactionBytes = 4'000'000;
// And a block's weight is this, at 4 per byte a witness does not hold: what a block cannot hold is
// no transaction, whatever it hashes to.
constexpr size_t kMaxTransactionWeight = 4'000'000;
// The smallest an input (outpoint, empty script, sequence) and an output (value, empty
// script) can be: a count larger than the bytes left can hold is refused before any of it
// is reserved.
constexpr size_t kMinInputBytes = 41;
constexpr size_t kMinOutputBytes = 9;
constexpr uint64_t kMaxMoney = 21'000'000ull * 100'000'000ull;

// Bounds-checked reading of the serialization.
class Reader
{
public:
    explicit Reader(const Bytes& data) : data_(data) {}

    bool failed() const { return failed_; }
    size_t position() const { return at_; }
    size_t left() const { return failed_ ? 0 : data_.size() - at_; }

    uint8_t u8()
    {
        if (!need(1)) return 0;
        return data_[at_++];
    }
    uint32_t u32()
    {
        if (!need(4)) return 0;
        const uint32_t value = uint32_t(data_[at_]) | uint32_t(data_[at_ + 1]) << 8 | uint32_t(data_[at_ + 2]) << 16
                               | uint32_t(data_[at_ + 3]) << 24;
        at_ += 4;
        return value;
    }
    uint64_t u64()
    {
        const uint64_t low = u32();
        const uint64_t high = u32();
        return low | high << 32;
    }
    uint64_t varint()
    {
        const uint8_t first = u8();
        if (first < 0xfd) return first;
        if (first == 0xfd) {
            const uint32_t low = u8();
            const uint32_t high = u8();
            return low | high << 8;
        }
        return first == 0xfe ? u32() : u64();
    }
    void skip(uint64_t count)
    {
        if (need(count))
            at_ += size_t(count);
    }
    // The hash, in the order explorers write it.
    Hash256 hash()
    {
        Hash256 out{};
        if (need(32)) {
            std::reverse_copy(data_.begin() + at_, data_.begin() + at_ + 32, out.begin());
            at_ += 32;
        }
        return out;
    }
    Bytes bytes(uint64_t count)
    {
        Bytes out;
        if (need(count)) {
            out.assign(data_.begin() + at_, data_.begin() + at_ + count);
            at_ += size_t(count);
        }
        return out;
    }

private:
    bool need(uint64_t count)
    {
        if (failed_ || count > data_.size() - at_) {
            failed_ = true;
            return false;
        }
        return true;
    }

    const Bytes& data_;
    size_t at_ = 0;
    bool failed_ = false;
};

Hash256 asDisplayed(const Bytes& computed)
{
    Hash256 out{};
    if (computed.size() == out.size())
        std::reverse_copy(computed.begin(), computed.end(), out.begin());
    return out;
}

// The bytes of a target, most significant first. False for bits that name no target: a
// sign set, or a number too long for 256 bits.
bool targetOf(uint32_t bits, Hash256* out)
{
    const int size = int(bits >> 24);
    const uint32_t word = bits & 0x007fffff;
    if ((bits & 0x00800000) != 0 || word == 0 || size > 34 || (size > 33 && word > 0xff) || (size > 32 && word > 0xffff))
        return false;
    out->fill(0);
    for (int i = 0; i < 3; ++i) {
        const uint8_t byte = uint8_t(word >> (8 * (2 - i)));
        const int at = 32 - size + i;
        if (at >= 0 && at < 32)
            (*out)[size_t(at)] = byte;
        else if (at < 0 && byte != 0)
            return false;
    }
    return true;
}

Botan::BigInt targetNumber(uint32_t bits)
{
    Hash256 target{};
    return targetOf(bits, &target) ? Botan::BigInt(std::span<const uint8_t>(target.data(), target.size())) : Botan::BigInt();
}

uint32_t bitsOf(const Botan::BigInt& target)
{
    int size = int(target.bytes());
    Botan::BigInt shifted = target;
    if (size > 3)
        shifted >>= size_t(8 * (size - 3));
    else
        shifted <<= size_t(8 * (3 - size));
    uint32_t word = uint32_t(shifted.to_u32bit());
    // The top bit of the three is a sign.
    if ((word & 0x00800000) != 0) {
        word >>= 8;
        ++size;
    }
    return uint32_t(size) << 24 | word;
}

} // namespace

std::string hashHex(const Hash256& hash)
{
    return toHex(Bytes(hash.begin(), hash.end()));
}

bool hashFromHex(const std::string& hex, Hash256* out)
{
    if (hex.size() != 64)
        return false;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Hash256 parsed{};
    for (size_t i = 0; i < parsed.size(); ++i) {
        const int high = digit(hex[2 * i]);
        const int low = digit(hex[2 * i + 1]);
        if (high < 0 || low < 0)
            return false;
        parsed[i] = uint8_t(high << 4 | low);
    }
    *out = parsed;
    return true;
}

bool Transaction::coinbase() const
{
    return inputs.size() == 1 && inputs[0].previous.n == 0xffffffffu && inputs[0].previous.txid == Hash256{};
}

bool Transaction::signalsReplaceable() const
{
    return std::any_of(inputs.begin(), inputs.end(), [](const TxInput& in) { return in.sequence < 0xfffffffeu; });
}

uint64_t Transaction::outputTotal() const
{
    uint64_t total = 0;
    for (const TxOutput& output : outputs)
        total += output.value;
    return total;
}

bool parseTransaction(const Bytes& raw, Transaction* out)
{
    if (raw.size() > kMaxTransactionBytes)
        return false;
    Reader in(raw);
    Transaction tx;
    in.skip(4); // version

    // The witness form says so with a zero where the count of inputs would be, then a one.
    bool witness = false;
    uint64_t inputCount = in.varint();
    if (inputCount == 0 && !in.failed()) {
        if (in.u8() != 1)
            return false;
        witness = true;
        inputCount = in.varint();
    }
    // A transaction spends something: an empty one is what a block's data hides behind.
    if (in.failed() || inputCount == 0 || inputCount > in.left() / kMinInputBytes)
        return false;

    const size_t inputsFrom = witness ? 6 : 4;
    tx.inputs.reserve(size_t(inputCount));
    for (uint64_t i = 0; i < inputCount; ++i) {
        TxInput input;
        input.previous.txid = in.hash();
        input.previous.n = in.u32();
        in.skip(in.varint());
        input.sequence = in.u32();
        if (in.failed())
            return false;
        tx.inputs.push_back(std::move(input));
    }

    const uint64_t outputCount = in.varint();
    if (in.failed() || outputCount == 0 || outputCount > in.left() / kMinOutputBytes)
        return false;
    tx.outputs.reserve(size_t(outputCount));
    uint64_t total = 0;
    for (uint64_t i = 0; i < outputCount; ++i) {
        TxOutput output;
        output.value = in.u64();
        output.script = in.bytes(in.varint());
        if (in.failed() || output.value > kMaxMoney)
            return false;
        total += output.value;
        if (total > kMaxMoney)
            return false;
        tx.outputs.push_back(std::move(output));
    }
    const size_t outputsEnd = in.position();

    if (witness) {
        bool anyWitness = false;
        for (uint64_t i = 0; i < inputCount; ++i) {
            const uint64_t items = in.varint();
            if (in.failed() || items > in.left())
                return false;
            anyWitness = anyWitness || items > 0;
            for (uint64_t item = 0; item < items; ++item)
                in.skip(in.varint());
            if (in.failed())
                return false;
        }
        // The form exists for its witness data; without any it is not how anyone writes one.
        if (!anyWitness)
            return false;
    }
    in.skip(4); // lock time
    if (in.failed() || in.left() != 0)
        return false;

    const size_t strippedSize = 4 + (outputsEnd - inputsFrom) + 4;
    if (3 * strippedSize + raw.size() > kMaxTransactionWeight)
        return false;
    if (tx.inputs.size() > 1) {
        std::vector<OutPoint> points;
        points.reserve(tx.inputs.size());
        for (const TxInput& input : tx.inputs)
            points.push_back(input.previous);
        std::sort(points.begin(), points.end(), [](const OutPoint& a, const OutPoint& b) {
            const int order = std::memcmp(a.txid.data(), b.txid.data(), a.txid.size());
            return order != 0 ? order < 0 : a.n < b.n;
        });
        if (std::adjacent_find(points.begin(), points.end()) != points.end())
            return false;
    }

    // What the name is the hash of: all but the marker, the flag and the witness data.
    Bytes stripped;
    stripped.reserve(strippedSize);
    stripped.insert(stripped.end(), raw.begin(), raw.begin() + 4);
    stripped.insert(stripped.end(), raw.begin() + inputsFrom, raw.begin() + outputsEnd);
    stripped.insert(stripped.end(), raw.end() - 4, raw.end());
    tx.txid = asDisplayed(dsha256(stripped));
    tx.size = int(raw.size());
    tx.strippedSize = int(strippedSize);
    tx.vsize = (3 * tx.strippedSize + tx.size + 3) / 4;
    *out = std::move(tx);
    return true;
}

std::string addressFromScript(const Bytes& script, const AddressFormat& format)
{
    const size_t size = script.size();
    const auto middle = [&script](size_t from, size_t count) { return Bytes(script.begin() + from, script.begin() + from + count); };
    // OP_DUP OP_HASH160 <20> OP_EQUALVERIFY OP_CHECKSIG
    if (size == 25 && script[0] == 0x76 && script[1] == 0xa9 && script[2] == 0x14 && script[23] == 0x88 && script[24] == 0xac) {
        Bytes payload{format.p2pkhVersion};
        const Bytes hash = middle(3, 20);
        payload.insert(payload.end(), hash.begin(), hash.end());
        return base58::encodeCheck(payload);
    }
    // OP_HASH160 <20> OP_EQUAL
    if (size == 23 && script[0] == 0xa9 && script[1] == 0x14 && script[22] == 0x87) {
        Bytes payload{format.p2shVersion};
        const Bytes hash = middle(2, 20);
        payload.insert(payload.end(), hash.begin(), hash.end());
        return base58::encodeCheck(payload);
    }
    // OP_0..OP_16 <2 to 40 bytes>: a witness program of any version, as BIP173 and BIP350 write it.
    if (size >= 4 && size <= 42 && (script[0] == 0x00 || (script[0] >= 0x51 && script[0] <= 0x60)) && script[1] == size - 2) {
        const int version = script[0] == 0x00 ? 0 : script[0] - 0x50;
        if (version == 0 && size != 22 && size != 34)
            return {};
        return bech32::segwitEncode(format.hrp, version, middle(2, size - 2));
    }
    return {};
}

std::string scripthashOfScript(const Bytes& script)
{
    return hashHex(asDisplayed(sha256(script)));
}

bool parseHeader(const Bytes& raw, BlockHeader* out)
{
    if (raw.size() != 80)
        return false;
    Reader in(raw);
    BlockHeader header;
    header.version = int32_t(in.u32());
    header.previous = in.hash();
    header.merkleRoot = in.hash();
    header.time = in.u32();
    header.bits = in.u32();
    header.nonce = in.u32();
    header.hash = asDisplayed(dsha256(raw));
    *out = header;
    return !in.failed();
}

Bytes serializeHeader(const BlockHeader& header)
{
    Bytes out;
    out.reserve(80);
    const auto word = [&out](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            out.push_back(uint8_t(value >> shift));
    };
    word(uint32_t(header.version));
    out.insert(out.end(), header.previous.rbegin(), header.previous.rend());
    out.insert(out.end(), header.merkleRoot.rbegin(), header.merkleRoot.rend());
    word(header.time);
    word(header.bits);
    word(header.nonce);
    return out;
}

bool headerHasWork(const BlockHeader& header, uint32_t limit)
{
    Hash256 target{};
    Hash256 most{};
    if (!targetOf(header.bits, &target) || !targetOf(limit, &most) || target == Hash256{})
        return false;
    // Both read from the most significant byte, as hashes here do.
    return std::memcmp(target.data(), most.data(), target.size()) <= 0
           && std::memcmp(header.hash.data(), target.data(), target.size()) <= 0;
}

uint32_t nextBits(uint32_t lastBits, uint32_t firstTime, uint32_t lastTime, uint32_t limit, uint32_t targetSeconds)
{
    // As signed numbers: a period can end earlier than it began.
    const int64_t span = std::clamp<int64_t>(int64_t(lastTime) - int64_t(firstTime), targetSeconds / 4, int64_t(targetSeconds) * 4);
    Botan::BigInt target = targetNumber(lastBits);
    target *= Botan::BigInt::from_u64(uint64_t(span));
    target /= Botan::BigInt::from_u64(targetSeconds);
    const Botan::BigInt most = targetNumber(limit);
    if (target > most)
        target = most;
    return bitsOf(target);
}

Hash256 merkleRootOf(const Hash256& leaf, const std::vector<Hash256>& branch, uint64_t position)
{
    // Hashed in the computed order, the reverse of how they read.
    Bytes node(leaf.rbegin(), leaf.rend());
    Bytes both;
    both.reserve(64);
    for (const Hash256& other : branch) {
        const Bytes sibling(other.rbegin(), other.rend());
        both.clear();
        const Bytes& left = (position & 1) != 0 ? sibling : node;
        const Bytes& right = (position & 1) != 0 ? node : sibling;
        both.insert(both.end(), left.begin(), left.end());
        both.insert(both.end(), right.begin(), right.end());
        node = dsha256(both);
        position >>= 1;
    }
    return asDisplayed(node);
}

bool proofLeadsTo(const Hash256& leaf, const std::vector<Hash256>& branch, uint64_t position, const Hash256& root)
{
    Bytes node(leaf.rbegin(), leaf.rend());
    Bytes both;
    both.reserve(64);
    Transaction any;
    for (const Hash256& other : branch) {
        const Bytes sibling(other.rbegin(), other.rend());
        both.clear();
        const Bytes& left = (position & 1) != 0 ? sibling : node;
        const Bytes& right = (position & 1) != 0 ? node : sibling;
        both.insert(both.end(), left.begin(), left.end());
        both.insert(both.end(), right.begin(), right.end());
        if (parseTransaction(both, &any))
            return false;
        node = dsha256(both);
        position >>= 1;
    }
    return asDisplayed(node) == root;
}

} // namespace chain
