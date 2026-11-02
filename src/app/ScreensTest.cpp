#include "app/StoreTest.h"

#include "app/WalletController.h"
#include "chain/Bip39.h"
#include "core/SettingsRepo.h"
#include "net/PriceFeed.h"
#include "ui/HistoryTableModel.h"
#include "ui/Holdings.h"
#include "ui/LanguageManager.h"
#include "ui/MainWindow.h"
#include "ui/AddressTableModel.h"
#include "ui/PlaceBar.h"
#include "ui/QrCode.h"
#include "ui/TableUtil.h"
#include "ui/pages/AddressesPage.h"
#include "ui/pages/HistoryPage.h"
#include "ui/pages/HoldingsPage.h"
#include "ui/pages/PortfolioPage.h"
#include "ui/pages/ReceivePage.h"
#include "ui/pages/WalletsPage.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSet>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableView>
#include <QTableWidget>

#include <array>
#include <functional>

namespace app {

namespace {

using Check = std::function<void(bool, const QString&)>;
using ui::QrCode;

bool waitFor(const std::function<bool()>& done, int ms)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

void pause(int ms)
{
    waitFor([] { return false; }, ms);
}

// ------------------------------------------------------------- reading a symbol back ----

// Where the standard puts the alignment patterns (ISO/IEC 18004, annex E), per version, written out
// rather than worked out as the code that draws them does.
const QList<QVector<int>>& alignmentTable()
{
    static const QList<QVector<int>> table = {
        {}, {}, {6, 18}, {6, 22}, {6, 26}, {6, 30}, {6, 34}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50}, {6, 30, 54}, {6, 32, 58}, {6, 34, 62},
        {6, 26, 46, 66}, {6, 26, 48, 70}, {6, 26, 50, 74}, {6, 30, 54, 78}, {6, 30, 56, 82}, {6, 30, 58, 86}, {6, 34, 62, 90},
        {6, 28, 50, 72, 94}, {6, 26, 50, 74, 98}, {6, 30, 54, 78, 102}, {6, 28, 54, 80, 106}, {6, 32, 58, 84, 110}, {6, 30, 58, 86, 114}, {6, 34, 62, 90, 118},
        {6, 26, 50, 74, 98, 122}, {6, 30, 54, 78, 102, 126}, {6, 26, 52, 78, 104, 130}, {6, 30, 56, 82, 108, 134}, {6, 34, 60, 86, 112, 138}, {6, 30, 58, 86, 114, 142}, {6, 34, 62, 90, 118, 146},
        {6, 30, 54, 78, 102, 126, 150}, {6, 24, 50, 76, 102, 128, 154}, {6, 28, 54, 80, 106, 132, 158}, {6, 32, 58, 84, 110, 136, 162}, {6, 26, 54, 82, 110, 138, 166}, {6, 30, 58, 86, 114, 142, 170},
    };
    return table;
}

struct ReadBack {
    QByteArray data;
    QString wrong; // empty: every part of it holds
};

// In the field the error correction works over.
struct Field {
    std::array<quint8, 512> power{};
    std::array<int, 256> log{};
    Field()
    {
        int value = 1;
        for (int i = 0; i < 255; ++i) {
            power[i] = quint8(value);
            power[i + 255] = quint8(value);
            log[value] = i;
            value <<= 1;
            if (value & 0x100)
                value ^= 0x11D;
        }
    }
};

// What a reader does, from nothing but the modules: checks what is fixed, reads which level and mask
// it says it is, takes the mask off, puts the blocks back together, checks each with its own error
// correction, and reads what it says. Written apart from the code that draws them.
ReadBack readSymbol(const QrCode& code, QrCode::Correction expected)
{
    ReadBack out;
    const int size = code.size();
    const int version = (size - 17) / 4;
    if (size < 21 || (size - 17) % 4 != 0 || version > QrCode::kMaxVersion) {
        out.wrong = QStringLiteral("a side of %1").arg(size);
        return out;
    }
    QVector<quint8> fixed(size * size, 0);
    const auto mark = [&](int x, int y) { fixed[y * size + x] = 1; };

    // Finders, each with its separator, and the format information around them.
    const auto finderHolds = [&](int left, int top) {
        for (int y = 0; y < 7; ++y) {
            for (int x = 0; x < 7; ++x) {
                const bool ring = x == 0 || x == 6 || y == 0 || y == 6;
                const bool core = x >= 2 && x <= 4 && y >= 2 && y <= 4;
                if (code.dark(left + x, top + y) != (ring || core))
                    return false;
            }
        }
        return true;
    };
    if (!finderHolds(0, 0) || !finderHolds(size - 7, 0) || !finderHolds(0, size - 7)) {
        out.wrong = QStringLiteral("a finder");
        return out;
    }
    for (int y = 0; y < 9; ++y) {
        for (int x = 0; x < 9; ++x)
            mark(x, y);
        for (int x = size - 8; x < size; ++x)
            mark(x, y);
    }
    for (int y = size - 8; y < size; ++y) {
        for (int x = 0; x < 9; ++x)
            mark(x, y);
    }
    // Separators are light.
    for (int i = 0; i < 8; ++i) {
        if (code.dark(i, 7) || code.dark(7, i) || code.dark(size - 1 - i, 7) || code.dark(size - 8, i) || code.dark(i, size - 8) || code.dark(7, size - 1 - i)) {
            out.wrong = QStringLiteral("a separator");
            return out;
        }
    }
    if (!code.dark(8, size - 8)) {
        out.wrong = QStringLiteral("the module that is always dark");
        return out;
    }
    for (int i = 8; i < size - 8; ++i) {
        if (code.dark(i, 6) != (i % 2 == 0) || code.dark(6, i) != (i % 2 == 0)) {
            out.wrong = QStringLiteral("the timing pattern");
            return out;
        }
    }
    for (int i = 0; i < size; ++i) {
        mark(i, 6);
        mark(6, i);
    }
    const QVector<int>& centres = alignmentTable().at(version);
    for (int i = 0; i < centres.size(); ++i) {
        for (int j = 0; j < centres.size(); ++j) {
            const int cx = centres[i];
            const int cy = centres[j];
            if ((cx == 6 && cy == 6) || (cx == 6 && cy == size - 7) || (cx == size - 7 && cy == 6))
                continue;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    if (code.dark(cx + dx, cy + dy) != (qMax(qAbs(dx), qAbs(dy)) != 1)) {
                        out.wrong = QStringLiteral("an alignment pattern at %1,%2").arg(cx).arg(cy);
                        return out;
                    }
                    mark(cx + dx, cy + dy);
                }
            }
        }
    }
    if (version >= 7) {
        // Twice, with a check of its own.
        int first = 0;
        int second = 0;
        for (int i = 0; i < 18; ++i) {
            first |= int(code.dark(size - 11 + i % 3, i / 3)) << i;
            second |= int(code.dark(i / 3, size - 11 + i % 3)) << i;
            mark(size - 11 + i % 3, i / 3);
            mark(i / 3, size - 11 + i % 3);
        }
        int rest = first;
        for (int bit = 17; bit >= 12; --bit) {
            if (rest & (1 << bit))
                rest ^= 0x1F25 << (bit - 12);
        }
        if (first != second || rest != 0 || (first >> 12) != version) {
            out.wrong = QStringLiteral("the version information");
            return out;
        }
    }

    // Twice too.
    int format = 0;
    int formatAgain = 0;
    for (int i = 0; i <= 5; ++i)
        format |= int(code.dark(8, i)) << i;
    format |= int(code.dark(8, 7)) << 6;
    format |= int(code.dark(8, 8)) << 7;
    format |= int(code.dark(7, 8)) << 8;
    for (int i = 9; i < 15; ++i)
        format |= int(code.dark(14 - i, 8)) << i;
    for (int i = 0; i < 8; ++i)
        formatAgain |= int(code.dark(size - 1 - i, 8)) << i;
    for (int i = 8; i < 15; ++i)
        formatAgain |= int(code.dark(8, size - 15 + i)) << i;
    if (format != formatAgain) {
        out.wrong = QStringLiteral("the format information differs between its copies");
        return out;
    }
    format ^= 0x5412;
    int rest = format;
    for (int bit = 14; bit >= 10; --bit) {
        if (rest & (1 << bit))
            rest ^= 0x537 << (bit - 10);
    }
    if (rest != 0) {
        out.wrong = QStringLiteral("the format information does not check");
        return out;
    }
    const int levelBits = format >> 13;
    const int wantedBits = expected == QrCode::Correction::Low ? 1 : expected == QrCode::Correction::Medium ? 0 : expected == QrCode::Correction::Quartile ? 3 : 2;
    if (levelBits != wantedBits) {
        out.wrong = QStringLiteral("another correction level");
        return out;
    }
    const int mask = (format >> 10) & 7;
    if (mask != code.mask()) {
        out.wrong = QStringLiteral("another mask");
        return out;
    }
    const auto flips = [mask](int x, int y) {
        switch (mask) {
        case 0: return (y + x) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (y + x) % 3 == 0;
        case 4: return (y / 2 + x / 3) % 2 == 0;
        case 5: return (y * x) % 2 + (y * x) % 3 == 0;
        case 6: return ((y * x) % 2 + (y * x) % 3) % 2 == 0;
        default: return ((y + x) % 2 + (y * x) % 3) % 2 == 0;
        }
    };

    // Up and down the pairs of columns from the right, past the vertical timing pattern.
    QVector<quint8> raw;
    int held = 0;
    int bits = 0;
    bool upward = true;
    for (int right = size - 1; right > 0; right -= 2) {
        if (right == 6)
            --right;
        for (int step = 0; step < size; ++step) {
            const int y = upward ? size - 1 - step : step;
            for (int x = right; x >= right - 1; --x) {
                if (fixed.at(y * size + x))
                    continue;
                held = (held << 1) | int(code.dark(x, y) != flips(x, y));
                if (++bits == 8) {
                    raw.append(quint8(held));
                    held = 0;
                    bits = 0;
                }
            }
        }
        upward = !upward;
    }

    const int blocks = QrCode::blockCount(version, expected);
    const int check = QrCode::blockCorrection(version, expected);
    const int total = int(raw.size());
    const int shortBlocks = blocks - total % blocks;
    const int shortData = total / blocks - check;
    QVector<QVector<quint8>> parts(blocks);
    int at = 0;
    for (int i = 0; i < shortData + 1; ++i) {
        for (int b = 0; b < blocks; ++b) {
            if (i < shortData || b >= shortBlocks)
                parts[b].append(raw[at++]);
        }
    }
    for (int i = 0; i < check; ++i) {
        for (int b = 0; b < blocks; ++b)
            parts[b].append(raw[at++]);
    }
    static const Field field;
    QVector<quint8> data;
    for (int b = 0; b < blocks; ++b) {
        // A code word of it is a multiple of the generator: every root gives nothing.
        for (int root = 0; root < check; ++root) {
            int sum = 0;
            for (const quint8 value : std::as_const(parts[b]))
                sum = (sum == 0 ? 0 : field.power[field.log[sum] + root]) ^ value;
            if (sum != 0) {
                out.wrong = QStringLiteral("block %1 does not check").arg(b);
                return out;
            }
        }
        data += parts[b].mid(0, parts[b].size() - check);
    }

    int position = 0;
    const auto take = [&](int count) {
        int value = 0;
        for (int i = 0; i < count; ++i, ++position)
            value = (value << 1) | ((data[position >> 3] >> (7 - (position & 7))) & 1);
        return value;
    };
    if (take(4) != 0x4) {
        out.wrong = QStringLiteral("not byte mode");
        return out;
    }
    const int length = take(version < 10 ? 8 : 16);
    if (position + length * 8 > data.size() * 8) {
        out.wrong = QStringLiteral("more than it holds");
        return out;
    }
    for (int i = 0; i < length; ++i)
        out.data.append(char(take(8)));
    // What is left: the terminator, whole bytes, then the two padding bytes in turn.
    const int capacity = int(data.size()) * 8;
    if (take(qMin(4, capacity - position)) != 0) {
        out.wrong = QStringLiteral("no terminator");
        return out;
    }
    if (position % 8 != 0 && take(8 - position % 8) != 0) {
        out.wrong = QStringLiteral("bits left over");
        return out;
    }
    for (int pad = 0; position < capacity; pad ^= 1) {
        if (take(8) != (pad == 0 ? 0xEC : 0x11)) {
            out.wrong = QStringLiteral("wrong padding");
            return out;
        }
    }
    return out;
}

void testQr(const Check& check, QString* report)
{
    using Level = QrCode::Correction;
    // The standard's figures for bytes.
    check(QrCode::capacity(1, Level::Low) == 17 && QrCode::capacity(1, Level::Medium) == 14 && QrCode::capacity(1, Level::Quartile) == 11 && QrCode::capacity(1, Level::High) == 7,
          QStringLiteral("the smallest QR code holds what the standard says"));
    check(QrCode::capacity(5, Level::Medium) == 84 && QrCode::capacity(10, Level::Medium) == 213 && QrCode::capacity(40, Level::Low) == 2953 && QrCode::capacity(40, Level::High) == 1273,
          QStringLiteral("and so do the larger ones"));

    QElapsedTimer clock;
    clock.start();
    QRandomGenerator random(20261102);
    int read = 0;
    QStringList wrong;
    QSet<int> masks;
    QSet<int> versions;
    for (const Level level : {Level::Low, Level::Medium, Level::Quartile, Level::High}) {
        for (int version = 1; version <= QrCode::kMaxVersion; ++version) {
            QByteArray bytes(QrCode::capacity(version, level), Qt::Uninitialized);
            for (char& byte : bytes)
                byte = char(random.bounded(256));
            const QrCode code = QrCode::encode(bytes, level);
            if (code.version() != version || code.size() != version * 4 + 17) {
                wrong << QStringLiteral("version %1 full is %2").arg(version).arg(code.version());
                continue;
            }
            const ReadBack back = readSymbol(code, level);
            if (!back.wrong.isEmpty() || back.data != bytes)
                wrong << QStringLiteral("version %1 level %2: %3").arg(version).arg(int(level)).arg(back.wrong.isEmpty() ? QStringLiteral("other bytes") : back.wrong);
            else
                ++read;
            masks.insert(code.mask());
            versions.insert(code.version());
            if (version < QrCode::kMaxVersion && QrCode::encode(bytes + 'x', level).version() != version + 1)
                wrong << QStringLiteral("a byte past version %1 does not go up").arg(version);
        }
    }
    check(wrong.isEmpty(), QStringLiteral("every version at every level, full, reads back as it was: %1").arg(wrong.mid(0, 4).join(QStringLiteral("; "))));
    check(read == 160 && versions.size() == 40, QStringLiteral("all of them (%1)").arg(read));
    check(masks.size() == 8, QStringLiteral("through every mask (%1)").arg(masks.size()));
    report->append(QStringLiteral("info  160 QR codes written and read back in %1 ms\n").arg(clock.elapsed()));

    check(QrCode::encode(QByteArray(QrCode::capacity(40, Level::Medium) + 1, 'x')).isNull(), QStringLiteral("what fits no symbol gives none, not a cut one"));
    const QrCode none = QrCode::encode({});
    check(!none.isNull() && readSymbol(none, Level::Medium).wrong.isEmpty() && readSymbol(none, Level::Medium).data.isEmpty(), QStringLiteral("nothing is a symbol too"));

    // Dark on white, with its quiet zone, a module a square.
    const QByteArray address = QByteArrayLiteral("bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");
    const QrCode code = QrCode::encode(address);
    const ReadBack back = readSymbol(code, Level::Medium);
    check(back.wrong.isEmpty() && back.data == address, QStringLiteral("an address reads back: %1").arg(back.wrong));
    const int scale = 3;
    const QImage image = code.toImage(scale);
    bool same = image.width() == (code.size() + 8) * scale && image.height() == image.width();
    for (int y = 0; same && y < code.size(); ++y) {
        for (int x = 0; x < code.size(); ++x) {
            for (const auto& corner : {std::pair{0, 0}, std::pair{scale - 1, scale - 1}}) {
                const QRgb pixel = image.pixel((x + 4) * scale + corner.first, (y + 4) * scale + corner.second);
                same = same && pixel == (code.dark(x, y) ? QColor(Qt::black).rgb() : QColor(Qt::white).rgb());
            }
        }
    }
    for (int i = 0; same && i < image.width(); ++i)
        same = image.pixel(i, 0) == QColor(Qt::white).rgb() && image.pixel(0, i) == QColor(Qt::white).rgb() && image.pixel(i, image.height() - 1) == QColor(Qt::white).rgb() && image.pixel(image.width() - 1, i) == QColor(Qt::white).rgb();
    check(same, QStringLiteral("and its picture is exactly its modules, in a white border"));
}

// ------------------------------------------------------------- a wallet to look at ----

QString addressAt(const wallet::AccountKey& key, quint32 index, wallet::Network network)
{
    return QString::fromStdString(chain::addressFromChainNode(key.node().deriveChild(0), key.type, key.account, 0, index, wallet::networkInfo(network).format).address);
}

// No server answers and no price source is asked: what the figures say is what a test wrote.
void offline(WalletController* controller)
{
    controller->prices()->useTestServer(QStringLiteral("http://127.0.0.1:1"));
    wallet::ScanSettings settings = controller->store().settings;
    settings.utxoReceiveGap = 5;
    settings.utxoChangeGap = 2;
    settings.evmGap = 4;
    for (const wallet::NetworkInfo& network : wallet::networks())
        settings.servers.insert(network.id, {network.family == wallet::Family::Utxo ? QStringLiteral("127.0.0.1:1") : QStringLiteral("https://127.0.0.1:1")});
    QString error;
    controller->applySettings(settings, &error);
}

void setPrices(WalletController* controller)
{
    QList<net::PriceQuote> quotes;
    for (const wallet::AssetInfo& info : wallet::assets()) {
        net::PriceQuote quote;
        quote.asset = info.asset;
        quote.usd = info.dollar ? 1.0 : (info.symbol == QLatin1String("BTC") ? 100000.0 : 2000.0);
        quote.at = QDateTime::currentDateTimeUtc();
        quote.source = QStringLiteral("Test");
        quotes.append(quote);
    }
    controller->prices()->restore(quotes);
}

// Every row of a wallet as a server would have it: checked, with nothing on it.
void answerAll(wallet::WalletModel* model, quint32 wallet)
{
    for (int i = 0; i < model->size(); ++i) {
        if (model->at(i).wallet != wallet || model->at(i).checked)
            continue;
        wallet::AddressRow row = model->at(i);
        row.checked = true;
        model->upsert(row);
    }
}

// ------------------------------------------------------------- what is given out ----

