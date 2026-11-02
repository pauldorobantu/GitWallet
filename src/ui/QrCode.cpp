#include "ui/QrCode.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

namespace app::ui {

namespace {

constexpr int kQuietModules = 4;
constexpr int kMasks = 8;

// Per version, 1 to 40: the error correction codewords of a block, and how many blocks there are.
constexpr std::array<std::array<quint8, QrCode::kMaxVersion + 1>, 4> kBlockCorrection = {{
    {0, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28,
     28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
     26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
    {0, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30,
     28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {0, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28,
     30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
}};
constexpr std::array<std::array<quint8, QrCode::kMaxVersion + 1>, 4> kBlocks = {{
    {0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8,
     8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
    {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16,
     17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
    {0, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20,
     23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
    {0, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25,
     25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},
}};

// The two bits that name a correction level in the format information.
int formatBits(QrCode::Correction correction)
{
    switch (correction) {
    case QrCode::Correction::Low: return 1;
    case QrCode::Correction::Medium: return 0;
    case QrCode::Correction::Quartile: return 3;
    case QrCode::Correction::High: return 2;
    }
    return 0;
}

// Modules a symbol has for data and correction, once its fixed patterns are taken out.
int dataModules(int version)
{
    int modules = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const int aligned = version / 7 + 2;
        modules -= (25 * aligned - 10) * aligned - 55;
        if (version >= 7)
            modules -= 36;
    }
    return modules;
}

int dataCodewords(int version, QrCode::Correction correction)
{
    const int level = int(correction);
    return dataModules(version) / 8 - kBlockCorrection[level][version] * kBlocks[level][version];
}

int countBits(int version)
{
    return version < 10 ? 8 : 16;
}

// Multiplication in the field the error correction works over, reduced by x^8 + x^4 + x^3 + x^2 + 1.
quint8 multiply(quint8 a, quint8 b)
{
    int result = 0;
    for (int bit = 7; bit >= 0; --bit) {
        result = (result << 1) ^ ((result >> 7) * 0x11D);
        result ^= ((b >> bit) & 1) * a;
    }
    return quint8(result);
}

QVector<quint8> generator(int degree)
{
    QVector<quint8> out(degree, 0);
    out[degree - 1] = 1;
    quint8 root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            out[j] = multiply(out[j], root);
            if (j + 1 < degree)
                out[j] ^= out[j + 1];
        }
        root = multiply(root, 0x02);
    }
    return out;
}

QVector<quint8> remainder(const quint8* data, int length, const QVector<quint8>& divisor)
{
    QVector<quint8> out(divisor.size(), 0);
    for (int i = 0; i < length; ++i) {
        const quint8 factor = data[i] ^ out.first();
        out.removeFirst();
        out.append(0);
        for (int j = 0; j < divisor.size(); ++j)
            out[j] ^= multiply(divisor[j], factor);
    }
    return out;
}

QVector<int> alignmentCentres(int version)
{
    if (version == 1)
        return {};
    const int count = version / 7 + 2;
    const int step = version == 32 ? 26 : (version * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
    QVector<int> out(count, 6);
    int position = version * 4 + 17 - 7;
    for (int i = count - 1; i >= 1; --i, position -= step)
        out[i] = position;
    return out;
}

// The symbol as it is being drawn: what is fixed goes in first and is marked so, and masks pass it by.
struct Canvas {
    int size = 0;
    QVector<quint8> dark;
    QVector<quint8> fixed;

    explicit Canvas(int side) : size(side), dark(side * side, 0), fixed(side * side, 0) {}

    void put(int x, int y, bool value)
    {
        dark[y * size + x] = value ? 1 : 0;
        fixed[y * size + x] = 1;
    }
    bool isFixed(int x, int y) const { return fixed.at(y * size + x) != 0; }
};

void drawFinder(Canvas& c, int centreX, int centreY)
{
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            const int x = centreX + dx;
            const int y = centreY + dy;
            if (x < 0 || y < 0 || x >= c.size || y >= c.size)
                continue;
            const int ring = std::max(std::abs(dx), std::abs(dy));
            c.put(x, y, ring != 2 && ring != 4);
        }
    }
}

void drawAlignment(Canvas& c, int centreX, int centreY)
{
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx)
            c.put(centreX + dx, centreY + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }
}

void drawFormat(Canvas& c, QrCode::Correction correction, int mask)
{
    const int data = formatBits(correction) << 3 | mask;
    int rest = data;
    for (int i = 0; i < 10; ++i)
        rest = (rest << 1) ^ ((rest >> 9) * 0x537);
    const int bits = (data << 10 | rest) ^ 0x5412;
    const auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };

    for (int i = 0; i <= 5; ++i)
        c.put(8, i, bit(i));
    c.put(8, 7, bit(6));
    c.put(8, 8, bit(7));
    c.put(7, 8, bit(8));
    for (int i = 9; i < 15; ++i)
        c.put(14 - i, 8, bit(i));

    for (int i = 0; i < 8; ++i)
        c.put(c.size - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i)
        c.put(8, c.size - 15 + i, bit(i));
    c.put(8, c.size - 8, true); // the one module that is always dark
}

void drawVersion(Canvas& c, int version)
{
    if (version < 7)
        return;
    int rest = version;
    for (int i = 0; i < 12; ++i)
        rest = (rest << 1) ^ ((rest >> 11) * 0x1F25);
    const int bits = version << 12 | rest;
    for (int i = 0; i < 18; ++i) {
        const bool value = ((bits >> i) & 1) != 0;
        const int a = c.size - 11 + i % 3;
        const int b = i / 3;
        c.put(a, b, value);
        c.put(b, a, value);
    }
}

void drawFixed(Canvas& c, int version, QrCode::Correction correction)
{
    for (int i = 0; i < c.size; ++i) {
        c.put(6, i, i % 2 == 0);
        c.put(i, 6, i % 2 == 0);
    }
    drawFinder(c, 3, 3);
    drawFinder(c, c.size - 4, 3);
    drawFinder(c, 3, c.size - 4);
    const QVector<int> centres = alignmentCentres(version);
    for (int i = 0; i < centres.size(); ++i) {
        for (int j = 0; j < centres.size(); ++j) {
            // Not over a finder: the three corners that have one.
            const bool corner = (i == 0 && j == 0) || (i == 0 && j == centres.size() - 1) || (i == centres.size() - 1 && j == 0);
            if (!corner)
                drawAlignment(c, centres[i], centres[j]);
        }
    }
    drawFormat(c, correction, 0); // only to mark its modules: the mask chosen writes them again
    drawVersion(c, version);
}

void drawCodewords(Canvas& c, const QVector<quint8>& codewords)
{
    // In pairs of columns from the right, up and down in turn, past column 6 with its timing pattern.
    int position = 0;
    const int bits = int(codewords.size()) * 8;
    for (int right = c.size - 1; right >= 1; right -= 2) {
        if (right == 6)
            right = 5;
        for (int step = 0; step < c.size; ++step) {
            for (int side = 0; side < 2; ++side) {
                const int x = right - side;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? c.size - 1 - step : step;
                if (c.isFixed(x, y))
                    continue;
                if (position < bits)
                    c.dark[y * c.size + x] = (codewords[position >> 3] >> (7 - (position & 7))) & 1;
                ++position; // what is left over reads as light
            }
        }
    }
}

bool maskFlips(int mask, int x, int y)
{
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    case 7: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
    return false;
}

void applyMask(Canvas& c, int mask)
{
    for (int y = 0; y < c.size; ++y) {
        for (int x = 0; x < c.size; ++x) {
            if (!c.isFixed(x, y) && maskFlips(mask, x, y))
                c.dark[y * c.size + x] ^= 1;
        }
    }
}

// The standard's four rules: long runs, 2x2 blocks, what looks like a finder, an uneven share of dark.
long penalty(const Canvas& c)
{
    long score = 0;
    const auto at = [&c](int x, int y) { return c.dark.at(y * c.size + x) != 0; };
    const auto line = [&](auto module) {
        int run = 0;
        bool previous = false;
        int window = 0; // the last 11 modules
        for (int i = 0; i < c.size; ++i) {
            const bool now = module(i);
            if (i > 0 && now == previous) {
                ++run;
                if (run == 5)
                    score += 3;
                else if (run > 5)
                    ++score;
            } else {
                run = 1;
                previous = now;
            }
            window = ((window << 1) | (now ? 1 : 0)) & 0x7FF;
            if (i >= 10 && (window == 0x5D0 || window == 0x05D))
                score += 40;
        }
    };
    for (int i = 0; i < c.size; ++i) {
        line([&](int x) { return at(x, i); });
        line([&](int y) { return at(i, y); });
    }
    for (int y = 0; y + 1 < c.size; ++y) {
        for (int x = 0; x + 1 < c.size; ++x) {
            const bool first = at(x, y);
            if (first == at(x + 1, y) && first == at(x, y + 1) && first == at(x + 1, y + 1))
                score += 3;
        }
    }
    const qsizetype dark = std::count(c.dark.cbegin(), c.dark.cend(), quint8(1));
    const qsizetype total = qsizetype(c.size) * c.size;
    const long fifths = long((std::abs(dark * 20 - total * 10) + total - 1) / total) - 1;
    score += std::max(fifths, 0L) * 10;
    return score;
}

QVector<quint8> withCorrection(const QVector<quint8>& data, int version, QrCode::Correction correction)
{
    const int level = int(correction);
    const int blocks = kBlocks[level][version];
    const int perBlock = kBlockCorrection[level][version];
    const int raw = dataModules(version) / 8;
    const int shortBlocks = blocks - raw % blocks;
    const int shortLength = raw / blocks;
    const QVector<quint8> divisor = generator(perBlock);

    QVector<QVector<quint8>> parts;
    QVector<QVector<quint8>> checks;
    int at = 0;
    for (int i = 0; i < blocks; ++i) {
        const int length = shortLength - perBlock + (i < shortBlocks ? 0 : 1);
        parts.append(data.mid(at, length));
        checks.append(remainder(data.constData() + at, length, divisor));
        at += length;
    }
    QVector<quint8> out;
    out.reserve(raw);
    for (int i = 0; i < shortLength - perBlock + 1; ++i) {
        for (int j = 0; j < blocks; ++j) {
            if (i < parts[j].size())
                out.append(parts[j][i]);
        }
    }
    for (int i = 0; i < perBlock; ++i) {
        for (int j = 0; j < blocks; ++j)
            out.append(checks[j][i]);
    }
    return out;
}

} // namespace

int QrCode::capacity(int version, Correction correction)
{
    if (version < 1 || version > kMaxVersion)
        return 0;
    // Four bits of mode and the count, the rest bytes.
    return (dataCodewords(version, correction) * 8 - 4 - countBits(version)) / 8;
}

int QrCode::blockCount(int version, Correction correction)
{
    return kBlocks[int(correction)][version];
}

int QrCode::blockCorrection(int version, Correction correction)
{
    return kBlockCorrection[int(correction)][version];
}

QrCode QrCode::encode(const QByteArray& data, Correction correction)
{
    int version = 1;
    while (version <= kMaxVersion && capacity(version, correction) < data.size())
        ++version;
    if (version > kMaxVersion)
        return {};

    const int room = dataCodewords(version, correction);
    QVector<quint8> codewords;
    codewords.reserve(room);
    int bits = 0;
    int held = 0;
    const auto append = [&](int value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            held = (held << 1) | ((value >> i) & 1);
            if (++bits == 8) {
                codewords.append(quint8(held));
                bits = 0;
                held = 0;
            }
        }
    };
    append(0x4, 4); // byte mode
    append(int(data.size()), countBits(version));
    for (const char byte : data)
        append(quint8(byte), 8);
    // As much of the terminator as there is room for, then whole bytes.
    const int left = room * 8 - (int(codewords.size()) * 8 + bits);
    append(0, std::min(4, left));
    if (bits > 0)
        append(0, 8 - bits);
    for (quint8 pad = 0xEC; codewords.size() < room; pad ^= 0xEC ^ 0x11)
        codewords.append(pad);

