#include "net/Multicall.h"

#include "chain/Hash.h"

#include <QStringView>

#include <exception>
#include <string>

namespace net::multicall {

namespace {

// Every ABI head slot, length and static value occupies one 32-byte word.
constexpr qsizetype kWord = 32;

// A function selector is the first four bytes of keccak256 over the canonical
// signature. Each selector is derived from its signature with chain::keccak256
// and compared with the published value; all three were verified on 2026-10-31
// (chain::keccak256 and an independent Keccak implementation agree):
//   aggregate3((address,bool,bytes)[])  -> 0x82ad56cb
//   getEthBalance(address)              -> 0x4d2301cc
//   balanceOf(address)                  -> 0x70a08231
// A mismatch (a broken hash build) leaves the selector empty, and every encoder
// then returns an empty QByteArray: no call is better than a call to the wrong
// function.
QByteArray checkedSelector(const char* signature, quint32 expected)
{
    try {
        const std::string text(signature);
        const chain::Bytes h = chain::keccak256(chain::Bytes(text.begin(), text.end()));
        if (h.size() < 4) return {};
        const quint32 derived = (quint32(h[0]) << 24) | (quint32(h[1]) << 16)
                                | (quint32(h[2]) << 8) | quint32(h[3]);
        if (derived != expected) return {};
        return QByteArray(reinterpret_cast<const char*>(h.data()), 4);
    } catch (const std::exception&) {
        return {};
    }
}

const QByteArray& aggregate3Selector()
{
    static const QByteArray s = checkedSelector("aggregate3((address,bool,bytes)[])", 0x82ad56cbu);
    return s;
}

const QByteArray& getEthBalanceSelector()
{
    static const QByteArray s = checkedSelector("getEthBalance(address)", 0x4d2301ccu);
    return s;
}

const QByteArray& balanceOfSelector()
{
    static const QByteArray s = checkedSelector("balanceOf(address)", 0x70a08231u);
    return s;
}

int hexNibble(QChar c)
{
    const char16_t u = c.unicode();
    if (u >= u'0' && u <= u'9') return u - u'0';
    if (u >= u'a' && u <= u'f') return u - u'a' + 10;
    if (u >= u'A' && u <= u'F') return u - u'A' + 10;
    return -1;
}

// Hex digits (no prefix) -> bytes. QByteArray::fromHex silently skips invalid
// characters, so every digit is checked here instead.
bool decodeHexDigits(QStringView digits, QByteArray* out)
{
    if (digits.size() % 2 != 0) return false;
    QByteArray bytes(digits.size() / 2, Qt::Uninitialized);
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const int hi = hexNibble(digits[2 * i]);
        const int lo = hexNibble(digits[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = static_cast<char>((hi << 4) | lo);
    }
    *out = bytes;
    return true;
}

// "0x" + exactly 40 hex digits in any letter case -> the 20 address bytes. The
// EIP-55 checksum is not required: the addresses passed here are derived by
// chain:: or are built-in contract addresses.
bool parseAddress(const QString& text, QByteArray* out)
{
    if (text.size() != 42 || !text.startsWith(QLatin1String("0x"))) return false;
    return decodeHexDigits(QStringView(text).mid(2), out);
}

void appendWord(QByteArray& out, quint64 value)
{
    char word[kWord] = {};
    for (int i = 0; i < 8; ++i)
        word[kWord - 1 - i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    out.append(word, kWord);
}

// An address is a uint160 in the ABI: left-padded with zeros to a full word.
void appendAddress(QByteArray& out, const QByteArray& addr20)
{
    out.append(QByteArray(kWord - addr20.size(), '\0'));
    out.append(addr20);
}

qsizetype paddedSize(qsizetype n)
{
    return (n + kWord - 1) / kWord * kWord;
}

// A `bytes` payload is right-padded with zeros to a whole number of words.
void appendPadded(QByteArray& out, const QByteArray& data)
{
    out.append(data);
    out.append(QByteArray(paddedSize(data.size()) - data.size(), '\0'));
}

// The word at `pos` as a number. False unless the whole word lies inside
// `data` and its value fits in 64 bits.
bool readWord(const QByteArray& data, qsizetype pos, quint64* value)
{
    if (pos < 0 || pos > data.size() - kWord) return false;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + pos);
    for (qsizetype i = 0; i < kWord - 8; ++i)
        if (p[i] != 0) return false;
    quint64 v = 0;
    for (qsizetype i = kWord - 8; i < kWord; ++i)
        v = (v << 8) | p[i];
    *value = v;
    return true;
}

// A word used as an offset or a length. Capping it at the buffer size keeps
// every position computed from it below a few times that size, so the sums in
// decodeAggregate3 cannot overflow.
bool readSize(const QByteArray& data, qsizetype pos, qsizetype* value)
{
    quint64 v = 0;
    if (!readWord(data, pos, &v) || v > static_cast<quint64>(data.size())) return false;
    *value = static_cast<qsizetype>(v);
    return true;
}

QByteArray encodeAddressCall(const QByteArray& selector, const QString& address)
{
    QByteArray addr;
    if (selector.isEmpty() || !parseAddress(address, &addr)) return {};
    QByteArray out = selector;
    appendAddress(out, addr);
    return out;
}

} // namespace

// Layout, after the selector: offset of the array (0x20), its length N, N head
// words giving where each tuple starts (counted from the first byte after the
// length word), then the tuples (target, allowFailure, 0x60, callData length,
// callData right-padded).
//
// Reference vector, checked byte for byte against an independent Python encoder
// and executed with eth_call on Multicall3 (ethereum-rpc.publicnode.com,
// 2026-10-31): the single call getEthBalance(0x9858EfFD232B4033E47d90003D41EC34EcaEda94)
// with Multicall3 itself as the target encodes as these 292 bytes (concatenate):
//   0x82ad56cb
//   0000000000000000000000000000000000000000000000000000000000000020
//   0000000000000000000000000000000000000000000000000000000000000001
//   0000000000000000000000000000000000000000000000000000000000000020
//   000000000000000000000000ca11bde05977b3631167028862be2a173976ca11
//   0000000000000000000000000000000000000000000000000000000000000001
//   0000000000000000000000000000000000000000000000000000000000000060
//   0000000000000000000000000000000000000000000000000000000000000024
//   4d2301cc0000000000000000000000009858effd232b4033e47d90003d41ec34
//   ecaeda9400000000000000000000000000000000000000000000000000000000
QByteArray encodeAggregate3(const QList<Call>& calls)
{
    const QByteArray& selector = aggregate3Selector();
    if (selector.isEmpty()) return {};

    // Every target is checked first: one bad target yields no calldata at all,
    // never a batch that silently calls something else.
    QList<QByteArray> targets;
    targets.reserve(calls.size());
    for (const Call& call : calls) {
        QByteArray addr;
        if (!parseAddress(call.target, &addr)) return {};
        targets.append(addr);
    }

    const qsizetype n = calls.size();
    QByteArray out = selector;
    appendWord(out, quint64(kWord)); // the only argument is dynamic: its data starts one word in
    appendWord(out, quint64(n));
    quint64 offset = quint64(n) * quint64(kWord); // the first tuple follows the n head words
    for (const Call& call : calls) {
        appendWord(out, offset);
        offset += quint64(4 * kWord + paddedSize(call.callData.size()));
    }
    for (qsizetype i = 0; i < n; ++i) {
        appendAddress(out, targets.at(i));
        appendWord(out, 1); // allowFailure: one failing token call must not void the whole batch
        appendWord(out, quint64(3 * kWord)); // callData follows the three head words of the tuple
        appendWord(out, quint64(calls.at(i).callData.size()));
        appendPadded(out, calls.at(i).callData);
    }
    return out;
}

bool decodeAggregate3(const QByteArray& returnData, QList<Result>* out)
{
    if (!out) return false;
    out->clear();

    // The return value is one dynamic array: the first word is its offset.
    qsizetype arrayAt = 0;
    qsizetype count = 0;
    if (!readSize(returnData, 0, &arrayAt) || !readSize(returnData, arrayAt, &count)) return false;
    const qsizetype base = arrayAt + kWord; // element offsets count from here
    // Each element needs at least its head word; checking that before reserve()
    // keeps a forged count from allocating anything.
    if (count > (returnData.size() - base) / kWord) return false;

    QList<Result> results;
    results.reserve(count);
    for (qsizetype i = 0; i < count; ++i) {
        qsizetype tupleRel = 0;
        if (!readSize(returnData, base + i * kWord, &tupleRel)) return false;
        const qsizetype tuple = base + tupleRel;

        // (bool success, bytes returnData): a strict 0/1 flag, then the offset
        // of the bytes counted from the tuple start.
        quint64 flag = 0;
        qsizetype dataRel = 0;
        if (!readWord(returnData, tuple, &flag) || flag > 1) return false;
        if (!readSize(returnData, tuple + kWord, &dataRel)) return false;

        const qsizetype lengthAt = tuple + dataRel;
        qsizetype length = 0;
        if (!readSize(returnData, lengthAt, &length)) return false;
        const qsizetype dataAt = lengthAt + kWord;
        if (length > returnData.size() - dataAt) return false;

        Result r;
        r.success = flag == 1;
        r.returnData = returnData.mid(dataAt, length);
        results.append(r);
    }
    *out = results;
    return true;
}

QByteArray encodeGetEthBalance(const QString& address)
{
    return encodeAddressCall(getEthBalanceSelector(), address);
}

QByteArray encodeBalanceOf(const QString& address)
{
    return encodeAddressCall(balanceOfSelector(), address);
}

QString toHex(const QByteArray& bytes)
{
    return QStringLiteral("0x") + QString::fromLatin1(bytes.toHex());
}

bool fromHex(const QString& hex, QByteArray* out)
{
    if (!out) return false;
    QStringView digits(hex);
    if (digits.startsWith(QLatin1String("0x")) || digits.startsWith(QLatin1String("0X")))
        digits = digits.mid(2);
    QByteArray bytes;
    if (!decodeHexDigits(digits, &bytes)) {
        out->clear();
        return false;
    }
    *out = bytes;
    return true;
}

} // namespace net::multicall