void testGivenOutRuns(const Check& check)
{
    wallet::GivenOut given;
    check(given.isEmpty() && given.end() == 0 && given.nextFree(5) == 5 && !given.contains(0), QStringLiteral("nothing given out holds nothing"));
    for (const quint32 index : {5u, 7u, 6u, 9u, 20u, 3u, 7u})
        given.add(index);
    const auto runsAre = [&given](const QList<QPair<quint32, quint32>>& wanted) {
        if (given.runs.size() != wanted.size())
            return false;
        for (qsizetype i = 0; i < wanted.size(); ++i) {
            if (given.runs.at(i).from != wanted.at(i).first || given.runs.at(i).to != wanted.at(i).second)
                return false;
        }
        return true;
    };
    check(runsAre({{3, 4}, {5, 8}, {9, 10}, {20, 21}}), QStringLiteral("runs join and stay in order, and one added twice is one"));
    given.add(4);
    check(runsAre({{3, 8}, {9, 10}, {20, 21}}), QStringLiteral("one between two runs makes them one"));
    given.add(2);
    check(runsAre({{2, 8}, {9, 10}, {20, 21}}), QStringLiteral("one before a run joins it"));
    check(given.end() == 21, QStringLiteral("the highest is known"));
    check(given.contains(2) && given.contains(7) && !given.contains(8) && given.contains(9) && !given.contains(10) && !given.contains(1) && !given.contains(21), QStringLiteral("and which are in"));
    check(given.nextFree(0) == 0 && given.nextFree(2) == 8 && given.nextFree(5) == 8 && given.nextFree(8) == 8 && given.nextFree(9) == 10 && given.nextFree(21) == 21, QStringLiteral("and what is free after each"));

    // Random against a plain set: what add() makes is what any order of adding makes.
    QRandomGenerator random(7);
    wallet::GivenOut many;
    QSet<quint32> plain;
    for (int i = 0; i < 2000; ++i) {
        const quint32 index = random.bounded(400u);
        many.add(index);
        plain.insert(index);
    }
    bool same = true;
    quint32 last = 0;
    for (qsizetype i = 0; i < many.runs.size(); ++i) {
        same = same && many.runs.at(i).from < many.runs.at(i).to && (i == 0 || many.runs.at(i).from > last);
        last = many.runs.at(i).to;
    }
    for (quint32 index = 0; index < 420; ++index) {
        same = same && many.contains(index) == plain.contains(index);
        quint32 free = index;
        while (plain.contains(free))
            ++free;
        same = same && many.nextFree(index) == free;
    }
    check(same, QStringLiteral("in any order they are what a plain list of them says"));
}

void testGivenOutFile(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    QFile::remove(wallet::WalletStore::filePath());
    const auto writeWith = [&](const QJsonValue& given) {
        QJsonObject account{{QStringLiteral("type"), QStringLiteral("p2wpkh")}, {QStringLiteral("network"), QStringLiteral("bitcoin")}, {QStringLiteral("account"), 0}, {QStringLiteral("xpub"), keys.first().xpub}};
        if (!given.isNull())
            account.insert(QStringLiteral("given"), given);
        const QJsonObject root{{QStringLiteral("format"), QStringLiteral("gitwallet")},
                               {QStringLiteral("version"), 4},
                               {QStringLiteral("nextWalletId"), 2},
                               {QStringLiteral("wallets"), QJsonArray{QJsonObject{{QStringLiteral("id"), 1}, {QStringLiteral("label"), QStringLiteral("Tills")}, {QStringLiteral("accounts"), QJsonArray{account}}}}},
                               {QStringLiteral("settings"), QJsonObject{}}};
        return wallet::WalletStore::writeRawForTest(QJsonDocument(root).toJson(QJsonDocument::Compact), &error);
    };
    {
        check(writeWith(QJsonArray{QJsonArray{2, 5}, QJsonArray{9, 10}}), QStringLiteral("a file that says what was given out"));
        wallet::WalletStore store;
        check(store.load(&error) && store.wallets.first().accounts.first().given.runs.size() == 2 && store.wallets.first().accounts.first().given.contains(4) && !store.wallets.first().accounts.first().given.contains(5),
              QStringLiteral("reads as it was written: %1").arg(error));
        check(store.save(&error), QStringLiteral("and is written again"));
        wallet::WalletStore again;
        check(again.load(&error) && again.wallets.first().accounts.first().given.end() == 10 && again.wallets.first().accounts.first().given.runs.size() == 2, QStringLiteral("to the same"));
    }
    for (const QJsonValue& bad : {QJsonValue(QJsonArray{QJsonArray{5, 2}}), QJsonValue(QJsonArray{QJsonArray{5, 5}}), QJsonValue(QJsonArray{QJsonArray{2, 5}, QJsonArray{5, 9}}), QJsonValue(QJsonArray{QJsonArray{9, 12}, QJsonArray{2, 5}}),
                                  QJsonValue(QJsonArray{QJsonArray{1}}), QJsonValue(QJsonArray{QJsonArray{-1, 3}}), QJsonValue(QJsonArray{QJsonArray{0, 3000000000.0}})}) {
        writeWith(bad);
        wallet::WalletStore store;
        check(!store.load(&error), QStringLiteral("a list no run of the app makes is damage: %1").arg(QString::fromUtf8(QJsonDocument(bad.toArray()).toJson(QJsonDocument::Compact))));
    }
    QFile::remove(wallet::WalletStore::filePath());

    // A file of the format before keeps a key it does not know, and reads as older.
    {
        const QJsonObject root{{QStringLiteral("format"), QStringLiteral("gitwallet")},
                               {QStringLiteral("version"), 3},
                               {QStringLiteral("nextWalletId"), 2},
                               {QStringLiteral("wallets"), QJsonArray{QJsonObject{{QStringLiteral("id"), 1}, {QStringLiteral("label"), QStringLiteral("Tills")}, {QStringLiteral("accounts"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("p2tr")}, {QStringLiteral("account"), 0}, {QStringLiteral("xpub"), QStringLiteral("something new")}}, QJsonObject{{QStringLiteral("type"), QStringLiteral("p2wpkh")}, {QStringLiteral("network"), QStringLiteral("bitcoin")}, {QStringLiteral("account"), 0}, {QStringLiteral("xpub"), keys.first().xpub}}}}}}},
                               {QStringLiteral("settings"), QJsonObject{}}};
        check(wallet::WalletStore::writeRawForTest(QJsonDocument(root).toJson(QJsonDocument::Compact), &error), QStringLiteral("a file of the format before"));
        wallet::WalletStore store;
        check(store.load(&error) && store.wallets.first().keptAccounts.size() == 1 && store.olderFormat() == 3, QStringLiteral("opens as older, with the key it does not know kept: %1").arg(error));
    }
    // Two builds of that format wrote one stretch: what they gave out is still given out.
    {
        const auto writeIssued = [&](const QJsonValue& issued, const QJsonValue& from) {
            QJsonObject account{{QStringLiteral("type"), QStringLiteral("p2wpkh")}, {QStringLiteral("network"), QStringLiteral("bitcoin")}, {QStringLiteral("account"), 0}, {QStringLiteral("xpub"), keys.first().xpub}, {QStringLiteral("issued"), issued}};
            if (!from.isNull())
                account.insert(QStringLiteral("issuedFrom"), from);
            const QJsonObject root{{QStringLiteral("format"), QStringLiteral("gitwallet")},
                                   {QStringLiteral("version"), 3},
                                   {QStringLiteral("nextWalletId"), 2},
                                   {QStringLiteral("wallets"), QJsonArray{QJsonObject{{QStringLiteral("id"), 1}, {QStringLiteral("label"), QStringLiteral("Tills")}, {QStringLiteral("accounts"), QJsonArray{account}}}}},
                                   {QStringLiteral("settings"), QJsonObject{}}};
            return wallet::WalletStore::writeRawForTest(QJsonDocument(root).toJson(QJsonDocument::Compact), &error);
        };
        check(writeIssued(400, 2), QStringLiteral("a file with what was given out as one stretch"));
        wallet::WalletStore store;
        check(store.load(&error) && store.wallets.first().accounts.first().given.contains(2) && store.wallets.first().accounts.first().given.contains(399) && !store.wallets.first().accounts.first().given.contains(1) && store.wallets.first().accounts.first().given.end() == 400,
              QStringLiteral("loses none of it: %1").arg(error));
        check(store.save(&error), QStringLiteral("and is written again"));
        wallet::WalletStore again;
        check(again.load(&error) && again.olderFormat() == 0 && again.wallets.first().accounts.first().given.end() == 400, QStringLiteral("in the new format, the same"));
        writeIssued(0, QJsonValue());
        wallet::WalletStore none;
        check(none.load(&error) && none.wallets.first().accounts.first().given.isEmpty(), QStringLiteral("none given out is none"));
        writeIssued(4, 9);
        wallet::WalletStore bad;
        check(!bad.load(&error), QStringLiteral("and a stretch that ends before it begins is damage"));
    }
    QFile::remove(wallet::WalletStore::filePath());
}

void testGivenOut(const Check& check, const QList<wallet::AccountKey>& keys)
{
    const auto ofType = [&keys](chain::ScriptType type) {
        for (const wallet::AccountKey& key : keys) {
            if (key.type == type)
                return key;
        }
        return wallet::AccountKey();
    };
    const wallet::AccountKey segwit = ofType(chain::ScriptType::P2WPKH);
    const wallet::AccountKey account = ofType(chain::ScriptType::EthAccount);
    const wallet::Network bitcoin = segwit.network;
    const wallet::Network ethereum = wallet::findNetwork(QStringLiteral("ethereum"))->network;

    QString error;
    auto controller = std::make_unique<WalletController>();
    offline(controller.get());
    QList<wallet::AccountKey> added{segwit, account};
    if (!controller->addKeysWallet(added, QStringLiteral("Tills"), &error)) {
        check(false, QStringLiteral("a wallet for the test: %1").arg(error));
        return;
    }
    wallet::WalletModel* model = controller->model();
    const quint32 id = controller->store().wallets.first().id;
    check(waitFor([&] { return !controller->loadingSaved(); }, 15000), QStringLiteral("what the database kept has been read"));
    check(controller->receiveKeys(id, bitcoin).size() == 1 && controller->receiveKeys(id, ethereum).size() == 1, QStringLiteral("this wallet can be paid on one key per network"));
    check(controller->nextReceive(id, bitcoin, chain::ScriptType::P2PKH).state == wallet::ReceiveOffer::State::NoKey, QStringLiteral("and has no key of another kind to offer"));

    const auto rowAt = [&](quint32 index, chain::ScriptType type = chain::ScriptType::P2WPKH) {
        const wallet::Network network = type == chain::ScriptType::EthAccount ? ethereum : bitcoin;
        return model->find(id, network, type, 0, 0, index);
    };
    check(waitFor([&] { return rowAt(4) >= 0; }, 10000), QStringLiteral("its rows are derived with no server"));
    const auto giveOut = [&](quint32 index) {
        const int at = rowAt(index);
        return at >= 0 && controller->markGivenOut(model->at(at), &error);
    };
    wallet::ReceiveOffer offer = controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH);
    check(offer.state == wallet::ReceiveOffer::State::Checking && offer.address.isEmpty(), QStringLiteral("with no server to answer, nobody can say one is unused, and none is offered"));

    // A server answers: the first is used, the second is not.
    wallet::AddressRow row = model->at(rowAt(0));
    row.checked = true;
    row.used = true;
    model->upsert(row);
    offer = controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH);
    check(offer.state == wallet::ReceiveOffer::State::Checking, QStringLiteral("the next has no answer yet: still none"));
    answerAll(model, id);
    offer = controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH);
    check(offer.state == wallet::ReceiveOffer::State::Ready && offer.index == 1, QStringLiteral("a used address is passed over: the next one (%1)").arg(offer.index));
    check(offer.address == addressAt(segwit, 1, bitcoin), QStringLiteral("and it is what the key gives"));
    check(offer.path == QLatin1String("m/84'/0'/0'/0/1"), QStringLiteral("at its own path (%1)").arg(offer.path));

    // Funded with no history flag counts too: a balance is a use.
    row = model->at(rowAt(1));
    row.confirmed = 500;
    model->upsert(row);
    check(controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH).index == 2, QStringLiteral("what holds something is used, flag or no flag"));
    row.confirmed = 0;
    model->upsert(row);

    // What is kept is not trusted.
    row = model->at(rowAt(1));
    const QString rightAddress = row.address;
    row.address = addressAt(segwit, 7, bitcoin);
    row.scripthash = QStringLiteral("aa");
    model->upsert(row);
    offer = controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH);
    check(offer.state == wallet::ReceiveOffer::State::Failed && offer.address.isEmpty() && !offer.why.isEmpty(), QStringLiteral("a row that says another address for that index: none is offered, and it says why"));
    row.address = rightAddress;
    row.scripthash = QString::fromStdString(chain::addressFromChainNode(segwit.node().deriveChild(0), segwit.type, segwit.account, 0, 1, wallet::networkInfo(bitcoin).format).electrumScripthash);
    model->upsert(row);

    // Given out: never again, in any order of asking.
    QSet<QString> offered;
    for (quint32 expected = 1; expected < 4; ++expected) {
        offer = controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH);
        check(offer.state == wallet::ReceiveOffer::State::Ready && offer.index == expected && !offered.contains(offer.address), QStringLiteral("address %1 comes up once").arg(expected));
        offered.insert(offer.address);
        check(giveOut(offer.index), QStringLiteral("and it is written down: %1").arg(error));
    }
    check(giveOut(1) && controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH).index == 4, QStringLiteral("saying so again of one moves nothing"));
    const QList<int> given = controller->givenOutRows(id, bitcoin, chain::ScriptType::P2WPKH, 10);
    check(given.size() == 3 && model->at(given.first()).index == 3 && model->at(given.last()).index == 1, QStringLiteral("what was given out is listed, the latest first, and what was used before it is not (%1)").arg(given.size()));
    check(controller->givenOutRows(id, bitcoin, chain::ScriptType::P2WPKH, 2).size() == 2, QStringLiteral("as far as asked"));
    check(controller->isGivenOut(model->at(rowAt(2))) && !controller->isGivenOut(model->at(rowAt(0))) && !controller->isGivenOut(model->at(rowAt(4))), QStringLiteral("and each can say if it was"));

    // Only what is unused and of a key Receive goes by: others are no one's to be given.
    {
        const int before = int(controller->givenOutRows(id, bitcoin, chain::ScriptType::P2WPKH, 1000).size());
        wallet::AddressRow used = model->at(rowAt(0));
        check(controller->markGivenOut(used, &error) && !controller->isGivenOut(used), QStringLiteral("what has history is not marked"));
        const int changeRow = model->find(id, bitcoin, chain::ScriptType::P2WPKH, 0, 1, 0);
        check(changeRow >= 0 && controller->markGivenOut(model->at(changeRow), &error), QStringLiteral("a change address asks nothing"));
        check(!controller->isGivenOut(model->at(changeRow)) && int(controller->givenOutRows(id, bitcoin, chain::ScriptType::P2WPKH, 1000).size()) == before, QStringLiteral("and does not count as its receive address of that number"));
    }
    // One out of order marks that one, and nothing around it.
    check(waitFor([&] { return rowAt(10) >= 0; }, 10000), QStringLiteral("rows further on"));
    answerAll(model, id);
    check(giveOut(10), QStringLiteral("an address further on is given out from the Addresses page"));
    check(controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH).index == 4, QStringLiteral("the next one is still the first free"));
    check(controller->givenOutRows(id, bitcoin, chain::ScriptType::P2WPKH, 1000).size() == 4, QStringLiteral("and only that one joins the list"));

    // It reaches past the gap, without a server: an address given out has to be followed.
    const quint32 gap = quint32(controller->store().settings.utxoReceiveGap);
    check(waitFor([&] { return rowAt(4 + gap - 1) >= 0; }, 10000), QStringLiteral("the window reaches its gap past what was given out"));
    pause(200);
    check(rowAt(11 + gap) < 0, QStringLiteral("and no further"));
    check(waitFor([&] { return rowAt(10 + gap) >= 0; }, 10000), QStringLiteral("past the last of them"));
    answerAll(model, id);
    check(giveOut(10 + gap), QStringLiteral("given out up to where the gap ended"));
    check(waitFor([&] { return rowAt(10 + 2 * gap) >= 0; }, 10000), QStringLiteral("it keeps up"));
    answerAll(model, id);
    // Every one of those it passes over, as a run that was given out holds nothing back from another.
    for (quint32 index = 4; index < gap + 10; ++index) {
        if (index != 10)
            giveOut(index);
    }
    check(controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH).index == gap + 11, QStringLiteral("and the next one is past all of those"));

    // Ethereum has its own, and the same rules.
    check(waitFor([&] { return rowAt(3, chain::ScriptType::EthAccount) >= 0; }, 15000), QStringLiteral("an Ethereum account has its rows"));
    answerAll(model, id);
    offer = controller->nextReceive(id, ethereum, chain::ScriptType::EthAccount);
    check(offer.state == wallet::ReceiveOffer::State::Ready && offer.index == 0 && offer.address == addressAt(account, 0, ethereum), QStringLiteral("its next address comes from its key"));
    row = model->at(rowAt(0, chain::ScriptType::EthAccount));
    row.address = row.address.toLower();
    model->upsert(row);
    check(controller->nextReceive(id, ethereum, chain::ScriptType::EthAccount).state == wallet::ReceiveOffer::State::Ready, QStringLiteral("its case is only a checksum: what is kept may differ in it"));
    check(controller->markGivenOut(model->at(rowAt(0, chain::ScriptType::EthAccount)), &error) && controller->nextReceive(id, ethereum, chain::ScriptType::EthAccount).index == 1, QStringLiteral("and it moves on by itself"));

    // A wallet can only say that of its own key.
    const int further = rowAt(gap + 11);
    check(further >= 0, QStringLiteral("the row past all of those is there"));
    if (further >= 0) {
        wallet::AddressRow other = model->at(further);
        other.wallet = id + 100;
        check(controller->markGivenOut(other, &error) && controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH).index == gap + 11, QStringLiteral("a wallet that is not there moves nothing"));
    }
    check(controller->givenOutWaiting(id) == int(gap) + 11, QStringLiteral("those with nothing come in yet are counted, for a removal to say so (%1)").arg(controller->givenOutWaiting(id)));

    // What is handed out is what the key gives, whatever is kept.
    row = model->at(rowAt(2));
    check(controller->checkedAddress(row) == addressAt(segwit, 2, bitcoin), QStringLiteral("a row's address is what its key gives"));
    row.address = addressAt(segwit, 3, bitcoin);
    check(controller->checkedAddress(row).isEmpty(), QStringLiteral("and another is not"));
    row.wallet = id + 100;
    check(controller->checkedAddress(row).isEmpty(), QStringLiteral("nor one of a wallet that is not there"));

    // It outlives the window.
    controller.reset();
    {
        wallet::WalletStore store;
        check(store.load(&error) && store.wallets.size() == 1, QStringLiteral("the file reads: %1").arg(error));
        wallet::GivenOut bitcoinGiven;
        wallet::GivenOut etherGiven;
        for (const wallet::AccountKey& key : store.wallets.first().accounts)
            (key.type == chain::ScriptType::EthAccount ? etherGiven : bitcoinGiven) = key.given;
        check(bitcoinGiven.end() == gap + 11 && bitcoinGiven.contains(1) && !bitcoinGiven.contains(0) && etherGiven.end() == 1, QStringLiteral("and says what was given out (up to %1, %2)").arg(bitcoinGiven.end()).arg(etherGiven.end()));
    }
    // With the database gone, so that the window can only come from what the file says, and from a scanner.
    QFile::remove(wallet::WalletStore::databasePath());
    controller = std::make_unique<WalletController>();
    controller->prices()->useTestServer(QStringLiteral("http://127.0.0.1:1"));
    check(controller->loadExisting(&error), QStringLiteral("a new start opens it: %1").arg(error));
    model = controller->model();
    check(waitFor([&] { return !controller->loadingSaved() && model->find(id, bitcoin, chain::ScriptType::P2WPKH, 0, 0, 10 + 2 * gap) >= 0; }, 15000), QStringLiteral("with its window as far as what was given out says"));
    check(controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH).state == wallet::ReceiveOffer::State::Checking, QStringLiteral("nobody has answered this run: none is offered"));
    answerAll(model, id);
    {
        wallet::AddressRow first = model->at(model->find(id, bitcoin, chain::ScriptType::P2WPKH, 0, 0, 0));
        first.used = true;
        model->upsert(first);
    }
    offer = controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH);
    check(offer.state == wallet::ReceiveOffer::State::Ready && offer.index == gap + 11, QStringLiteral("then it goes on where it stood (%1)").arg(offer.index));
    check(controller->removeAllWallets(&error), QStringLiteral("and it all goes: %1").arg(error));
    controller.reset();
}