    Canvas base(version * 4 + 17);
    drawFixed(base, version, correction);
    drawCodewords(base, withCorrection(codewords, version, correction));

    Canvas best = base;
    int bestMask = 0;
    long lowest = std::numeric_limits<long>::max();
    for (int mask = 0; mask < kMasks; ++mask) {
        Canvas trial = base;
        applyMask(trial, mask);
        drawFormat(trial, correction, mask);
        const long score = penalty(trial);
        if (score < lowest) {
            lowest = score;
            best = trial;
            bestMask = mask;
        }
    }

    QrCode out;
    out.modules_ = best.dark;
    out.size_ = best.size;
    out.version_ = version;
    out.mask_ = bestMask;
    return out;
}

QImage QrCode::toImage(int scale, const QColor& dark, const QColor& light) const
{
    if (isNull() || scale < 1)
        return {};
    const int side = (size_ + 2 * kQuietModules) * scale;
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(light);
    const QRgb ink = dark.rgb();
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            if (!this->dark(x, y))
                continue;
            for (int dy = 0; dy < scale; ++dy) {
                auto* line = reinterpret_cast<QRgb*>(image.scanLine((y + kQuietModules) * scale + dy));
                std::fill_n(line + (x + kQuietModules) * scale, scale, ink);
            }
        }
    }
    return image;
}

} // namespace app::ui