// ------------------------------------------------------------- the screens ----

void clickCell(QTableView* view, int row, int column = 0)
{
    const QPoint at = view->visualRect(view->model()->index(row, column)).center();
    QMouseEvent press(QEvent::MouseButtonPress, at, view->viewport()->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, view->viewport()->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &release);
}

// The row with this name, or -1.
int rowNamed(const QTableView* view, const QString& name)
{
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        if (view->model()->index(row, 0).data().toString() == name)
            return row;
    }
    return -1;
}

// The text of a column by its title.
QString cellOf(const QTableView* view, int row, const QString& title)
{
    const QAbstractItemModel* model = view->model();
    for (int column = 0; column < model->columnCount(); ++column) {
        if (model->headerData(column, Qt::Horizontal).toString() == title)
            return model->index(row, column).data().toString();
    }
    return {};
}

QPushButton* buttonNamed(QWidget* root, const QString& text)
{
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        if (button->isVisible() && button->text() == text)
            return button;
    }
    return nullptr;
}

QStringList tabTexts(const QTabBar* bar)
{
    QStringList out;
    for (int i = 0; i < bar->count(); ++i)
        out << bar->tabText(i);
    return out;
}

// The picture the page shows, against the code for what it should say.
bool pictureSays(const QLabel* qr, const QString& text)
{
    const QPixmap shown = qr->pixmap();
    if (shown.isNull())
        return false;
    const QrCode code = QrCode::encode(text.toUtf8());
    const QImage image = shown.toImage().convertToFormat(QImage::Format_RGB32);
    const int modules = code.size() + 8;
    return image.width() % modules == 0 && image == code.toImage(image.width() / modules).convertToFormat(QImage::Format_RGB32);
}

// What the figures may say when something behind them is missing.
void testFiguresHonest(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    const wallet::Network bitcoin = wallet::findNetwork(QStringLiteral("bitcoin"))->network;
    const wallet::Network ethereum = wallet::findNetwork(QStringLiteral("ethereum"))->network;
    const wallet::Asset btc = wallet::networkInfo(bitcoin).coin;
    const wallet::Asset eth = wallet::networkInfo(ethereum).coin;
    QList<wallet::AccountKey> added;
    for (const wallet::AccountKey& key : keys) {
        if (key.type == chain::ScriptType::P2WPKH || key.type == chain::ScriptType::EthAccount)
            added.append(key);
    }
    {
        ui::MainWindow window;
        window.show();
        window.resize(1280, 800);
        WalletController* controller = window.controller();
        offline(controller);
        if (!controller->addKeysWallet(added, QStringLiteral("Shop"), &error)) {
            check(false, QStringLiteral("a wallet for the test: %1").arg(error));
            return;
        }
        const quint32 shop = controller->store().wallets.first().id;
        // Rows come on the next turn: for now the key has none.
        const ui::Holding early = ui::holdingOf(controller, btc, shop);
        check(early.watched && early.missing && !early.complete, QStringLiteral("a key with no rows yet is no answer"));
        check(early.amountText() == QStringLiteral("—"), QStringLiteral("and reads as none"));
        wallet::WalletModel* model = controller->model();
        check(waitFor([&] { return !controller->loadingSaved() && model->find(shop, bitcoin, chain::ScriptType::P2WPKH, 0, 0, 19) >= 0 && model->find(shop, ethereum, chain::ScriptType::EthAccount, 0, 0, 19) >= 0; }, 20000), QStringLiteral("then it has them"));
        check(!ui::holdingOf(controller, btc, shop).missing, QStringLiteral("and that goes"));

        window.showPage(QStringLiteral("portfolio"));
        auto* portfolio = window.findChild<ui::PortfolioPage*>();
        QTableView* coins = portfolio->table()->view();
        auto* total = portfolio->findChild<QLabel*>(QStringLiteral("heroValue"));
        portfolio->findChild<QCheckBox*>(QStringLiteral("hideZero"))->setChecked(false);
        wallet::AddressRow row = model->at(model->find(shop, bitcoin, chain::ScriptType::P2WPKH, 0, 0, 0));
        row.checked = true;
        row.used = true;
        row.confirmed = 100000000;
        model->upsert(row);
        row = model->at(model->find(shop, ethereum, chain::ScriptType::EthAccount, 0, 0, 0));
        row.checked = true;
        row.used = true;
        row.setBalance(eth, chain::Amount::fromU64(1000000000000000000ULL));
        model->upsert(row);
        const auto answered = [&] {
            answerAll(model, shop);
            pause(50);
            return ui::holdingOf(controller, btc, shop).complete && ui::holdingOf(controller, eth, shop).complete;
        };
        check(waitFor(answered, 8000), QStringLiteral("every row has answered"));
        pause(450);
        // No price at all.
        const auto bitcoinRow = [&] { return rowNamed(coins, QStringLiteral("Bitcoin")); };
        const auto etherRow = [&] { return rowNamed(coins, QStringLiteral("Ethereum")); };
        check(total->text() == QStringLiteral("—"), QStringLiteral("with no price there is no total, not $0.00 (%1)").arg(total->text()));
        check(cellOf(coins, bitcoinRow(), QStringLiteral("Holdings")) == QStringLiteral("1 BTC") && cellOf(coins, bitcoinRow(), QStringLiteral("Value")) == QStringLiteral("—"), QStringLiteral("what is held still shows, with no worth"));
        check(cellOf(coins, bitcoinRow(), QStringLiteral("Share")) == QStringLiteral("—") && portfolio->table()->parts().isEmpty(), QStringLiteral("and no share"));
        // A price for one of what is held.
        {
            net::PriceQuote quote;
            quote.asset = btc;
            quote.usd = 100000.0;
            quote.at = QDateTime::currentDateTimeUtc();
            quote.source = QStringLiteral("Test");
            controller->prices()->restore({quote});
        }
        controller->refresh();
        check(waitFor([&] { return cellOf(coins, bitcoinRow(), QStringLiteral("Value")) == QStringLiteral("$100,000.00"); }, 3000), QStringLiteral("one priced shows its worth"));
        pause(300);
        check(total->text() == QStringLiteral("—"), QStringLiteral("but no sum that would leave the other out (%1)").arg(total->text()));
        check(cellOf(coins, etherRow(), QStringLiteral("Value")) == QStringLiteral("—"), QStringLiteral("and the other says nothing of its worth"));
        check(cellOf(coins, bitcoinRow(), QStringLiteral("Share")) == QStringLiteral("—") && portfolio->table()->parts().isEmpty(), QStringLiteral("and no share, nor a bar, of what is not known"));
        const auto allocation = [&]() -> ui::AllocationBar* {
            for (QWidget* each : portfolio->findChildren<QWidget*>()) {
                if (auto* bar = dynamic_cast<ui::AllocationBar*>(each))
                    return bar;
            }
            return nullptr;
        };
        check(!allocation()->isVisible(), QStringLiteral("and the bar on the page is gone"));
        {
            bool says = false;
            for (const QLabel* label : portfolio->findChildren<QLabel*>())
                says = says || (label->isVisible() && label->text().contains(QLatin1String("no price for ETH")));
            check(says, QStringLiteral("and it says which has no price"));
        }
        // Every price.
        setPrices(controller);
        controller->refresh();
        check(waitFor([&] { return total->text() == QStringLiteral("$102,000.00"); }, 3000), QStringLiteral("with all of them the sum is whole (%1)").arg(total->text()));
        pause(350);
        check(cellOf(coins, bitcoinRow(), QStringLiteral("Share")) == QStringLiteral("98.0%") && !portfolio->table()->parts().isEmpty(), QStringLiteral("with its shares and bar"));
        check(allocation()->isVisible() && allocation()->parts().size() == 2, QStringLiteral("and the bar on the page shows both"));
        // What holds something is no 0.0%, however small beside the rest.
        row = model->at(model->find(shop, ethereum, chain::ScriptType::EthAccount, 0, 0, 0));
        row.setBalance(eth, chain::Amount::fromU64(10000000000000ULL));
        model->upsert(row);
        check(waitFor([&] { return cellOf(coins, etherRow(), QStringLiteral("Share")) == QStringLiteral("<0.1%"); }, 3000), QStringLiteral("a share too small to read says so: %1").arg(cellOf(coins, etherRow(), QStringLiteral("Share"))));
        row.setBalance(eth, chain::Amount::fromU64(1000000000000000000ULL));
        model->upsert(row);

        // A wallet with only an Ethereum key: its history says it is not read.
        const QList<wallet::AccountKey> second = wallet::WalletStore::deriveAccounts(chain::bip39::split("legal winner thank year wave sausage worth useful legal winner thank yellow"), {});
        QList<wallet::AccountKey> justEther;
        for (const wallet::AccountKey& key : second) {
            if (key.type == chain::ScriptType::EthAccount)
                justEther.append(key);
        }
        check(controller->addKeysWallet(justEther, QStringLiteral("Ether only"), &error), QStringLiteral("a wallet with only an Ethereum key: %1").arg(error));
        const quint32 etherOnly = controller->store().wallets.last().id;
        auto* history = window.findChild<ui::HistoryPage*>();
        const auto saysNotRead = [&] {
            for (const QLabel* label : history->findChildren<QLabel*>()) {
                if (label->isVisible() && label->text().contains(QLatin1String("Not read yet")))
                    return true;
            }
            return false;
        };
        ui::Place place;
        place.root = ui::Place::Root::Wallets;
        place.wallet = etherOnly;
        window.goTo(place, ui::Place::Tab::History);
        pause(300);
        check(saysNotRead(), QStringLiteral("its History says it is not read yet, rather than that nothing happened"));
        place.wallet = shop;
        window.goTo(place, ui::Place::Tab::History);
        pause(300);
        check(!saysNotRead(), QStringLiteral("one that has a Bitcoin key shows its list"));
        auto* line = history->findChild<QLabel*>(QStringLiteral("historyStatus"));
        check(line && line->isVisible() && line->text().contains(QLatin1String("Ethereum")) && !line->text().startsWith(QLatin1String("Every transaction")), QStringLiteral("and says what is left out: %1").arg(line ? line->text() : QString()));
        window.showPage(QStringLiteral("history"));
        pause(300);
        check(!saysNotRead(), QStringLiteral("and all of it shows its list"));
        {
            // Chosen there, the same wallet says the same.
            auto* combo = history->findChild<QComboBox*>(QStringLiteral("historyWallet"));
            combo->setCurrentIndex(combo->findData(etherOnly));
            pause(300);
            check(saysNotRead(), QStringLiteral("and choosing that wallet there says it is not read, rather than no results"));
            check(combo->isVisible(), QStringLiteral("with the way back to the others still there"));
            combo->setCurrentIndex(0);
            pause(300);
            check(!saysNotRead(), QStringLiteral("and the others read as before"));
        }
        check(controller->removeAllWallets(&error), QStringLiteral("they go: %1").arg(error));
    }
    pause(100);

    // A key this version cannot read, in one wallet, says nothing of the other's figures.
    {
        auto controller = std::make_unique<WalletController>();
        offline(controller.get());
        check(controller->addKeysWallet(added, QStringLiteral("Shop"), &error), QStringLiteral("one wallet: %1").arg(error));
        const quint32 shop = controller->store().wallets.first().id;
        QList<wallet::AccountKey> other = wallet::WalletStore::deriveAccounts(chain::bip39::split("letter advice cage absurd amount doctor acoustic avoid letter advice cage above"), {});
        other.removeIf([](const wallet::AccountKey& key) { return key.type != chain::ScriptType::P2WPKH; });
        check(controller->addKeysWallet(other, QStringLiteral("Newer"), &error), QStringLiteral("and another: %1").arg(error));
        const quint32 newer = controller->store().wallets.last().id;
        controller.reset();
        {
            wallet::WalletStore store;
            check(store.load(&error) && store.wallets.size() == 2, QStringLiteral("the file reads: %1").arg(error));
            store.wallets[1].keptAccounts.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("p2tr")}, {QStringLiteral("account"), 0}, {QStringLiteral("xpub"), QStringLiteral("something new")}});
            check(store.save(&error), QStringLiteral("and takes a key of a kind this version does not know: %1").arg(error));
        }
        controller = std::make_unique<WalletController>();
        controller->prices()->useTestServer(QStringLiteral("http://127.0.0.1:1"));
        check(controller->loadExisting(&error), QStringLiteral("it opens: %1").arg(error));
        wallet::WalletModel* model = controller->model();
        check(waitFor([&] { return !controller->loadingSaved() && model->find(shop, bitcoin, chain::ScriptType::P2WPKH, 0, 0, 19) >= 0 && model->find(shop, ethereum, chain::ScriptType::EthAccount, 0, 0, 19) >= 0 && model->find(newer, bitcoin, chain::ScriptType::P2WPKH, 0, 0, 19) >= 0; }, 20000), QStringLiteral("with its rows"));
        const auto bothAnswered = [&] {
            answerAll(model, shop);
            answerAll(model, newer);
            pause(50);
            return ui::holdingOf(controller.get(), btc, shop).complete;
        };
        check(waitFor(bothAnswered, 8000), QStringLiteral("the other wallet's figure is whole"));
        check(controller->hasUnreadKeys() && !ui::holdingOf(controller.get(), btc).complete, QStringLiteral("the figure of them all is not"));
        const ui::Holding its = ui::holdingOf(controller.get(), btc, newer);
        check(!its.complete && its.unread, QStringLiteral("nor that of the wallet with that key"));
        check(ui::holdingNote(controller.get(), its).contains(QLatin1String("cannot read")), QStringLiteral("which says why: %1").arg(ui::holdingNote(controller.get(), its)));
        controller.reset();
        // All of that wallet's keys are of that kind.
        {
            wallet::WalletStore store;
            check(store.load(&error), QStringLiteral("the file reads again: %1").arg(error));
            store.wallets[1].accounts.clear();
            check(store.save(&error), QStringLiteral("and takes a wallet of nothing but that: %1").arg(error));
        }
        controller = std::make_unique<WalletController>();
        controller->prices()->useTestServer(QStringLiteral("http://127.0.0.1:1"));
        check(controller->loadExisting(&error), QStringLiteral("it opens: %1").arg(error));
        const ui::Holding unreadOnly = ui::holdingOf(controller.get(), btc, newer);
        check(!unreadOnly.watched && unreadOnly.unread && ui::holdingNote(controller.get(), unreadOnly).contains(QLatin1String("cannot read")), QStringLiteral("it does not say it has no key: %1").arg(ui::holdingNote(controller.get(), unreadOnly)));
        check(!ui::totalOf(controller.get(), newer).complete, QStringLiteral("and its total is not whole"));
        waitFor([&] { return !controller->loadingSaved(); }, 15000);
        check(controller->removeAllWallets(&error), QStringLiteral("they go: %1").arg(error));
    }
}

void testScreens(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    ui::MainWindow window;
    window.show();
    window.resize(1280, 800); // the least the window goes to, on any screen with room for it
    WalletController* controller = window.controller();
    offline(controller);
    // As with a server that answers: without one each line below says so, which is tried on its own.
    controller->setStatusForTest([](wallet::NetworkStatus& status) { status.connecting = false; });
    auto* stack = window.findChild<QStackedWidget*>();
    auto* portfolio = window.findChild<ui::PortfolioPage*>();
    auto* holdings = window.findChild<ui::HoldingsPage*>();
    auto* addresses = window.findChild<ui::AddressesPage*>();
    auto* history = window.findChild<ui::HistoryPage*>();
    auto* receive = window.findChild<ui::ReceivePage*>();
    auto* wallets = window.findChild<ui::WalletsPage*>();
    ui::PlaceBar* bar = window.placeBar();
    auto* tabs = bar->findChild<QTabBar*>(QStringLiteral("placeTabs"));

    window.showPage(QStringLiteral("portfolio"));
    check(stack->currentWidget() == portfolio && !bar->isVisible(), QStringLiteral("it opens on the Portfolio, with no path above it"));
    check(buttonNamed(portfolio, QStringLiteral("Add wallet")) != nullptr, QStringLiteral("which offers to add a wallet while there is none"));
    window.showPage(QStringLiteral("receive"));
    check(!receive->findChild<QWidget*>(QStringLiteral("receiveQr"))->isVisible() && receive->shownAddress().isEmpty(), QStringLiteral("Receive has nothing to offer without a wallet"));

    QList<wallet::AccountKey> first;
    for (const wallet::AccountKey& key : keys) {
        if (key.type == chain::ScriptType::P2WPKH || key.type == chain::ScriptType::EthAccount)
            first.append(key);
    }
    if (!controller->addKeysWallet(first, QStringLiteral("Shop"), &error)) {
        check(false, QStringLiteral("a wallet for the test: %1").arg(error));
        return;
    }
    // Before any answer, nothing is said of what nobody knows.
    {
        const quint32 id = controller->store().wallets.first().id;
        const wallet::Network bitcoinHere = wallet::findNetwork(QStringLiteral("bitcoin"))->network;
        check(waitFor([&] { return controller->model()->find(id, bitcoinHere, chain::ScriptType::P2WPKH, 0, 0, 0) >= 0 && !controller->loadingSaved(); }, 15000), QStringLiteral("a first row"));
        setPrices(controller);
        ui::Place place;
        place.root = ui::Place::Root::Addresses;
        place.address = controller->model()->at(controller->model()->find(id, bitcoinHere, chain::ScriptType::P2WPKH, 0, 0, 0));
        window.goTo(place, ui::Place::Tab::Holdings);
        pause(100);
        auto* said = bar->findChild<QLabel*>(QStringLiteral("placeDetail"));
        check(!said->text().contains(QLatin1String("0 BTC")) && !said->text().contains(QLatin1String("$0.00")) && said->text().contains(QLatin1String("Not checked yet")), QStringLiteral("an address nobody answered for holds no zero on the bar: %1").arg(said->text()));
        ui::Place wholeCoin;
        wholeCoin.asset = wallet::networkInfo(bitcoinHere).coin;
        window.goTo(wholeCoin, ui::Place::Tab::Addresses);
        pause(100);
        check(!said->text().contains(QLatin1String("hold it")) && !said->text().contains(QLatin1String("holds it")), QStringLiteral("and a coin no count of holders: %1").arg(said->text()));
        window.showPage(QStringLiteral("portfolio"));
    }
    QList<wallet::AccountKey> second = wallet::WalletStore::deriveAccounts(chain::bip39::split("legal winner thank year wave sausage worth useful legal winner thank yellow"), {});
    second.removeIf([](const wallet::AccountKey& key) { return key.type != chain::ScriptType::P2WPKH; }); // no Ethereum key at all
    if (!controller->addKeysWallet(second, QStringLiteral("Warehouse"), &error)) {
        check(false, QStringLiteral("a second wallet for the test: %1").arg(error));
        return;
    }
    setPrices(controller);
    wallet::WalletModel* model = controller->model();
    const quint32 shop = controller->store().wallets.at(0).id;
    const quint32 warehouse = controller->store().wallets.at(1).id;
    const wallet::Network bitcoin = wallet::findNetwork(QStringLiteral("bitcoin"))->network;
    const wallet::Network ethereum = wallet::findNetwork(QStringLiteral("ethereum"))->network;
    const wallet::Asset btc = wallet::networkInfo(bitcoin).coin;
    const wallet::Asset eth = wallet::networkInfo(ethereum).coin;
    check(waitFor([&] { return !controller->loadingSaved() && model->find(shop, bitcoin, chain::ScriptType::P2WPKH, 0, 0, 4) >= 0 && model->find(warehouse, bitcoin, chain::ScriptType::P2WPKH, 0, 0, 4) >= 0 && model->find(shop, ethereum, chain::ScriptType::EthAccount, 0, 0, 3) >= 0; }, 20000),
          QStringLiteral("the wallets have their rows"));

    window.showPage(QStringLiteral("portfolio"));
    QTableView* coins = portfolio->table()->view();
    auto* hideZero = portfolio->findChild<QCheckBox*>(QStringLiteral("hideZero"));
    auto* total = portfolio->findChild<QLabel*>(QStringLiteral("heroValue"));
    hideZero->setChecked(true);
    const auto settle = [&] { pause(450); };

    // Nothing has answered.
    settle();
    check(total->text() == QStringLiteral("—"), QStringLiteral("with no answer yet the total is no zero (%1)").arg(total->text()));
    check(coins->model()->rowCount() == wallet::assets().size(), QStringLiteral("nor are the coins: not one is hidden as if it held nothing"));
    const int bitcoinRow = rowNamed(coins, QStringLiteral("Bitcoin"));
    check(bitcoinRow >= 0 && cellOf(coins, bitcoinRow, QStringLiteral("Holdings")) == QStringLiteral("—"), QStringLiteral("a coin nobody has answered for reads a dash"));
    check(cellOf(coins, bitcoinRow, QStringLiteral("Addresses")) == QStringLiteral("—") && cellOf(coins, bitcoinRow, QStringLiteral("Share")) == QStringLiteral("—"), QStringLiteral("with no count or share of nothing"));
    check(coins->model()->index(bitcoinRow, 0).data(Qt::UserRole + 2).toString().contains(QLatin1String("not checked yet")), QStringLiteral("and says why under its name: %1").arg(coins->model()->index(bitcoinRow, 0).data(Qt::UserRole + 2).toString()));

    // Part of it has.
    wallet::AddressRow row = model->at(model->find(shop, bitcoin, chain::ScriptType::P2WPKH, 0, 0, 0));
    row.checked = true;
    row.used = true;
    row.confirmed = 150000000;
    model->upsert(row);
    settle();
    check(total->text() == QStringLiteral("≥ $150,000.00"), QStringLiteral("what is known so far is a lower bound (%1)").arg(total->text()));
    check(cellOf(coins, rowNamed(coins, QStringLiteral("Bitcoin")), QStringLiteral("Holdings")) == QStringLiteral("≥ 1.5 BTC"), QStringLiteral("and so is the coin's (%1)").arg(cellOf(coins, rowNamed(coins, QStringLiteral("Bitcoin")), QStringLiteral("Holdings"))));
    check(cellOf(coins, rowNamed(coins, QStringLiteral("Bitcoin")), QStringLiteral("Share")) == QStringLiteral("—") && cellOf(coins, rowNamed(coins, QStringLiteral("Ethereum")), QStringLiteral("Share")) == QStringLiteral("—"), QStringLiteral("no share of a whole nobody knows yet"));
    check(portfolio->table()->parts().isEmpty(), QStringLiteral("nor a bar of it"));
    check(cellOf(coins, rowNamed(coins, QStringLiteral("Bitcoin")), QStringLiteral("Addresses")) == QStringLiteral("≥ 1"), QStringLiteral("and the count of holders is a lower bound too"));
    {
        // One wallet has answered, the other has not: each says its own.
        answerAll(model, shop);
        ui::Place place;
        place.asset = btc;
        window.goTo(place, ui::Place::Tab::Holdings);
        settle();
        QTableView* wallets2 = holdings->table()->view();
        check(cellOf(wallets2, rowNamed(wallets2, QStringLiteral("Shop")), QStringLiteral("Holdings")) == QStringLiteral("1.5 BTC"), QStringLiteral("a wallet that has answered reads plain"));
        check(cellOf(wallets2, rowNamed(wallets2, QStringLiteral("Warehouse")), QStringLiteral("Holdings")) == QStringLiteral("—"), QStringLiteral("and one that has not reads a dash, and is not hidden"));
        check(model->uncheckedOn(bitcoin, shop) == 0 && model->uncheckedOn(bitcoin, warehouse) > 0, QStringLiteral("as the counts per wallet say"));
        check(model->holders(btc, shop) == 1 && model->holders(btc, warehouse) == 0 && model->holders(btc) == 1, QStringLiteral("and those of holders"));
        window.showPage(QStringLiteral("portfolio"));
    }

    // All of it has. Rows of the same wallets may still be coming.
    const auto answerEverything = [&] {
        answerAll(model, shop);
        answerAll(model, warehouse);
    };
    answerEverything();
    row = model->at(model->find(shop, ethereum, chain::ScriptType::EthAccount, 0, 0, 1));
    row.checked = true;
    row.used = true;
    row.setBalance(eth, chain::Amount::fromU64(1000000000000000000ULL));
    model->upsert(row);
    answerEverything();
    check(waitFor([&] { answerEverything(); pause(50); return total->text() == QStringLiteral("$152,000.00"); }, 8000), QStringLiteral("every balance answered, it reads plain and whole (%1)").arg(total->text()));
    settle();
    const int btcRow = rowNamed(coins, QStringLiteral("Bitcoin"));
    check(coins->model()->rowCount() == 2 && btcRow == 0, QStringLiteral("what holds nothing is hidden, the largest first (%1 rows)").arg(coins->model()->rowCount()));
    check(cellOf(coins, btcRow, QStringLiteral("Holdings")) == QStringLiteral("1.5 BTC") && cellOf(coins, btcRow, QStringLiteral("Value")) == QStringLiteral("$150,000.00"), QStringLiteral("with what it holds and what that is worth"));
    check(cellOf(coins, btcRow, QStringLiteral("Price")) == QStringLiteral("$100,000.00") && cellOf(coins, btcRow, QStringLiteral("Share")) == QStringLiteral("98.7%"), QStringLiteral("its price and share (%1)").arg(cellOf(coins, btcRow, QStringLiteral("Share"))));
    check(cellOf(coins, btcRow, QStringLiteral("Addresses")) == QStringLiteral("1"), QStringLiteral("and how many addresses hold it"));
    const QList<ui::AllocationBar::Part> parts = portfolio->table()->parts();
    double partsSum = 0.0;
    for (const ui::AllocationBar::Part& part : parts)
        partsSum += part.usd;
    check(qAbs(partsSum - 152000.0) < 0.01, QStringLiteral("what the total is made of adds up to it"));
    hideZero->setChecked(false);
    check(coins->model()->rowCount() == wallet::assets().size(), QStringLiteral("and they come back when asked"));
    hideZero->setChecked(true);

    // A coin: wallets. A wallet with no key on its network is no zero.
    clickCell(coins, rowNamed(coins, QStringLiteral("Ethereum")));
    QTableView* inside = holdings->table()->view();
    check(stack->currentWidget() == holdings && bar->isVisible(), QStringLiteral("a click on a coin opens it"));
    check(bar->pathTexts() == QStringList({QStringLiteral("Portfolio"), QStringLiteral("Ethereum")}), QStringLiteral("and the path says where: %1").arg(bar->pathTexts().join(QLatin1Char('>'))));
    check(tabTexts(tabs) == QStringList({QStringLiteral("Wallets"), QStringLiteral("Addresses"), QStringLiteral("History")}), QStringLiteral("with a tab for each way to look at it"));
    settle();
    check(rowNamed(inside, QStringLiteral("Warehouse")) < 0, QStringLiteral("a wallet with no key on its network is no holder, and goes with Hide zero"));
    holdings->findChild<QCheckBox*>()->setChecked(false);
    pause(50);
    check(cellOf(inside, rowNamed(inside, QStringLiteral("Warehouse")), QStringLiteral("Holdings")) == QStringLiteral("—"), QStringLiteral("without it, it shows no zero"));
    check(rowNamed(inside, QStringLiteral("Shop")) == 0 && cellOf(inside, 0, QStringLiteral("Holdings")) == QStringLiteral("1 ETH"), QStringLiteral("the one that holds it comes first, with what it holds"));
    {
        const int holdingsColumn = [&] {
            for (int column = 0; column < inside->model()->columnCount(); ++column) {
                if (inside->model()->headerData(column, Qt::Horizontal).toString() == QLatin1String("Holdings"))
                    return column;
            }
            return 0;
        }();
        const QString tip = inside->model()->index(rowNamed(inside, QStringLiteral("Warehouse")), holdingsColumn).data(Qt::ToolTipRole).toString();
        check(tip.contains(QLatin1String("This wallet has no key on")), QStringLiteral("and says it is this wallet that has no key, not every wallet: %1").arg(tip));
    }
    holdings->findChild<QCheckBox*>()->setChecked(true);

    window.goTo([&] { ui::Place place; place.asset = btc; return place; }(), ui::Place::Tab::Holdings);
    settle();
    check(bar->pathTexts() == QStringList({QStringLiteral("Portfolio"), QStringLiteral("Bitcoin")}), QStringLiteral("another coin says so"));
    clickCell(inside, rowNamed(inside, QStringLiteral("Shop")));
    check(bar->pathTexts() == QStringList({QStringLiteral("Portfolio"), QStringLiteral("Bitcoin"), QStringLiteral("Shop")}), QStringLiteral("a wallet goes one step further: %1").arg(bar->pathTexts().join(QLatin1Char('>'))));
    check(stack->currentWidget() == addresses && tabTexts(tabs) == QStringList({QStringLiteral("Addresses"), QStringLiteral("History")}), QStringLiteral("to the addresses of that coin in it"));
    auto* addressTable = addresses->findChild<QTableView*>();
    check(addressTable->model()->rowCount() == 1, QStringLiteral("those that hold it (%1)").arg(addressTable->model()->rowCount()));
    auto* state = addresses->findChild<QComboBox*>(QStringLiteral("stateFilter"));
    state->setCurrentIndex(0);
    pause(150);
    check(addressTable->model()->rowCount() == model->counts(shop).rows - model->rowsOn(ethereum), QStringLiteral("and every address of it when asked (%1)").arg(addressTable->model()->rowCount()));
    state->setCurrentIndex(1);
    pause(150);
    check(!addresses->findChild<QComboBox*>(QStringLiteral("walletFilter"))->isVisible(), QStringLiteral("no combo says other than the path"));

    tabs->setCurrentIndex(1);
    emit tabs->tabBarClicked(1);
    check(stack->currentWidget() == history && bar->tab() == ui::Place::Tab::History, QStringLiteral("its History tab is the History page"));
    check(history->tableModel()->filter().wallet == shop && history->tableModel()->filter().network == int(bitcoin), QStringLiteral("narrowed to this wallet and this network"));
    check(!history->findChild<QWidget*>(QStringLiteral("historyWallet"))->isVisible(), QStringLiteral("with no combo that says other"));

    // Up the path.
    // A new child shows on the next event-loop turn.
    auto pathLink = [&](const QString& text) {
        pause(50);
        return buttonNamed(bar, text);
    };
    if (QPushButton* link = pathLink(QStringLiteral("Bitcoin")))
        link->click();
    check(bar->pathTexts() == QStringList({QStringLiteral("Portfolio"), QStringLiteral("Bitcoin")}) && stack->currentWidget() == holdings, QStringLiteral("a step of the path goes back to it"));
    if (QPushButton* link = pathLink(QStringLiteral("Portfolio")))
        link->click();
    check(!bar->isVisible() && stack->currentWidget() == portfolio, QStringLiteral("the first to where it all began"));
    window.showPage(QStringLiteral("addresses"));
    pause(150);
    check(addressTable->model()->rowCount() == model->size() && !bar->isVisible(), QStringLiteral("the Addresses page is whole again (%1 of %2)").arg(addressTable->model()->rowCount()).arg(model->size()));
    check(addresses->findChild<QComboBox*>(QStringLiteral("stateFilter"))->currentIndex() == 0, QStringLiteral("with nothing of the place left in its filters"));
    window.showPage(QStringLiteral("history"));
    check(history->tableModel()->filter().wallet == 0 && history->tableModel()->filter().network < 0, QStringLiteral("and so is the History page"));

    // The other way down: a wallet, its coins, an address.
    window.showPage(QStringLiteral("wallets"));
    {
        ui::Place place;
        place.root = ui::Place::Root::Wallets;
        place.wallet = shop;
        window.goTo(place, ui::Place::Tab::Holdings);
    }
    settle();
    check(bar->pathTexts() == QStringList({QStringLiteral("Wallets"), QStringLiteral("Shop")}), QStringLiteral("a wallet opens the same way: %1").arg(bar->pathTexts().join(QLatin1Char('>'))));
    check(tabTexts(tabs) == QStringList({QStringLiteral("Coins"), QStringLiteral("Addresses"), QStringLiteral("History")}), QStringLiteral("to its coins"));
    check(inside->model()->rowCount() == 2 && rowNamed(inside, QStringLiteral("Bitcoin")) == 0, QStringLiteral("with what it holds (%1)").arg(inside->model()->rowCount()));
    clickCell(inside, rowNamed(inside, QStringLiteral("Bitcoin")));
    check(bar->pathTexts() == QStringList({QStringLiteral("Wallets"), QStringLiteral("Shop"), QStringLiteral("Bitcoin")}), QStringLiteral("and a coin of it after that: %1").arg(bar->pathTexts().join(QLatin1Char('>'))));

    // A double click is not two: the second lands on what the first opened, and is not for it.
    window.showPage(QStringLiteral("portfolio"));
    settle();
    pause(QApplication::doubleClickInterval() + 50);
    clickCell(coins, rowNamed(coins, QStringLiteral("Bitcoin")));
    clickCell(inside, 0);
    const QStringList afterTwo = bar->pathTexts();
    check(afterTwo == QStringList({QStringLiteral("Portfolio"), QStringLiteral("Bitcoin")}), QStringLiteral("two quick clicks open one thing: %1").arg(afterTwo.join(QLatin1Char('>'))));
    pause(QApplication::doubleClickInterval() + 50);
    clickCell(inside, rowNamed(inside, QStringLiteral("Shop")));
    check(bar->pathTexts().size() == 3, QStringLiteral("and a click after that opens what it is on"));

    // An address: all it holds, in its own coin.
    row = model->at(model->find(shop, ethereum, chain::ScriptType::EthAccount, 0, 0, 1));
    {
        ui::Place place;
        place.root = ui::Place::Root::Addresses;
        place.address = row;
        window.goTo(place, ui::Place::Tab::Holdings);
    }
    settle();
    check(tabTexts(tabs) == QStringList({QStringLiteral("Assets"), QStringLiteral("History")}), QStringLiteral("an address has its assets and its history"));
    check(bar->pathTexts().size() == 2 && bar->pathTexts().at(1).contains(row.address.left(8)), QStringLiteral("and is named by both ends of it: %1").arg(bar->pathTexts().join(QLatin1Char('>'))));
    check(cellOf(inside, rowNamed(inside, QStringLiteral("Ethereum")), QStringLiteral("Holdings")) == QStringLiteral("1 ETH") && cellOf(inside, rowNamed(inside, QStringLiteral("Ethereum")), QStringLiteral("Value")) == QStringLiteral("$2,000.00"), QStringLiteral("each asset in its own coin, with USD"));
    auto* title = bar->findChild<QLabel*>(QStringLiteral("placeTitle"));
    auto* detail = bar->findChild<QLabel*>(QStringLiteral("placeDetail"));
    check(title->toolTip() == row.address && detail->text().contains(QLatin1String("m/44'/60'/0'/0/1")) && detail->text().contains(QLatin1String("Shop")), QStringLiteral("and the bar says which and where: %1").arg(detail->text()));
    check(title->text().size() >= 36 && title->text().startsWith(row.address.left(4)) && title->text().endsWith(row.address.right(4)), QStringLiteral("nearly whole at the least the window goes to, or whole: %1").arg(title->text()));
    window.resize(1024, 800);
    pause(100);
    check(title->text().startsWith(row.address.left(4)) && title->text().endsWith(row.address.right(4)), QStringLiteral("and on a smaller screen with both its ends: %1").arg(title->text()));
    check(bar->width() <= 1024 - 188 && buttonNamed(bar, QStringLiteral("Show QR")) && buttonNamed(bar, QStringLiteral("Show QR"))->width() >= buttonNamed(bar, QStringLiteral("Show QR"))->sizeHint().width(), QStringLiteral("its buttons whole"));
    window.resize(1280, 800);
    check(controller->setAddressLabel(row.address, QStringLiteral("Till 4"), &error), QStringLiteral("a label for it"));
    check(waitFor([&] { return title->toolTip().startsWith(QLatin1String("Till 4")); }, 2000), QStringLiteral("shows"));
    check(bar->pathTexts().value(1) == QLatin1String("Till 4"), QStringLiteral("and the path goes by it: %1").arg(bar->pathTexts().join(QLatin1Char('>'))));

    // Its history tab, on a network with none yet: says so, rather than reading as empty.
    tabs->setCurrentIndex(1);
    emit tabs->tabBarClicked(1);
    pause(300);
    check(stack->currentWidget() == history && bar->tab() == ui::Place::Tab::History, QStringLiteral("its History tab opens"));
    bool saysSo = false;
    for (const QLabel* label : history->findChildren<QLabel*>())
        saysSo = saysSo || (label->isVisible() && label->text().contains(QLatin1String("Not read yet")));
    check(saysSo, QStringLiteral("and on a network with no history yet it says so"));
    check(!history->findChild<QTableView*>()->isVisible(), QStringLiteral("instead of an empty list"));
    window.showPage(QStringLiteral("history"));
    pause(200);
    check(history->findChild<QTableView*>() != nullptr, QStringLiteral("back on all of it"));
    check(history->tableModel()->filter().scripthash.isEmpty() && history->tableModel()->filter().wallet == 0 && history->tableModel()->filter().network < 0, QStringLiteral("with nothing of the place left in its filter"));
    saysSo = false;
    for (const QLabel* label : history->findChildren<QLabel*>())
        saysSo = saysSo || (label->isVisible() && label->text().contains(QLatin1String("Not read yet")));
    check(!saysSo, QStringLiteral("and leaving it, that goes"));

    // Receive: what is on the page is what the key gives, as text and as a code.
    window.showPage(QStringLiteral("receive"));
    receive->showFor(shop, btc);
    pause(100);
    auto* qr = receive->findChild<QLabel*>(QStringLiteral("receiveQr"));
    const wallet::ReceiveOffer next = controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH);
    check(next.state == wallet::ReceiveOffer::State::Ready && receive->shownAddress() == next.address, QStringLiteral("Receive shows the next unused address (%1)").arg(next.index));
    check(receive->shownPayload() == next.address && pictureSays(qr, next.address), QStringLiteral("and the picture of its QR code says just that"));
    const ReadBack plain = readSymbol(QrCode::encode(receive->shownPayload().toUtf8()), QrCode::Correction::Medium);
    check(plain.wrong.isEmpty() && QString::fromUtf8(plain.data) == next.address, QStringLiteral("which reads back: %1").arg(plain.wrong));

    auto* amount = receive->findChild<QLineEdit*>(QStringLiteral("receiveAmount"));
    auto* status = receive->findChild<QLabel*>(QStringLiteral("receiveStatus"));
    amount->setText(QStringLiteral("0.015"));
    check(receive->shownPayload() == QStringLiteral("bitcoin:%1?amount=0.015").arg(next.address), QStringLiteral("an amount goes into it as BIP 21 asks: %1").arg(receive->shownPayload()));
    const ReadBack uri = readSymbol(QrCode::encode(receive->shownPayload().toUtf8()), QrCode::Correction::Medium);
    check(uri.wrong.isEmpty() && QString::fromUtf8(uri.data) == receive->shownPayload(), QStringLiteral("and reads back: %1").arg(uri.wrong));
    check(pictureSays(qr, receive->shownPayload()), QStringLiteral("and the picture changes with it"));
    for (const QString& bad : {QStringLiteral("1e5"), QStringLiteral("-1"), QStringLiteral("0,5"), QStringLiteral("0.123456789"), QStringLiteral("0"), QStringLiteral("abc")}) {
        amount->setText(bad);
        check(receive->shownPayload() == next.address && status->isVisible(), QStringLiteral("%1 is no amount: the code stays the plain address, and it says so").arg(bad));
    }
    amount->clear();
    check(pictureSays(qr, next.address), QStringLiteral("cleared, the picture is the plain address again"));

    // A label stays on this PC.
    auto* label = receive->findChild<QLineEdit*>(QStringLiteral("receiveLabel"));
    label->insert(QStringLiteral("Order 1001")); // as typing does: setText is not someone typing
    check(!receive->shownPayload().contains(QLatin1String("Order")), QStringLiteral("a label does not go into the code"));

    auto* list = receive->findChild<QTableWidget*>(QStringLiteral("receiveList"));
    const QString before = receive->shownAddress();
    const int listed = list->rowCount();
    buttonNamed(receive, QStringLiteral("New address"))->click();
    pause(100);
    check(receive->shownAddress() != before && !receive->shownAddress().isEmpty(), QStringLiteral("New address gives another"));
    check(label->text().isEmpty(), QStringLiteral("with its own label to come"));
    check(controller->addressLabel(before) == QLatin1String("Order 1001"), QStringLiteral("the label went with the one before it"));
    check(pictureSays(qr, receive->shownAddress()), QStringLiteral("and the picture went on with the address"));
    check(list->rowCount() == listed + 1 && list->item(0, 0)->text() == before && list->item(0, 1)->text() == QLatin1String("Order 1001"), QStringLiteral("which is listed as given out, the latest first"));
    check(list->item(0, 5)->text() == QLatin1String("Waiting"), QStringLiteral("waiting for its payment (%1)").arg(list->item(0, 5)->text()));

    // A click on what was given out shows it again, and moves nothing on.
    const quint32 mark = controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH).index;
    emit list->cellClicked(0, 0);
    check(receive->shownAddress() == before && receive->shownPayload() == before, QStringLiteral("what was given out can be shown again"));
    check(controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH).index == mark, QStringLiteral("which marks nothing"));
    check(label->text() == QLatin1String("Order 1001"), QStringLiteral("with its label"));
    check(pictureSays(qr, before), QStringLiteral("and its picture"));

    // A label typed for one stays with what it was typed for, and an amount is for one payment.
    {
        const wallet::ReceiveOffer now = controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH);
        check(controller->setAddressLabel(now.address, QStringLiteral("Till 3"), &error), QStringLiteral("the next address has a label of its own"));
        window.showPage(QStringLiteral("receive"));
        pause(50);
        check(receive->shownAddress() == now.address && label->text() == QLatin1String("Till 3"), QStringLiteral("it comes up with it"));
        buttonNamed(receive, QStringLiteral("Copy address"))->click();
        check(controller->addressLabel(now.address) == QLatin1String("Till 3"), QStringLiteral("and Copy leaves it as it is"));
        check(receive->lastCopied() == now.address, QStringLiteral("and hands over what is on screen"));
        check(controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH).index > now.index, QStringLiteral("which is not offered again"));
        check(receive->shownAddress() == now.address, QStringLiteral("though it stays on screen"));
        // Coming back, it is the next one, with nothing of this payment on it.
        amount->setText(QStringLiteral("0.5"));
        window.showPage(QStringLiteral("portfolio"));
        window.showPage(QStringLiteral("receive"));
        pause(50);
        check(receive->shownAddress() == controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH).address && receive->shownAddress() != now.address, QStringLiteral("coming back brings the next"));
        check(amount->text().isEmpty() && label->text().isEmpty() && receive->shownPayload() == receive->shownAddress(), QStringLiteral("with no amount or label of the last payment"));
    }

    // Saved as a file: what is on screen, and marked.
    {
        const wallet::ReceiveOffer now = controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH);
        const QString file = QDir(wallet::WalletStore::dataDir()).filePath(QStringLiteral("receive-test.png"));
        amount->setText(QStringLiteral("0.25"));
        const QString said = receive->shownPayload();
        check(receive->saveQrAs(file), QStringLiteral("the code can be saved"));
        const QImage saved(file);
        const QrCode code = QrCode::encode(said.toUtf8());
        check(!saved.isNull() && saved.convertToFormat(QImage::Format_RGB32) == code.toImage(saved.width() / (code.size() + 8)).convertToFormat(QImage::Format_RGB32), QStringLiteral("the file says what the screen says: %1").arg(said));
        check(controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH).index > now.index, QStringLiteral("and it is marked"));
        QFile::remove(file);
        buttonNamed(receive, QStringLiteral("New address"))->click();
        check(amount->text().isEmpty() && receive->shownAddress() != now.address, QStringLiteral("New address leaves that behind"));
    }

    // Show QR on an address nobody was given: it says so, and letting it go marks it.
    {
        const wallet::ReceiveOffer now = controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH);
        const wallet::AddressRow unused = model->at(model->find(shop, bitcoin, chain::ScriptType::P2WPKH, 0, 0, now.index));
        receive->showAddress(unused);
        auto* line = receive->findChild<QLabel*>(QStringLiteral("receiveState"));
        check(line->text().contains(QLatin1String("Unused")) && !line->text().contains(QLatin1String("Given out")), QStringLiteral("shown, an address nobody was given does not say it was: %1").arg(line->text()));
        check(controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH).index == now.index, QStringLiteral("and showing it marks nothing"));
        buttonNamed(receive, QStringLiteral("Copy address"))->click();
        check(controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH).index > now.index, QStringLiteral("copying it does"));
        check(line->text().contains(QLatin1String("Given out")), QStringLiteral("and it says so: %1").arg(line->text()));
        // Further on, it marks that one and no other.
        const wallet::ReceiveOffer then = controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH);
        const wallet::AddressRow further = model->at(model->find(shop, bitcoin, chain::ScriptType::P2WPKH, 0, 0, then.index + 2));
        receive->showAddress(further);
        buttonNamed(receive, QStringLiteral("New address"))->click();
        check(controller->isGivenOut(further) && controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH).index == then.index, QStringLiteral("New address on one further on marks that one, and the first free comes up"));
        check(receive->shownAddress() == then.address, QStringLiteral("on screen too"));
        // A change address is no one's to be given, and says nothing of being given out.
        const wallet::AddressRow change = model->at(model->find(shop, bitcoin, chain::ScriptType::P2WPKH, 0, 1, 0));
        receive->showAddress(change);
        buttonNamed(receive, QStringLiteral("Copy address"))->click();
        check(receive->lastCopied() == change.address && !controller->isGivenOut(change), QStringLiteral("a change address can be copied, and is not marked"));
    }

    // While shown, a wallet changing elsewhere leaves it, and Copy still marks what is on screen.
    {
        const wallet::ReceiveOffer now = controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH);
        receive->showFor(shop, btc);
        emit list->cellClicked(0, 0);
        check(controller->renameWallet(warehouse, QStringLiteral("Warehouse 2"), &error), QStringLiteral("a wallet is renamed elsewhere"));
        check(controller->renameWallet(warehouse, QStringLiteral("Warehouse"), &error), QStringLiteral("and back"));
        check(receive->shownAddress() == list->item(0, 0)->text(), QStringLiteral("what was asked for stays through that"));
        receive->showFor(shop, btc);
        check(receive->shownAddress() == now.address, QStringLiteral("and the next one is still next"));
        // A line that no longer says what its row says shows nothing else: rows move.
        const QString said = list->item(0, 0)->text();
        list->item(0, 0)->setText(QStringLiteral("something else"));
        emit list->cellClicked(0, 0);
        check(receive->shownAddress() == now.address && list->item(0, 0)->text() == said, QStringLiteral("a line that says something else than its row shows nothing, and is read again"));
    }

    // A label longer than the field holds, as one read from a file can be: Copy does not cut it.
    {
        const wallet::ReceiveOffer now = controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH);
        const QString longLabel(1500, QLatin1Char('x'));
        check(controller->setAddressLabel(now.address, longLabel, &error), QStringLiteral("a label longer than the field"));
        receive->showFor(shop, btc);
        buttonNamed(receive, QStringLiteral("Copy address"))->click();
        check(controller->addressLabel(now.address) == longLabel, QStringLiteral("is left whole by what was not typed (%1)").arg(controller->addressLabel(now.address).size()));
        receive->showFor(shop, btc);
    }

    // A label refused stays, with why, and New address does not move on from it.
    {
        const QString shown = receive->shownAddress();
        label->selectAll();
        label->insert(QStringLiteral("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"));
        buttonNamed(receive, QStringLiteral("New address"))->click();
        check(receive->shownAddress() == shown, QStringLiteral("what looks like seed words as a label stops New address"));
        check(status->isVisible() && !status->text().isEmpty(), QStringLiteral("with why: %1").arg(status->text()));
        check(label->text().isEmpty(), QStringLiteral("and is not kept on screen"));
        check(controller->addressLabel(shown).isEmpty(), QStringLiteral("nor anywhere"));
    }

    // What cannot be written is said, stays said, and is tried again. A program changing the file does that.
    {
        const QString path = wallet::WalletStore::filePath();
        const QByteArray whole = [&path] {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        }();
        {
            QFile file(path);
            file.open(QIODevice::Append);
            file.write("x");
        }
        const wallet::ReceiveOffer now = controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH);
        receive->showFor(shop, btc);
        buttonNamed(receive, QStringLiteral("New address"))->click();
        auto* owed = receive->findChild<QLabel*>(QStringLiteral("receiveOwed"));
        check(controller->givenOutOwed(), QStringLiteral("what was given out cannot be written: it is owed"));
        check(owed->isVisible() && !owed->text().isEmpty(), QStringLiteral("the page says so, after moving on"));
        check(controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH).index != now.index, QStringLiteral("this session goes on past it all the same"));
        pause(50);
        bool noticed = false;
        for (const QLabel* each : window.findChildren<QLabel*>())
            noticed = noticed || (each->isVisible() && each->text().contains(QLatin1String("could not be written down")) && each != owed);
        check(noticed, QStringLiteral("and the notice above every page says so too"));
        {
            QFile file(path);
            file.open(QIODevice::WriteOnly | QIODevice::Truncate);
            file.write(whole);
        }
        const QString after = receive->shownAddress();
        buttonNamed(receive, QStringLiteral("New address"))->click();
        check(!controller->givenOutOwed(), QStringLiteral("once it can be written, it is"));
        check(receive->shownAddress() != after, QStringLiteral("and on it goes"));
        check(waitFor([&] { return !owed->isVisible(); }, 2000), QStringLiteral("and the page stops saying so"));
    }
    receive->showFor(shop, btc);

    // A payment to it says so: on the page, and the next address stays what it was.
    emit list->cellClicked(int(list->rowCount()) - 1, 0);
    {
        const int position = model->find(shop, bitcoin, chain::ScriptType::P2WPKH, 0, 0, next.index);
        wallet::AddressRow paid = model->at(position);
        auto* line = receive->findChild<QLabel*>(QStringLiteral("receiveState"));
        // As a server's notice comes: used, with what it holds not read yet.
        paid.used = true;
        paid.checked = false;
        model->upsert(paid);
        check(waitFor([&] { return line->text().contains(QLatin1String("being read")); }, 3000), QStringLiteral("paid, it does not say it holds nothing while that is being read: %1").arg(line->text()));
        paid.checked = true;
        paid.confirmed = 2000;
        model->upsert(paid);
        check(waitFor([&] { return line->text().contains(QLatin1String("Holds")); }, 3000), QStringLiteral("then a payment to it shows at once"));
        check(waitFor([&] { return list->item(int(list->rowCount()) - 1, 5)->text() == QLatin1String("Received"); }, 3000), QStringLiteral("and in the list"));
    }

    // Ethereum and its tokens: the same address, and what may be sent to it.
    buttonNamed(receive, QStringLiteral("New address"))->click();
    auto* coin = receive->findChild<QComboBox*>(QStringLiteral("receiveCoin"));
    const wallet::AssetInfo* usdt = wallet::findAsset(QStringLiteral("ethereum/usdt"));
    coin->setCurrentIndex(coin->findData(int(usdt->asset)));
    pause(100);
    check(receive->shownAddress() == controller->nextReceive(shop, ethereum, chain::ScriptType::EthAccount).address, QStringLiteral("a token is received on its network's next address: %1").arg(receive->shownAddress()));
    check(!receive->findChild<QWidget*>(QStringLiteral("receiveType"))->isVisible() && !amount->isVisible(), QStringLiteral("with no kind of address to choose, and no amount wallets would agree on"));
    bool warns = false;
    for (const QLabel* each : receive->findChildren<QLabel*>())
        warns = warns || (each->isVisible() && each->text().contains(QLatin1String("USDT")) && each->text().contains(QLatin1String("Ethereum network")));
    check(warns, QStringLiteral("and it says what may be sent, on which network"));

    // A wallet without that key has no such coin to offer.
    auto* walletCombo = receive->findChild<QComboBox*>(QStringLiteral("receiveWallet"));
    walletCombo->setCurrentIndex(walletCombo->findData(warehouse));
    pause(100);
    check(coin->findData(int(eth)) < 0 && coin->findData(int(btc)) >= 0, QStringLiteral("a wallet offers only what it has a key for"));
    check(receive->shownAddress() == controller->nextReceive(warehouse, bitcoin, chain::ScriptType::P2WPKH).address, QStringLiteral("so it goes back to what it has: %1").arg(receive->shownAddress()));

    // What was given out in one wallet is nothing to another.
    check(controller->nextReceive(warehouse, bitcoin, chain::ScriptType::P2WPKH).index == 0, QStringLiteral("each wallet has its own next address"));

    // A wallet goes: nothing shows what it was, and the page stands on the other.
    {
        ui::Place place;
        place.root = ui::Place::Root::Wallets;
        place.wallet = warehouse;
        window.goTo(place, ui::Place::Tab::Holdings);
    }
    check(controller->removeWallet(warehouse, &error), QStringLiteral("a wallet can go while it is open: %1").arg(error));
    check(!bar->isVisible() && stack->currentWidget() == wallets, QStringLiteral("and the place goes with it"));
    pause(100);
    window.showPage(QStringLiteral("receive"));
    check(receive->shownAddress() == controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH).address || receive->shownAddress() == controller->nextReceive(shop, ethereum, chain::ScriptType::EthAccount).address,
          QStringLiteral("and Receive stands on the wallet that is left"));
    check(walletCombo->currentData().toUInt() == shop, QStringLiteral("whose name it shows"));

    // One wallet: a step through it says nothing.
    {
        ui::Place place;
        place.asset = btc;
        window.goTo(place, ui::Place::Tab::Holdings);
    }
    check(bar->pathTexts() == QStringList({QStringLiteral("Portfolio"), QStringLiteral("Bitcoin")}) && stack->currentWidget() == addresses, QStringLiteral("with one wallet a coin opens straight on its addresses"));
    check(tabTexts(tabs) == QStringList({QStringLiteral("Addresses"), QStringLiteral("History")}), QStringLiteral("without a tab of one wallet"));
    window.showPage(QStringLiteral("portfolio"));

    // A name with an ampersand reads as it is, link or not.
    check(controller->renameWallet(shop, QStringLiteral("R&D"), &error), QStringLiteral("a wallet named with an ampersand"));
    {
        ui::Place place;
        place.root = ui::Place::Root::Wallets;
        place.wallet = shop;
        place.asset = btc;
        window.goTo(place, ui::Place::Tab::Addresses);
        check(bar->pathTexts().value(1) == QLatin1String("R&D"), QStringLiteral("as a step of the path: %1").arg(bar->pathTexts().join(QLatin1Char('>'))));
        pause(50);
        QPushButton* link = nullptr;
        for (QPushButton* each : bar->findChildren<QPushButton*>(QStringLiteral("pathLink"))) {
            if (each->isVisible() && each->text().contains(QLatin1String("R&&D")))
                link = each;
        }
        check(link != nullptr, QStringLiteral("drawn with its ampersand, not a shortcut"));
    }
    window.showPage(QStringLiteral("portfolio"));

    check(controller->removeAllWallets(&error), QStringLiteral("and they can all go: %1").arg(error));
    pause(100);
    check(buttonNamed(portfolio, QStringLiteral("Add wallet")) != nullptr, QStringLiteral("the Portfolio is back to offering to add one"));
    window.showPage(QStringLiteral("receive"));
    check(receive->shownAddress().isEmpty(), QStringLiteral("and Receive has nothing to offer"));
}

// An address goes out when someone asks for that and in no other way, never twice without a word, and
// what the screens say of it is no more than is known.
void testHandingOut(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    const wallet::Network bitcoin = wallet::findNetwork(QStringLiteral("bitcoin"))->network;
    const wallet::Network ethereum = wallet::findNetwork(QStringLiteral("ethereum"))->network;
    const wallet::Asset btc = wallet::networkInfo(bitcoin).coin;
    const wallet::Asset eth = wallet::networkInfo(ethereum).coin;
    const chain::ScriptType segwit = chain::ScriptType::P2WPKH;
    const chain::ScriptType account = chain::ScriptType::EthAccount;
    ui::MainWindow window;
    window.show();
    window.resize(1280, 800);
    WalletController* controller = window.controller();
    offline(controller);
    const auto answering = [](wallet::NetworkStatus& status) { status.connecting = false; };
    controller->setStatusForTest(answering);

    QList<wallet::AccountKey> added;
    for (const wallet::AccountKey& key : keys) {
        if (key.type == segwit || key.type == account)
            added.append(key);
    }
    QList<wallet::AccountKey> second = wallet::WalletStore::deriveAccounts(chain::bip39::split("legal winner thank year wave sausage worth useful legal winner thank yellow"), {});
    second.removeIf([segwit](const wallet::AccountKey& key) { return key.type != segwit; });
    if (!controller->addKeysWallet(added, QStringLiteral("Shop"), &error) || !controller->addKeysWallet(second, QStringLiteral("Warehouse"), &error)) {
        check(false, QStringLiteral("wallets for the test: %1").arg(error));
        return;
    }
    const quint32 shop = controller->store().wallets.at(0).id;
    const quint32 warehouse = controller->store().wallets.at(1).id;
    wallet::WalletModel* model = controller->model();
    check(waitFor([&] { return !controller->loadingSaved() && model->find(shop, bitcoin, segwit, 0, 0, 19) >= 0 && model->find(shop, ethereum, account, 0, 0, 19) >= 0 && model->find(warehouse, bitcoin, segwit, 0, 0, 19) >= 0; }, 20000), QStringLiteral("they have rows"));
    const auto rowOf = [&](quint32 wallet, quint32 index) { return model->at(model->find(wallet, bitcoin, segwit, 0, 0, index)); };
    const auto nextOf = [&](quint32 wallet) { return controller->nextReceive(wallet, bitcoin, segwit); };

    auto* portfolio = window.findChild<ui::PortfolioPage*>();
    auto* addresses = window.findChild<ui::AddressesPage*>();
    auto* receive = window.findChild<ui::ReceivePage*>();
    auto* wallets = window.findChild<ui::WalletsPage*>();
    ui::PlaceBar* bar = window.placeBar();
    auto* detail = bar->findChild<QLabel*>(QStringLiteral("placeDetail"));
    const auto goToWallet = [&](quint32 wallet) {
        ui::Place place;
        place.root = ui::Place::Root::Wallets;
        place.wallet = wallet;
        window.goTo(place, ui::Place::Tab::Holdings);
    };
    const auto goToCoin = [&](wallet::Asset asset, quint32 wallet) {
        ui::Place place;
        place.asset = asset;
        place.wallet = wallet;
        window.goTo(place, ui::Place::Tab::Addresses);
    };
    const auto goToAddress = [&](const wallet::AddressRow& row) {
        ui::Place place;
        place.root = ui::Place::Root::Addresses;
        place.address = row;
        window.goTo(place, ui::Place::Tab::Holdings);
    };

    // How many hold something, before anything has answered.
    goToWallet(shop);
    check(detail->text().contains(QStringLiteral("— funded of")), QStringLiteral("no count of what holds something while nothing has answered: %1").arg(detail->text()));
    window.showPage(QStringLiteral("portfolio"));
    pause(50);
    const auto portfolioCount = [&] {
        for (const QLabel* label : portfolio->findChildren<QLabel*>()) {
            if (label->isVisible() && label->text().contains(QLatin1String(" funded of ")))
                return label->text();
        }
        return QString();
    };
    check(portfolioCount().contains(QStringLiteral("— funded of")), QStringLiteral("on the Portfolio neither: %1").arg(portfolioCount()));

    // What every other page says of one coin of a wallet, the Wallets page says too.
    {
        wallet::AddressRow one = rowOf(shop, 0);
        one.checked = true;
        one.used = true;
        one.confirmed = 100000000;
        model->upsert(one);
        for (int i = 0; i < model->size(); ++i) {
            if (model->at(i).network != bitcoin || model->at(i).checked)
                continue;
            wallet::AddressRow each = model->at(i);
            each.checked = true;
            model->upsert(each);
        }
        window.showPage(QStringLiteral("wallets"));
        auto* table = wallets->findChild<QTableView*>();
        check(waitFor([&] { return cellOf(table, rowNamed(table, QStringLiteral("Shop")), QStringLiteral("BTC")) == QStringLiteral("1"); }, 3000), QStringLiteral("Bitcoin answered, its figure on the Wallets page is plain though Ethereum has not: %1").arg(cellOf(table, rowNamed(table, QStringLiteral("Shop")), QStringLiteral("BTC"))));
        check(cellOf(table, rowNamed(table, QStringLiteral("Shop")), QStringLiteral("ETH")) == QStringLiteral("—"), QStringLiteral("and Ethereum says nothing yet"));
        check(cellOf(table, rowNamed(table, QStringLiteral("Warehouse")), QStringLiteral("USD")) == QStringLiteral("—"), QStringLiteral("with no price a wallet of nothing has no $0.00 there either: %1").arg(cellOf(table, rowNamed(table, QStringLiteral("Warehouse")), QStringLiteral("USD"))));
    }
    answerAll(model, shop);
    answerAll(model, warehouse);
    auto* holdingsPage = window.findChild<ui::HoldingsPage*>();
    const auto countUnder = [&] {
        for (const QLabel* each : holdingsPage->findChildren<QLabel*>()) {
            if (each->isVisible() && each->text().contains(QLatin1String("hold something")))
                return each->text();
        }
        return QString();
    };

    // A wallet of nothing, every row answered, with Hide zero on: a page with nothing on it would say nothing.
    window.showPage(QStringLiteral("portfolio"));
    portfolio->findChild<QCheckBox*>(QStringLiteral("hideZero"))->setChecked(true);
    goToWallet(warehouse);
    pause(50);
    holdingsPage->table()->refresh();
    check(holdingsPage->table()->rowsShown() == 1 && holdingsPage->table()->rowsHolding() == 0, QStringLiteral("what it has a key for stays, known to hold nothing (%1)").arg(holdingsPage->table()->rowsShown()));
    check(countUnder() == QStringLiteral("0 of 1 hold something"), QStringLiteral("with a count that says so: %1").arg(countUnder()));

    // An old price says it is old, wherever it makes a figure.
    {
        net::PriceQuote old;
        old.asset = btc;
        old.usd = 50000.0;
        old.at = QDateTime::currentDateTimeUtc().addSecs(-7200);
        old.source = QStringLiteral("Test");
        controller->prices()->restore({old});
        check(controller->priceNote(btc).contains(QLatin1String("price from")), QStringLiteral("a price two hours old says so: %1").arg(controller->priceNote(btc)));
        goToCoin(btc, 0);
        check(detail->text().contains(QLatin1String("price from")), QStringLiteral("on a coin: %1").arg(detail->text()));
        goToWallet(shop);
        check(detail->text().contains(QLatin1String("price from")), QStringLiteral("on a wallet: %1").arg(detail->text()));
        goToAddress(rowOf(shop, 0));
        check(detail->text().contains(QLatin1String("price from")), QStringLiteral("and on an address: %1").arg(detail->text()));
        setPrices(controller);
        check(controller->priceNote(btc).isEmpty(), QStringLiteral("one just read does not"));
        goToWallet(shop);
        check(!detail->text().contains(QLatin1String("price from")), QStringLiteral("nor does what it makes"));
    }

    // A figure goes by its own scope, and in its own coin's digits.
    {
        wallet::AddressRow half = rowOf(warehouse, 0);
        half.used = true;
        half.confirmed = 50000000;
        model->upsert(half);
        const wallet::Asset usdt = wallet::findAsset(QStringLiteral("ethereum/usdt"))->asset;
        wallet::AddressRow dollars = model->at(model->find(shop, ethereum, account, 0, 0, 5));
        dollars.setBalance(usdt, chain::Amount::fromU64(5000000));
        model->upsert(dollars);
        const ui::Holding five = ui::holdingOf(controller, usdt, shop);
        check(five.amountText() == QStringLiteral("5.00 USDT") && five.usd == 5.0, QStringLiteral("six decimals are six: %1").arg(five.amountText()));
        goToWallet(warehouse);
        auto* title = bar->findChild<QLabel*>(QStringLiteral("placeTitle"));
        check(title->text() == QStringLiteral("$50,000.00"), QStringLiteral("a wallet's page has its own total, not all of them: %1").arg(title->text()));
        check(countUnder() == QStringLiteral("1 of 1 hold something"), QStringLiteral("and its own count: %1").arg(countUnder()));
        ui::Place place;
        place.asset = btc;
        window.goTo(place, ui::Place::Tab::Holdings);
        pause(50);
        holdingsPage->table()->refresh();
        QTableView* wallets = holdingsPage->table()->view();
        check(cellOf(wallets, rowNamed(wallets, QStringLiteral("Shop")), QStringLiteral("Share")) == QStringLiteral("66.7%"), QStringLiteral("a coin's wallets have their shares of it: %1").arg(cellOf(wallets, rowNamed(wallets, QStringLiteral("Shop")), QStringLiteral("Share"))));
        half.used = false;
        half.confirmed = 0;
        model->upsert(half);
        dollars.setBalance(usdt, chain::Amount());
        model->upsert(dollars);
    }

    // Receive: what it says of what is on screen.
    window.showPage(QStringLiteral("receive"));
    receive->showFor(shop, btc);
    pause(50);
    auto* line = receive->findChild<QLabel*>(QStringLiteral("receiveState"));
    auto* notCounted = receive->findChild<QLabel*>(QStringLiteral("receiveNotCounted"));
    auto* label = receive->findChild<QLineEdit*>(QStringLiteral("receiveLabel"));
    auto* amount = receive->findChild<QLineEdit*>(QStringLiteral("receiveAmount"));
    auto* list = receive->findChild<QTableWidget*>(QStringLiteral("receiveList"));
    auto* status = receive->findChild<QLabel*>(QStringLiteral("receiveStatus"));
    auto* walletCombo = receive->findChild<QComboBox*>(QStringLiteral("receiveWallet"));
    auto* coinCombo = receive->findChild<QComboBox*>(QStringLiteral("receiveCoin"));
    check(!(receive->findChild<QLabel*>(QStringLiteral("receiveAddress"))->textInteractionFlags() & Qt::TextSelectableByMouse), QStringLiteral("the address on Receive cannot be selected past its Copy button"));
    check(notCounted->isVisible(), QStringLiteral("what nobody has been given says a scan cannot be seen, and what to do"));
    {
        const quint32 first = nextOf(shop).index;
        window.showPage(QStringLiteral("portfolio"));
        window.showPage(QStringLiteral("receive"));
        pause(50);
        check(nextOf(shop).index == first && receive->shownAddress() == nextOf(shop).address, QStringLiteral("going elsewhere with nothing typed marks nothing"));
        amount->setText(QStringLiteral("abc"));
        window.showPage(QStringLiteral("portfolio"));
        check(nextOf(shop).index == first, QStringLiteral("nor does what is no amount"));
        window.showPage(QStringLiteral("receive"));
        pause(50);

        // A label says who it is for.
        const QString typedFor = receive->shownAddress();
        label->insert(QStringLiteral("Order 7"));
        window.showPage(QStringLiteral("portfolio"));
        check(controller->addressLabel(typedFor) == QLatin1String("Order 7") && controller->isGivenOut(rowOf(shop, first)), QStringLiteral("a label typed goes with it, and it counts as given out"));
        window.showPage(QStringLiteral("receive"));
        pause(50);
        check(receive->shownAddress() != typedFor && label->text().isEmpty(), QStringLiteral("and the next one comes up clean"));

        // So does an amount, also when the way out is Receive itself.
        const wallet::ReceiveOffer asked = nextOf(shop);
        amount->setText(QStringLiteral("0.5"));
        window.showPage(QStringLiteral("receive"));
        pause(50);
        check(controller->isGivenOut(rowOf(shop, asked.index)) && receive->shownAddress() != asked.address && amount->text().isEmpty(), QStringLiteral("an amount says the same"));

        // Copy counts, and the page stops asking.
        buttonNamed(receive, QStringLiteral("Copy address"))->click();
        check(!notCounted->isVisible() && !nextOf(shop).address.isEmpty(), QStringLiteral("once copied, that line goes"));
        // Of what stays on screen, a label is kept as the field is left.
        const QString held = receive->shownAddress();
        label->insert(QStringLiteral("Order 8"));
        emit label->editingFinished();
        check(controller->addressLabel(held) == QLatin1String("Order 8"), QStringLiteral("a label for what stays on screen is kept as the field is left"));
        buttonNamed(receive, QStringLiteral("New address"))->click();
    }

    // What is copied from an address's own page counts, and Show QR brings up that one as it stands.
    {
        const wallet::ReceiveOffer offered = nextOf(shop);
        goToAddress(rowOf(shop, offered.index));
        buttonNamed(bar, QStringLiteral("Show QR"))->click();
        pause(50);
        check(receive->shownAddress() == offered.address && !controller->isGivenOut(rowOf(shop, offered.index)), QStringLiteral("Show QR brings up that address, and marks nothing"));
        goToAddress(rowOf(shop, offered.index));
        buttonNamed(bar, QStringLiteral("Copy address"))->click();
        check(controller->isGivenOut(rowOf(shop, offered.index)), QStringLiteral("Copy address on its page marks it"));
        window.showPage(QStringLiteral("receive"));
        pause(50);
    }

    // A double click on New address is one.
    {
        const int before = controller->givenOutCount(shop, bitcoin, segwit);
        QPushButton* next = buttonNamed(receive, QStringLiteral("New address"));
        const QPoint at = next->rect().center();
        const auto send = [&](QEvent::Type type, Qt::MouseButtons buttons) {
            QMouseEvent event(type, at, next->mapToGlobal(at), Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(next, &event);
        };
        send(QEvent::MouseButtonPress, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
        send(QEvent::MouseButtonDblClick, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
        check(controller->givenOutCount(shop, bitcoin, segwit) == before + 1, QStringLiteral("a double click on New address gives out one (%1 from %2)").arg(controller->givenOutCount(shop, bitcoin, segwit)).arg(before));
    }

    // A label typed for one is not put over another's when the offer moves on by itself.
    {
        window.showPage(QStringLiteral("receive"));
        pause(50);
        const wallet::ReceiveOffer offered = nextOf(shop);
        const wallet::AddressRow after = rowOf(shop, offered.index + 1);
        check(controller->setAddressLabel(after.address, QStringLiteral("Till 3"), &error), QStringLiteral("the one after it has a label of its own"));
        label->insert(QStringLiteral("Order 1"));
        wallet::AddressRow paid = rowOf(shop, offered.index);
        paid.used = true;
        paid.confirmed = 500;
        model->upsert(paid);
        check(waitFor([&] { return receive->shownAddress() == after.address; }, 3000), QStringLiteral("the one on screen is paid, and the offer moves on"));
        check(label->text() == QLatin1String("Till 3"), QStringLiteral("what it is called stands: %1").arg(label->text()));
        check(status->isVisible() && status->text().contains(QLatin1String("moved on")), QStringLiteral("and it says what became of what was typed: %1").arg(status->text()));
        buttonNamed(receive, QStringLiteral("Copy address"))->click();
        check(controller->addressLabel(after.address) == QLatin1String("Till 3"), QStringLiteral("so Copy leaves it"));
        buttonNamed(receive, QStringLiteral("New address"))->click();
    }

    // The Addresses list: a click chooses, two open, and only asking hands out.
    {
        window.showPage(QStringLiteral("addresses"));
        addresses->showWallet(shop);
        pause(50);
        auto* table = addresses->findChild<QTableView*>();
        const int addressColumn = ui::AddressTableModel::ColAddress;
        const auto rowInTable = [&](const QString& address) {
            for (int row = 0; row < table->model()->rowCount(); ++row) {
                if (table->model()->index(row, addressColumn).data().toString() == address) {
                    table->scrollTo(table->model()->index(row, addressColumn));
                    return row;
                }
            }
            return -1;
        };
        const wallet::ReceiveOffer free = nextOf(shop);
        check(ui::handOutNote(controller, rowOf(shop, free.index)).isEmpty(), QStringLiteral("nothing to say of handing out one nobody has had"));
        int at = rowInTable(free.address);
        clickCell(table, at, addressColumn);
        check(!controller->isGivenOut(rowOf(shop, free.index)) && addresses->lastCopied().isEmpty(), QStringLiteral("a click on an address chooses it, and hands nothing out"));
        {
            const QPoint point = table->visualRect(table->model()->index(at, addressColumn)).center();
            const auto send = [&](QEvent::Type type, Qt::MouseButtons buttons) {
                QMouseEvent event(type, point, table->viewport()->mapToGlobal(point), Qt::LeftButton, buttons, Qt::NoModifier);
                QApplication::sendEvent(table->viewport(), &event);
            };
            send(QEvent::MouseButtonPress, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, Qt::NoButton);
            send(QEvent::MouseButtonDblClick, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, Qt::NoButton);
        }
        check(bar->isVisible() && !controller->isGivenOut(rowOf(shop, free.index)), QStringLiteral("nor does opening it with a double click"));
        check(!(bar->findChild<QLabel*>(QStringLiteral("placeTitle"))->textInteractionFlags() & Qt::TextSelectableByMouse), QStringLiteral("and its address there cannot be selected past Copy address either"));
        window.showPage(QStringLiteral("addresses"));
        addresses->showWallet(shop);
        pause(50);
        at = rowInTable(free.address);
        clickCell(table, at, addressColumn);
        pause(150); // the button goes by what is chosen, a few times a second
        buttonNamed(addresses, QStringLiteral("Copy address"))->click();
        check(controller->isGivenOut(rowOf(shop, free.index)) && addresses->lastCopied() == free.address, QStringLiteral("Copy address hands it out, and marks it"));
        at = rowInTable(free.address);
        check(cellOf(table, at, QStringLiteral("State")) == QStringLiteral("Given out"), QStringLiteral("and the list then says so: %1").arg(cellOf(table, at, QStringLiteral("State"))));
        check(ui::handOutNote(controller, rowOf(shop, free.index)).contains(QLatin1String("given out before")), QStringLiteral("and a second time it says it was: %1").arg(ui::handOutNote(controller, rowOf(shop, free.index))));
        goToAddress(rowOf(shop, free.index));
        check(detail->text().contains(QLatin1String("Given out")), QStringLiteral("as does its own page: %1").arg(detail->text()));
        window.showPage(QStringLiteral("addresses"));
        addresses->showWallet(shop);
        pause(50);

        // Ctrl+C is the same.
        const wallet::ReceiveOffer then = nextOf(shop);
        clickCell(table, rowInTable(then.address), addressColumn);
        QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
        QApplication::sendEvent(table, &copy);
        check(controller->isGivenOut(rowOf(shop, then.index)) && addresses->lastCopied() == then.address, QStringLiteral("Ctrl+C hands it out the same way"));

        // All of them, past what the list on Receive holds.
        auto* state = addresses->findChild<QComboBox*>(QStringLiteral("stateFilter"));
        state->setCurrentIndex(3);
        pause(50);
        bool all = table->model()->rowCount() > 0;
        for (int row = 0; row < table->model()->rowCount(); ++row)
            all = all && cellOf(table, row, QStringLiteral("State")) == QStringLiteral("Given out");
        check(all, QStringLiteral("State finds what was given out and is still waiting (%1)").arg(table->model()->rowCount()));
        state->setCurrentIndex(0);

        // What else a copy has to say.
        check(ui::handOutNote(controller, rowOf(shop, 0)).contains(QLatin1String("used before")), QStringLiteral("one with history is said to have it"));
        wallet::AddressRow unanswered = rowOf(shop, 19);
        unanswered.checked = false;
        check(ui::handOutNote(controller, unanswered).contains(QLatin1String("no server")), QStringLiteral("one nobody has answered for is said not to be known"));
        wallet::AddressRow change = model->at(model->find(shop, bitcoin, segwit, 0, 1, 0));
        check(ui::handOutNote(controller, change).contains(QLatin1String("change address")), QStringLiteral("and a change address is said not to be kept"));
    }

    // Asked for a coin the wallet on screen has no key for, it goes to one that has.
    {
        window.showPage(QStringLiteral("receive"));
        walletCombo->setCurrentIndex(walletCombo->findData(warehouse));
        pause(50);
        {
            ui::Place place;
            place.asset = eth;
            window.goTo(place, ui::Place::Tab::Holdings);
            buttonNamed(bar, QStringLiteral("Receive"))->click();
        }
        pause(50);
        check(walletCombo->currentData().toUInt() == shop && coinCombo->currentData().toInt() == int(eth), QStringLiteral("Receive on a coin comes up on that coin, of a wallet that has it"));
        check(receive->shownAddress() == controller->nextReceive(shop, ethereum, account).address, QStringLiteral("with its address: %1").arg(receive->shownAddress()));
        receive->showFor(shop, btc);
        pause(50);
    }

    // Down from where the reader is: a double click keeps the way there, and leaving lets go of it.
    {
        ui::Place place;
        place.root = ui::Place::Root::Wallets;
        place.wallet = shop;
        place.asset = btc;
        window.goTo(place, ui::Place::Tab::Addresses);
        pause(150);
        auto* table = addresses->findChild<QTableView*>();
        check(table->model()->rowCount() >= 1, QStringLiteral("a coin of a wallet has what holds it (%1)").arg(table->model()->rowCount()));

        // What it exports is what it shows.
        const QString file = QDir(wallet::WalletStore::dataDir()).filePath(QStringLiteral("place.csv"));
        check(addresses->exportCsv(file, &error), QStringLiteral("it exports: %1").arg(error));
        {
            QFile read(file);
            read.open(QIODevice::ReadOnly);
            const int lines = int(read.readAll().count('\n'));
            check(lines == table->model()->rowCount() + 1, QStringLiteral("only what the place shows (%1 lines for %2 rows)").arg(lines).arg(table->model()->rowCount()));
        }
        QFile::remove(file);

        const QPoint point = table->visualRect(table->model()->index(0, ui::AddressTableModel::ColAddress)).center();
        const auto send = [&](QEvent::Type type, Qt::MouseButtons buttons) {
            QMouseEvent event(type, point, table->viewport()->mapToGlobal(point), Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(table->viewport(), &event);
        };
        send(QEvent::MouseButtonPress, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
        send(QEvent::MouseButtonDblClick, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
        const QStringList path = bar->pathTexts();
        check(path.size() == 4 && path.at(0) == QLatin1String("Wallets") && path.at(1) == QLatin1String("Shop") && path.at(2) == QLatin1String("Bitcoin"), QStringLiteral("a double click goes on down: %1").arg(path.join(QLatin1Char('>'))));

        // Left from its Addresses tab, for a page that has no list.
        window.goTo(place, ui::Place::Tab::Addresses);
        pause(100);
        window.showPage(QStringLiteral("settings"));
        check(!bar->isVisible(), QStringLiteral("going elsewhere leaves the place"));
        window.showPage(QStringLiteral("addresses"));
        pause(150);
        check(table->model()->rowCount() == model->size(), QStringLiteral("and the page is whole again (%1 of %2)").arg(table->model()->rowCount()).arg(model->size()));
    }

    // An address nothing reads any more says so, and handing it out brings it back under watch.
    {
        const wallet::AddressRow gone = rowOf(shop, 19);
        goToAddress(gone);
        model->dropRows([&gone](const wallet::AddressRow& row) { return !(row.wallet == gone.wallet && row.network == gone.network && row.type == gone.type && row.change == 0 && row.index == gone.index); });
        bar->refresh();
        check(detail->text().contains(QLatin1String("No scan follows")) && !detail->text().contains(QLatin1String("BTC")), QStringLiteral("a row that has gone shows no figure of what nothing reads: %1").arg(detail->text()));
        auto* holdings = window.findChild<ui::HoldingsPage*>();
        holdings->table()->refresh();
        check(holdings->table()->rowsTotal() == 0, QStringLiteral("and the table under it has none"));
        bool counted = false;
        for (const QLabel* each : holdings->findChildren<QLabel*>())
            counted = counted || (each->isVisible() && each->text().contains(QLatin1String("hold something")));
        check(!counted, QStringLiteral("nor a count of nothing"));
        window.showPage(QStringLiteral("receive"));
        receive->showAddress(gone);
        pause(50);
        buttonNamed(receive, QStringLiteral("Copy address"))->click();
        check(controller->isGivenOut(gone) && receive->lastCopied() == gone.address, QStringLiteral("copied all the same, it counts as given out, row or none"));
        receive->showFor(shop, btc);
        pause(50);
    }

    // With no server answering, nothing says what it cannot know.
    {
        const QDateTime earlier = QDateTime::currentDateTime().addSecs(-3600);
        controller->setStatusForTest([earlier](wallet::NetworkStatus& status) {
            status.connecting = true;
            status.balancesAt = earlier;
        });
        window.showPage(QStringLiteral("receive"));
        receive->showFor(shop, btc);
        pause(50);
        check(line->text().contains(QLatin1String("no server is answering")), QStringLiteral("what is offered says no server answers: %1").arg(line->text()));
        int waitingRow = -1;
        for (int row = 0; row < list->rowCount() && waitingRow < 0; ++row) {
            if (list->item(row, 3)->text() != QLatin1String("0") && !list->item(row, 3)->text().contains(QLatin1String("BTC")))
                waitingRow = row;
        }
        check(waitingRow >= 0 && list->item(waitingRow, 3)->text() == QStringLiteral("—") && list->item(waitingRow, 4)->text() == QStringLiteral("—"), QStringLiteral("what was given out has no zero nobody stands behind"));
        check(waitingRow >= 0 && list->item(waitingRow, 5)->text().startsWith(QLatin1String("As of")), QStringLiteral("and says as of when: %1").arg(waitingRow >= 0 ? list->item(waitingRow, 5)->text() : QString()));
        emit list->cellClicked(waitingRow, 0);
        check(line->text().contains(QLatin1String("nothing had come in as of")), QStringLiteral("shown, it says the same: %1").arg(line->text()));
        goToWallet(shop);
        check(detail->text().contains(QLatin1String("as of")), QStringLiteral("a wallet's page says as of when: %1").arg(detail->text()));
        goToAddress(rowOf(shop, 0));
        check(detail->text().contains(QLatin1String("as of")), QStringLiteral("and an address's: %1").arg(detail->text()));
        window.showPage(QStringLiteral("portfolio"));
        pause(50);
        {
            bool bitcoinSaid = false;
            bool etherSaid = false;
            for (const QLabel* each : portfolio->findChildren<QLabel*>()) {
                if (!each->isVisible())
                    continue;
                bitcoinSaid = bitcoinSaid || each->text().contains(QLatin1String("BTC balances from"));
                etherSaid = etherSaid || each->text().contains(QLatin1String("ETH balances from"));
            }
            check(bitcoinSaid && etherSaid, QStringLiteral("and the Portfolio says it of each"));
        }
        controller->setStatusForTest([](wallet::NetworkStatus& status) { status.connecting = true; });
        window.showPage(QStringLiteral("receive"));
        receive->showFor(shop, btc);
        pause(50);
        check(line->text().contains(QLatin1String("No server is answering")), QStringLiteral("before any whole answer it says only that: %1").arg(line->text()));
        controller->setStatusForTest(answering);
    }

    // Of what was only saved, the same.
    {
        controller->setShowingSavedForTest(true);
        receive->showAddress(rowOf(shop, 0));
        check(line->text().contains(QLatin1String("as far as was saved")), QStringLiteral("what holds something says it was saved: %1").arg(line->text()));
        wallet::AddressRow emptied = rowOf(shop, 0);
        emptied.confirmed = 0;
        model->upsert(emptied);
        pause(500);
        check(line->text().contains(QLatin1String("as far as was saved")) && !line->text().contains(QLatin1String("now")), QStringLiteral("what held nothing has no \"now\": %1").arg(line->text()));
        emptied.confirmed = 100000000;
        model->upsert(emptied);
        controller->setShowingSavedForTest(false);
    }

    // What is in no block yet is not paid yet, wherever it shows.
    {
        receive->showFor(shop, btc);
        pause(50);
        const wallet::ReceiveOffer offered = nextOf(shop);
        buttonNamed(receive, QStringLiteral("Copy address"))->click();
        wallet::AddressRow paid = rowOf(shop, offered.index);
        const qint64 before = ui::holdingOf(controller, btc, shop).incoming;
        paid.used = true;
        paid.unconfirmed = 2000;
        model->upsert(paid);
        check(ui::holdingOf(controller, btc, shop).incoming == before + 2000 && ui::holdingOf(controller, btc).incoming == before + 2000, QStringLiteral("it is counted apart"));
        check(ui::holdingNote(controller, ui::holdingOf(controller, btc, shop)).contains(QLatin1String("not in a block yet")), QStringLiteral("and a coin says so"));
        receive->showAddress(paid);
        check(line->text().contains(QLatin1String("not in a block yet")), QStringLiteral("as does Receive: %1").arg(line->text()));
        check(waitFor([&] { return list->rowCount() > 0 && list->item(0, 5)->text() == QLatin1String("Not in a block yet"); }, 3000), QStringLiteral("and its list, rather than Received: %1").arg(list->rowCount() > 0 ? list->item(0, 5)->text() : QString()));
        goToAddress(paid);
        check(detail->text().contains(QLatin1String("not in a block yet")), QStringLiteral("and its own page"));
        goToWallet(shop);
        check(detail->text().contains(QLatin1String("not in a block yet")), QStringLiteral("and its wallet"));
        window.showPage(QStringLiteral("addresses"));
        addresses->showWallet(shop);
        pause(50);
        auto* table = addresses->findChild<QTableView*>();
        bool said = false;
        for (int row = 0; row < table->model()->rowCount(); ++row)
            said = said || (table->model()->index(row, ui::AddressTableModel::ColAddress).data().toString() == paid.address && cellOf(table, row, QStringLiteral("State")) == QStringLiteral("Not in a block yet"));
        check(said, QStringLiteral("and the Addresses list"));
        paid.unconfirmed = 0;
        paid.confirmed = 2000;
        model->upsert(paid);
        check(ui::holdingOf(controller, btc, shop).incoming == before, QStringLiteral("in a block, it is no longer counted apart"));
        window.showPage(QStringLiteral("receive"));
        receive->showFor(shop, btc);
        check(waitFor([&] { return list->rowCount() > 0 && list->item(0, 5)->text() == QLatin1String("Received"); }, 3000), QStringLiteral("and reads Received"));
    }

    // A row not every lookup has answered for is a lower bound in the lists too.
    {
        wallet::AddressRow part = model->at(model->find(shop, ethereum, account, 0, 0, 2));
        check(controller->markGivenOut(part, &error), QStringLiteral("an Ethereum address given out"));
        part.setBalance(eth, chain::Amount::fromU64(1000000000000000000ULL));
        part.checked = false;
        model->upsert(part);
        window.showPage(QStringLiteral("addresses"));
        addresses->showWallet(shop);
        pause(50);
        auto* table = addresses->findChild<QTableView*>();
        int found = -1;
        for (int row = 0; row < table->model()->rowCount() && found < 0; ++row) {
            if (table->model()->index(row, ui::AddressTableModel::ColAddress).data().toString() == part.address)
                found = row;
        }
        check(found >= 0 && cellOf(table, found, QStringLiteral("Balance")).startsWith(QStringLiteral("≥")) && cellOf(table, found, QStringLiteral("USD")).startsWith(QStringLiteral("≥")), QStringLiteral("on the Addresses list: %1").arg(found >= 0 ? cellOf(table, found, QStringLiteral("Balance")) : QString()));
        window.showPage(QStringLiteral("receive"));
        receive->showFor(shop, eth);
        pause(50);
        int partLine = -1;
        for (int row = 0; row < list->rowCount() && partLine < 0; ++row) {
            if (list->item(row, 0)->text() == part.address)
                partLine = row;
        }
        check(partLine >= 0 && list->item(partLine, 3)->text().startsWith(QStringLiteral("≥")), QStringLiteral("and in what was given out: %1").arg(partLine >= 0 ? list->item(partLine, 3)->text() : QString()));
        goToCoin(eth, 0);
        check(bar->findChild<QLabel*>(QStringLiteral("placeTitle"))->text().startsWith(QStringLiteral("≥")), QStringLiteral("and on the coin's page"));
        part.checked = true;
        part.setBalance(eth, chain::Amount());
        model->upsert(part);
    }

    // No server will say anything of one, and the scan has ended: it stops nothing after it.
    {
        const wallet::ReceiveOffer offered = nextOf(shop);
        check(offered.state == wallet::ReceiveOffer::State::Ready, QStringLiteral("there is one to offer"));
        wallet::AddressRow silent = rowOf(shop, offered.index);
        silent.checked = false;
        model->upsert(silent);
        check(nextOf(shop).state == wallet::ReceiveOffer::State::Checking, QStringLiteral("while the scan is on its way, it waits for an answer"));
        controller->setStatusForTest([](wallet::NetworkStatus& status) {
            status.connecting = false;
            status.scanning = false;
        });
        const wallet::ReceiveOffer past = nextOf(shop);
        check(past.state == wallet::ReceiveOffer::State::Ready && past.index > offered.index, QStringLiteral("once the scan has ended, the next that has answered is offered (%1)").arg(past.index));
        // Nor is nothing said of a coin while something has not answered.
        const wallet::Asset usdc = wallet::findAsset(QStringLiteral("ethereum/usdc"))->asset;
        const auto says = [&](const QString& text) {
            for (const QLabel* each : addresses->findChildren<QLabel*>()) {
                if (each->isVisible() && each->text() == text)
                    return true;
            }
            return false;
        };
        wallet::AddressRow silentEther = model->at(model->find(shop, ethereum, account, 0, 0, 3));
        silentEther.checked = false;
        model->upsert(silentEther);
        goToCoin(usdc, shop);
        pause(150);
        check(!says(QStringLiteral("Nothing here holds it")), QStringLiteral("nobody says nothing holds a coin while an address has not answered"));
        silentEther.checked = true;
        model->upsert(silentEther);
        pause(150);
        goToCoin(usdc, shop);
        pause(150);
        check(says(QStringLiteral("Nothing here holds it")), QStringLiteral("once all have, it can be said"));
        // A chain chosen in a wallet's place is a filter like any other.
        ui::Place place;
        place.root = ui::Place::Root::Wallets;
        place.wallet = warehouse;
        window.goTo(place, ui::Place::Tab::Addresses);
        pause(100);
        QComboBox* chains = nullptr;
        for (QComboBox* each : addresses->findChildren<QComboBox*>()) {
            if (each->findData(int(ethereum)) >= 0)
                chains = each;
        }
        chains->setCurrentIndex(chains->findData(int(ethereum)));
        pause(150);
        check(says(QStringLiteral("No results")) && !says(QStringLiteral("Nothing here holds it")), QStringLiteral("a chain the wallet has no key on is no results, not nothing held"));
        chains->setCurrentIndex(0);
        controller->setStatusForTest(answering);
        silent.checked = true;
        model->upsert(silent);
    }

    // A second account of one kind: what is copied of it counts too.
    {
        QList<wallet::AccountKey> two = wallet::WalletStore::deriveAccounts(chain::bip39::split("letter advice cage absurd amount doctor acoustic avoid letter advice cage above"), {});
        two.removeIf([segwit](const wallet::AccountKey& key) { return key.type != segwit; });
        QList<wallet::AccountKey> other = wallet::WalletStore::deriveAccounts(chain::bip39::split("zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong"), {});
        other.removeIf([segwit](const wallet::AccountKey& key) { return key.type != segwit; });
        other.first().account = 1;
        two.append(other.first());
        if (controller->addKeysWallet(two, QStringLiteral("Two accounts"), &error)) {
            const quint32 both = controller->store().wallets.last().id;
            check(waitFor([&] { return model->find(both, bitcoin, segwit, 1, 0, 3) >= 0 && model->find(both, bitcoin, segwit, 0, 0, 3) >= 0; }, 15000), QStringLiteral("with rows on both accounts"));
            answerAll(model, both);
            const wallet::AddressRow row = model->at(model->find(both, bitcoin, segwit, 1, 0, 3));
            check(controller->markGivenOut(row, &error) && controller->isGivenOut(row), QStringLiteral("an address of the second account is marked"));
            check(!controller->isGivenOut(model->at(model->find(both, bitcoin, segwit, 0, 0, 3))), QStringLiteral("and the same index of the first is not"));
            check(controller->givenOutCount(both, bitcoin, segwit) == 1 && controller->givenOutRows(both, bitcoin, segwit, 10).size() == 1, QStringLiteral("and it is listed"));
            receive->showFor(both, btc);
            pause(50);
            emit list->cellClicked(0, 0);
            check(receive->shownAddress() == row.address, QStringLiteral("and a click on that line brings up that address: %1").arg(receive->shownAddress()));
            // Its wallet goes while that is on screen.
            check(controller->removeWallet(both, &error), QStringLiteral("the wallet goes: %1").arg(error));
            pause(50);
            check(receive->shownAddress() != row.address && walletCombo->findData(both) < 0, QStringLiteral("and so does what was on screen of it"));
        } else {
            check(false, QStringLiteral("a wallet with two accounts: %1").arg(error));
        }
    }

    // A name is not a word of the program's.
    {
        check(controller->renameWallet(warehouse, QStringLiteral("Send"), &error), QStringLiteral("a wallet named as a word the program has"));
        const QString language = ui::LanguageManager::instance()->language();
        ui::LanguageManager::instance()->setLanguage(QStringLiteral("es"));
        pause(50);
        auto* filter = addresses->findChild<QComboBox*>(QStringLiteral("walletFilter"));
        check(walletCombo->itemText(walletCombo->findData(warehouse)) == QLatin1String("Send") && filter->itemText(filter->findData(warehouse)) == QLatin1String("Send"), QStringLiteral("keeps its name in other languages: %1").arg(walletCombo->itemText(walletCombo->findData(warehouse))));
        check(filter->itemText(0) == QStringLiteral("Todas las billeteras"), QStringLiteral("while the words of the list are translated: %1").arg(filter->itemText(0)));
        check(ui::trText(QStringLiteral("%1 of the addresses it gave out have had nothing come in yet. If its seed is added again, they will be offered to someone else: only this PC knows they were given out.")) != QStringLiteral("%1 of the addresses it gave out have had nothing come in yet. If its seed is added again, they will be offered to someone else: only this PC knows they were given out."), QStringLiteral("and what a removal warns of too"));
        ui::LanguageManager::instance()->setLanguage(language);
        pause(50);
    }

    // More than the list holds: it says so, and where the rest are.
    {
        QList<wallet::AccountKey> many = wallet::WalletStore::deriveAccounts(chain::bip39::split("letter advice cage absurd amount doctor acoustic avoid letter advice cage above"), {});
        many.removeIf([segwit](const wallet::AccountKey& key) { return key.type != segwit; });
        many.first().given.runs.append({0, 250});
        check(controller->addKeysWallet(many, QStringLiteral("Many"), &error), QStringLiteral("a wallet that has given out 250: %1").arg(error));
        const quint32 id = controller->store().wallets.last().id;
        check(waitFor([&] { return model->find(id, bitcoin, segwit, 0, 0, 249) >= 0; }, 15000), QStringLiteral("with its rows"));
        answerAll(model, id);
        receive->showFor(id, btc);
        window.showPage(QStringLiteral("receive"));
        receive->showFor(id, btc);
        pause(50);
        bool titled = false;
        for (const QLabel* each : receive->findChildren<QLabel*>())
            titled = titled || (each->isVisible() && each->text() == QStringLiteral("Given out: the latest 200 of 250"));
        check(list->rowCount() == 200 && titled, QStringLiteral("the list says it holds the latest 200 of 250 (%1)").arg(list->rowCount()));
    }

    check(controller->removeAllWallets(&error), QStringLiteral("they all go: %1").arg(error));
    pause(100);
}

// What was given out holds a window whatever was found before it, when Settings says what a gap would cost.
void testGapsOverGivenOut(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    const wallet::Network bitcoin = wallet::findNetwork(QStringLiteral("bitcoin"))->network;
    auto controller = std::make_unique<WalletController>();
    offline(controller.get());
    wallet::AccountKey key;
    for (const wallet::AccountKey& each : keys) {
        if (each.type == chain::ScriptType::P2WPKH)
            key = each;
    }
    wallet::ScanSettings wide = controller->store().settings;
    wide.utxoReceiveGap = 40;
    check(controller->applySettings(wide, &error), QStringLiteral("gaps wider than the least: %1").arg(error));
    key.given.runs.append({0, 300});
    QList<wallet::AccountKey> added{key};
    if (!controller->addKeysWallet(added, QStringLiteral("Tills"), &error)) {
        check(false, QStringLiteral("a wallet for the test: %1").arg(error));
        return;
    }
    const quint32 id = controller->store().wallets.first().id;
    check(controller->store().wallets.first().accounts.first().given.end() == 300, QStringLiteral("that has given out 300 addresses"));
    wallet::WalletModel* model = controller->model();
    check(waitFor([&] { return !controller->loadingSaved() && model->find(id, bitcoin, key.type, 0, 0, 299) >= 0; }, 20000), QStringLiteral("with its rows"));
    // Nearly all of them paid, with 31 in the middle still waiting.
    for (quint32 index = 0; index < 300; ++index) {
        if (index >= 100 && index <= 130)
            continue;
        wallet::AddressRow row = model->at(model->find(id, bitcoin, key.type, 0, 0, index));
        row.checked = true;
        row.used = true;
        model->upsert(row);
    }
    wallet::ScanSettings narrow = wide;
    narrow.utxoReceiveGap = 20;
    const WalletController::GapImpact impact = controller->gapImpact(narrow);
    check(impact.used == 0, QStringLiteral("a smaller gap leaves out none of what was given out: %1").arg(impact.used));
    check(controller->removeAllWallets(&error), QStringLiteral("it goes: %1").arg(error));
    controller.reset();

    // What could not be written is tried a last time as the window goes.
    {
        controller = std::make_unique<WalletController>();
        offline(controller.get());
        wallet::AccountKey plain = key;
        plain.given = {};
        QList<wallet::AccountKey> one{plain};
        check(controller->addKeysWallet(one, QStringLiteral("Tills"), &error), QStringLiteral("a wallet for the last try: %1").arg(error));
        wallet::WalletModel* rows = controller->model();
        const quint32 own = controller->store().wallets.first().id;
        check(waitFor([&] { return !controller->loadingSaved() && rows->find(own, bitcoin, plain.type, 0, 0, 3) >= 0; }, 15000), QStringLiteral("with its rows"));
        answerAll(rows, own);
        const QString path = wallet::WalletStore::filePath();
        const QByteArray whole = [&path] {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        }();
        {
            QFile file(path);
            file.open(QIODevice::Append);
            file.write("x");
        }
        check(!controller->markGivenOut(rows->at(rows->find(own, bitcoin, plain.type, 0, 0, 3)), &error) && controller->givenOutOwed(), QStringLiteral("a file changed outside it cannot be written"));
        {
            QFile file(path);
            file.open(QIODevice::WriteOnly | QIODevice::Truncate);
            file.write(whole);
        }
        controller.reset();
        wallet::WalletStore store;
        check(store.load(&error) && store.wallets.first().accounts.first().given.contains(3), QStringLiteral("back as it was, the last try as the window goes writes it: %1").arg(error));
        controller = std::make_unique<WalletController>();
        controller->prices()->useTestServer(QStringLiteral("http://127.0.0.1:1"));
        check(controller->loadExisting(&error), QStringLiteral("it opens: %1").arg(error));
        waitFor([&] { return !controller->loadingSaved(); }, 15000);
        check(controller->removeAllWallets(&error), QStringLiteral("it goes: %1").arg(error));
    }
}

} // namespace

// In AlertsTest.cpp.
void runAlertsTest(const std::function<void(bool, const QString&)>& check, const QList<wallet::AccountKey>& someKeys);

void runScreensTest(const Check& check, QString* report, const QList<wallet::AccountKey>& someKeys)
{
    testQr(check, report);
    testGivenOutRuns(check);
    testGivenOutFile(check, someKeys);
    testGivenOut(check, someKeys);
    // Leaves the box as it found it: this is one setting per Windows user.
    const QString hideZero = SettingsRepo().get(QStringLiteral("ui.hideZero"));
    const QString hideDust = SettingsRepo().get(QStringLiteral("ui.hideDust"));
    const QString alerts = SettingsRepo().get(QStringLiteral("ui.paymentAlerts"));
    // These go by every row: what dust does to a list has checks of its own.
    SettingsRepo().set(QStringLiteral("ui.hideDust"), QStringLiteral("0"));
    testFiguresHonest(check, someKeys);
    testScreens(check, someKeys);
    testHandingOut(check, someKeys);
    testGapsOverGivenOut(check, someKeys);
    runAlertsTest(check, someKeys);
    SettingsRepo().set(QStringLiteral("ui.hideZero"), hideZero);
    SettingsRepo().set(QStringLiteral("ui.hideDust"), hideDust);
    SettingsRepo().set(QStringLiteral("ui.paymentAlerts"), alerts);
}

// In ScanTest.cpp, with the servers it fakes.
void runGivenOutScans(const std::function<void(bool, const QString&)>& check, const QList<wallet::AccountKey>& someKeys);

// The screens with something in them, to look at: made-up figures on public test seeds, in a throw-away
// folder and with no server. For the docs and for reports, which say so.
bool writeScreenPictures(const QString& folder)
{
    QDir().mkpath(folder);
    QString error;
    ui::MainWindow window;
    window.resize(1600, 950);
    window.show();
    WalletController* controller = window.controller();
    offline(controller);
    controller->setStatusForTest([](wallet::NetworkStatus& status) {
        status.connecting = false;
        status.scanning = false;
    });
    struct Named {
        const char* name;
        const char* words;
        bool ethereum;
    };
    const QList<Named> seeds = {
        {"Online shop", "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about", true},
        {"Warehouse EU", "legal winner thank year wave sausage worth useful legal winner thank yellow", true},
        {"Partner keys", "letter advice cage absurd amount doctor acoustic avoid letter advice cage above", false},
    };
    for (const Named& seed : seeds) {
        QList<wallet::AccountKey> keys = wallet::WalletStore::deriveAccounts(chain::bip39::split(seed.words), {});
        const bool ethereum = seed.ethereum;
        keys.removeIf([ethereum](const wallet::AccountKey& key) { return !ethereum && key.type == chain::ScriptType::EthAccount; });
        if (!controller->addKeysWallet(keys, QString::fromLatin1(seed.name), &error))
            return false;
    }
    QList<net::PriceQuote> quotes;
    for (const wallet::AssetInfo& info : wallet::assets()) {
        net::PriceQuote quote;
        quote.asset = info.asset;
        quote.usd = info.dollar ? 1.0 : (info.symbol == QLatin1String("BTC") ? 77000.0 : 2250.0);
        quote.at = QDateTime::currentDateTimeUtc();
        quote.source = QStringLiteral("DefiLlama");
        quotes.append(quote);
    }
    controller->prices()->restore(quotes);
    wallet::WalletModel* model = controller->model();
    const quint32 shop = controller->store().wallets.at(0).id;
    const quint32 warehouse = controller->store().wallets.at(1).id;
    const quint32 partner = controller->store().wallets.at(2).id;
    const wallet::Network bitcoin = wallet::findNetwork(QStringLiteral("bitcoin"))->network;
    const wallet::Network ethereum = wallet::findNetwork(QStringLiteral("ethereum"))->network;
    const chain::ScriptType segwit = chain::ScriptType::P2WPKH;
    const chain::ScriptType account = chain::ScriptType::EthAccount;
    if (!waitFor([&] {
            return !controller->loadingSaved() && controller->labelsBlocker().isEmpty() && model->find(shop, bitcoin, segwit, 0, 0, 12) >= 0 && model->find(warehouse, bitcoin, segwit, 0, 0, 12) >= 0
                   && model->find(partner, bitcoin, segwit, 0, 0, 12) >= 0 && model->find(shop, ethereum, account, 0, 0, 12) >= 0 && model->find(warehouse, ethereum, account, 0, 0, 12) >= 0;
        }, 30000))
        return false;
    const auto holdBitcoin = [&](quint32 wallet, quint32 index, qint64 satoshi) {
        wallet::AddressRow row = model->at(model->find(wallet, bitcoin, segwit, 0, 0, index));
        row.checked = true;
        row.used = true;
        row.confirmed = satoshi;
        model->upsert(row);
    };
    const auto holdOnEthereum = [&](quint32 wallet, quint32 index, const char* asset, quint64 units) {
        wallet::AddressRow row = model->at(model->find(wallet, ethereum, account, 0, 0, index));
        row.checked = true;
        row.used = true;
        row.setBalance(wallet::findAsset(QString::fromLatin1(asset))->asset, chain::Amount::fromU64(units));
        model->upsert(row);
    };
    holdBitcoin(shop, 0, 84000000);
    holdBitcoin(shop, 3, 37500000);
    holdBitcoin(shop, 4, 12250000);
    holdBitcoin(warehouse, 0, 12500000);
    holdBitcoin(warehouse, 1, 4100000);
    holdBitcoin(partner, 0, 5000000);
    holdOnEthereum(shop, 0, "ethereum/eth", 2400000000000000000ULL);
    holdOnEthereum(shop, 1, "ethereum/usdt", 18450000000ULL);
    holdOnEthereum(shop, 2, "ethereum/usdc", 6200000000ULL);
    holdOnEthereum(warehouse, 0, "ethereum/usdt", 3000000000ULL);
    holdOnEthereum(warehouse, 1, "ethereum/eth", 650000000000000000ULL);
    const auto answerAllRows = [&] {
        for (const quint32 wallet : {shop, warehouse, partner})
            answerAll(model, wallet);
    };
    if (!waitFor([&] { answerAllRows(); pause(100); return controller->balancesComplete() || ui::totalOf(controller).complete; }, 20000))
        return false;

    // What Receive gave out, for its list.
    const QStringList orders = {QStringLiteral("Order 1042"), QStringLiteral("Order 1043"), QStringLiteral("Till 2")};
    quint32 given = 0;
    for (const QString& order : orders) {
        answerAllRows();
        const wallet::ReceiveOffer offer = controller->nextReceive(shop, bitcoin, segwit);
        if (offer.state != wallet::ReceiveOffer::State::Ready || !controller->markGivenOut(model->at(model->find(shop, bitcoin, segwit, 0, 0, offer.index)), &error) || !controller->setAddressLabel(offer.address, order, &error))
            return false;
        given = offer.index;
    }
    {
        wallet::AddressRow paid = model->at(model->find(shop, bitcoin, segwit, 0, 0, given));
        paid.used = true;
        paid.confirmed = 1850000;
        model->upsert(paid);
    }
    answerAllRows();

    const auto picture = [&](const QString& name) {
        answerAllRows();
        pause(300);
        answerAllRows();
        pause(500);
        return window.grab().save(folder + QLatin1Char('/') + name + QStringLiteral(".png"));
    };
    bool all = true;
    window.showPage(QStringLiteral("portfolio"));
    window.findChild<QCheckBox*>(QStringLiteral("hideZero"))->setChecked(true);
    all = picture(QStringLiteral("portfolio")) && all;
    ui::Place place;
    place.asset = wallet::networkInfo(bitcoin).coin;
    window.goTo(place, ui::Place::Tab::Holdings);
    all = picture(QStringLiteral("coin")) && all;
    place.wallet = shop;
    window.goTo(place, ui::Place::Tab::Addresses);
    all = picture(QStringLiteral("coin-wallet-addresses")) && all;
    place = {};
    place.root = ui::Place::Root::Wallets;
    place.wallet = shop;
    window.goTo(place, ui::Place::Tab::Holdings);
    all = picture(QStringLiteral("wallet")) && all;
    place.address = model->at(model->find(shop, ethereum, account, 0, 0, 1));
    place.root = ui::Place::Root::Addresses;
    place.wallet = 0;
    window.goTo(place, ui::Place::Tab::Holdings);
    all = picture(QStringLiteral("address")) && all;
    window.showPage(QStringLiteral("receive"));
    window.findChild<ui::ReceivePage*>()->showFor(shop, wallet::networkInfo(bitcoin).coin);
    all = picture(QStringLiteral("receive")) && all;
    controller->removeAllWallets(&error);
    QCoreApplication::processEvents();
    return all;
}

StoreTestResult runScreensTestAlone()
{
    StoreTestResult result;
    const Check check = [&result](bool ok, const QString& what) {
        ++result.total;
        if (ok)
            ++result.passed;
        result.report += (ok ? QStringLiteral("ok    ") : QStringLiteral("FAIL  ")) + what + QLatin1Char('\n');
    };
    const QList<wallet::AccountKey> keys = wallet::WalletStore::deriveAccounts(chain::bip39::split("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"), {});
    runScreensTest(check, &result.report, keys);
    runGivenOutScans(check, keys);
    QFile::remove(wallet::WalletStore::filePath());
    return result;
}

bool writeQrSamples(const QString& folder)
{
    using Level = QrCode::Correction;
    QDir().mkpath(folder);
    QJsonArray manifest;
    int number = 0;
    const auto add = [&](const QByteArray& text, Level level) {
        const QrCode code = QrCode::encode(text, level);
        const QString file = QStringLiteral("qr-%1.png").arg(++number, 3, 10, QLatin1Char('0'));
        if (code.isNull() || !code.toImage(5).save(folder + QLatin1Char('/') + file))
            return false;
        manifest.append(QJsonObject{{QStringLiteral("file"), file},
                                    {QStringLiteral("text"), QString::fromLatin1(text)},
                                    {QStringLiteral("version"), code.version()},
                                    {QStringLiteral("mask"), code.mask()},
                                    {QStringLiteral("level"), int(level)}});
        return true;
    };
    // What the app puts in one, then every version at every level, full.
    for (const char* text : {"bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu", "37VucYSaXLCAsxYyAPfbSi9eh4iEcbShgf", "1LqBGSKuX5yYUonjxT5qGfpUsXKYYWeabA",
                             "0x9858EfFD232B4033E47d90003D41EC34EcaEda94", "bitcoin:bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu?amount=0.015",
                             "bitcoin:bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu?amount=20999999.12345678"}) {
        if (!add(QByteArray(text), Level::Medium))
            return false;
    }
    QRandomGenerator random(20261102);
    for (const Level level : {Level::Low, Level::Medium, Level::Quartile, Level::High}) {
        for (int version = 1; version <= QrCode::kMaxVersion; ++version) {
            QByteArray text(QrCode::capacity(version, level), Qt::Uninitialized);
            static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~:/?#";
            for (char& c : text)
                c = kChars[random.bounded(int(sizeof(kChars)) - 1)];
            if (!add(text, level))
                return false;
        }
    }
    QFile file(folder + QStringLiteral("/samples.json"));
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(QJsonDocument(manifest).toJson()) > 0;
}

} // namespace app
