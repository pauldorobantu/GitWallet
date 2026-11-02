#include "app/StoreTest.h"

#include "app/WalletController.h"
#include "chain/Bip39.h"
#include "core/SettingsRepo.h"
#include "net/PriceFeed.h"
#include "ui/HistoryTableModel.h"
#include "ui/Holdings.h"
#include "ui/MainWindow.h"
#include "ui/PlaceBar.h"
#include "ui/QrCode.h"
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
        check(controller->markGivenOut(id, bitcoin, chain::ScriptType::P2WPKH, offer.index, &error), QStringLiteral("and it is written down: %1").arg(error));
    }
    check(controller->markGivenOut(id, bitcoin, chain::ScriptType::P2WPKH, 1, &error) && controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH).index == 4, QStringLiteral("saying so again of an older one moves nothing back"));
    const QList<int> given = controller->givenOutRows(id, bitcoin, chain::ScriptType::P2WPKH, 10);
    check(given.size() == 3 && model->at(given.first()).index == 3 && model->at(given.last()).index == 1, QStringLiteral("what was given out is listed, the latest first, and what was used before it is not (%1)").arg(given.size()));
    check(controller->givenOutRows(id, bitcoin, chain::ScriptType::P2WPKH, 2).size() == 2, QStringLiteral("as far as asked"));

    // It reaches past the gap, without a server: an address given out has to be followed.
    const quint32 gap = quint32(controller->store().settings.utxoReceiveGap);
    check(waitFor([&] { return rowAt(4 + gap - 1) >= 0; }, 10000), QStringLiteral("the window reaches its gap past what was given out"));
    pause(200);
    check(rowAt(4 + gap) < 0, QStringLiteral("and no further"));
    check(controller->markGivenOut(id, bitcoin, chain::ScriptType::P2WPKH, gap + 3, &error), QStringLiteral("given out up to where the gap ended"));
    check(waitFor([&] { return rowAt(2 * gap + 3) >= 0; }, 10000), QStringLiteral("it keeps up"));
    answerAll(model, id);
    check(controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH).index == gap + 4, QStringLiteral("and the next one is past all of those"));

    // Ethereum has its own, and the same rules.
    check(waitFor([&] { return rowAt(3, chain::ScriptType::EthAccount) >= 0; }, 15000), QStringLiteral("an Ethereum account has its rows"));
    answerAll(model, id);
    offer = controller->nextReceive(id, ethereum, chain::ScriptType::EthAccount);
    check(offer.state == wallet::ReceiveOffer::State::Ready && offer.index == 0 && offer.address == addressAt(account, 0, ethereum), QStringLiteral("its next address comes from its key"));
    row = model->at(rowAt(0, chain::ScriptType::EthAccount));
    row.address = row.address.toLower();
    model->upsert(row);
    check(controller->nextReceive(id, ethereum, chain::ScriptType::EthAccount).state == wallet::ReceiveOffer::State::Ready, QStringLiteral("its case is only a checksum: what is kept may differ in it"));
    check(controller->markGivenOut(id, ethereum, chain::ScriptType::EthAccount, 0, &error) && controller->nextReceive(id, ethereum, chain::ScriptType::EthAccount).index == 1, QStringLiteral("and it moves on by itself"));

    // A wallet can only say that of its own key.
    check(controller->markGivenOut(id + 100, bitcoin, chain::ScriptType::P2WPKH, 500, &error) && controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH).index == gap + 4, QStringLiteral("a wallet that is not there moves nothing"));

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
        quint32 bitcoinIssued = 0;
        quint32 bitcoinFrom = 0;
        quint32 etherIssued = 0;
        for (const wallet::AccountKey& key : store.wallets.first().accounts) {
            if (key.type == chain::ScriptType::EthAccount) {
                etherIssued = key.issued;
            } else {
                bitcoinIssued = key.issued;
                bitcoinFrom = key.issuedFrom;
            }
        }
        check(bitcoinIssued == gap + 4 && bitcoinFrom == 1 && etherIssued == 1, QStringLiteral("and says what was given out (%1 from %2, %3)").arg(bitcoinIssued).arg(bitcoinFrom).arg(etherIssued));
    }
    controller = std::make_unique<WalletController>();
    controller->prices()->useTestServer(QStringLiteral("http://127.0.0.1:1"));
    check(controller->loadExisting(&error), QStringLiteral("a new start opens it: %1").arg(error));
    check(waitFor([&] { return !controller->loadingSaved() && controller->model()->size() > 0; }, 15000), QStringLiteral("with what it kept"));
    model = controller->model();
    check(controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH).state != wallet::ReceiveOffer::State::NoKey, QStringLiteral("its keys are there"));
    if (controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH).state == wallet::ReceiveOffer::State::Ready)
        check(controller->nextReceive(id, bitcoin, chain::ScriptType::P2WPKH).index == gap + 4, QStringLiteral("and goes on where it stood"));
    check(waitFor([&] { return model->find(id, bitcoin, chain::ScriptType::P2WPKH, 0, 0, 2 * gap + 3) >= 0; }, 10000), QStringLiteral("with its window as far as what was given out says"));
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

void testScreens(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    ui::MainWindow window;
    window.show();
    window.resize(1280, 800); // the least the window goes to, on any screen with room for it
    WalletController* controller = window.controller();
    offline(controller);
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

    // Part of it has.
    wallet::AddressRow row = model->at(model->find(shop, bitcoin, chain::ScriptType::P2WPKH, 0, 0, 0));
    row.checked = true;
    row.used = true;
    row.confirmed = 150000000;
    model->upsert(row);
    settle();
    check(total->text() == QStringLiteral("≥ $150,000.00"), QStringLiteral("what is known so far is a lower bound (%1)").arg(total->text()));
    check(cellOf(coins, rowNamed(coins, QStringLiteral("Bitcoin")), QStringLiteral("Holdings")) == QStringLiteral("≥ 1.5 BTC"), QStringLiteral("and so is the coin's (%1)").arg(cellOf(coins, rowNamed(coins, QStringLiteral("Bitcoin")), QStringLiteral("Holdings"))));

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
    pause(QApplication::doubleClickInterval() + 50);
    clickCell(coins, rowNamed(coins, QStringLiteral("Ethereum")));
    QTableView* inside = holdings->table()->view();
    check(stack->currentWidget() == holdings && bar->isVisible(), QStringLiteral("a click on a coin opens it"));
    check(bar->pathTexts() == QStringList({QStringLiteral("Portfolio"), QStringLiteral("Ethereum")}), QStringLiteral("and the path says where: %1").arg(bar->pathTexts().join(QLatin1Char('>'))));
    check(tabTexts(tabs) == QStringList({QStringLiteral("Wallets"), QStringLiteral("Addresses"), QStringLiteral("History")}), QStringLiteral("with a tab for each way to look at it"));
    settle();
    int rowOfWarehouse = rowNamed(inside, QStringLiteral("Warehouse"));
    if (rowOfWarehouse >= 0)
        check(cellOf(inside, rowOfWarehouse, QStringLiteral("Holdings")) == QStringLiteral("—"), QStringLiteral("a wallet with no key on its network shows no zero"));
    check(rowNamed(inside, QStringLiteral("Shop")) == 0 && cellOf(inside, 0, QStringLiteral("Holdings")) == QStringLiteral("1 ETH"), QStringLiteral("the one that holds it comes first, with what it holds"));

    window.goTo([&] { ui::Place place; place.asset = btc; return place; }(), ui::Place::Tab::Holdings);
    settle();
    check(bar->pathTexts() == QStringList({QStringLiteral("Portfolio"), QStringLiteral("Bitcoin")}), QStringLiteral("another coin says so"));
    pause(QApplication::doubleClickInterval() + 50);
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
    pause(QApplication::doubleClickInterval() + 50);
    clickCell(inside, rowNamed(inside, QStringLiteral("Bitcoin")));
    check(bar->pathTexts() == QStringList({QStringLiteral("Wallets"), QStringLiteral("Shop"), QStringLiteral("Bitcoin")}), QStringLiteral("and a coin of it after that: %1").arg(bar->pathTexts().join(QLatin1Char('>'))));

    // A double click is not two: the second lands on what the first opened.
    window.showPage(QStringLiteral("portfolio"));
    settle();
    pause(QApplication::doubleClickInterval() + 50);
    clickCell(coins, rowNamed(coins, QStringLiteral("Bitcoin")));
    settle();
    clickCell(inside, 0);
    const QStringList afterTwo = bar->pathTexts();
    check(afterTwo == QStringList({QStringLiteral("Portfolio"), QStringLiteral("Bitcoin")}) || QApplication::doubleClickInterval() < 450, QStringLiteral("two quick clicks open one thing: %1").arg(afterTwo.join(QLatin1Char('>'))));

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
    check(history->findChild<QTableView*>() != nullptr && !saysSo == false, QStringLiteral("back on all of it"));
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
    check(receive->shownPayload() == next.address && !qr->pixmap().isNull(), QStringLiteral("and its QR code says just that"));
    const ReadBack plain = readSymbol(QrCode::encode(receive->shownPayload().toUtf8()), QrCode::Correction::Medium);
    check(plain.wrong.isEmpty() && QString::fromUtf8(plain.data) == next.address, QStringLiteral("which reads back: %1").arg(plain.wrong));

    auto* amount = receive->findChild<QLineEdit*>(QStringLiteral("receiveAmount"));
    auto* status = receive->findChild<QLabel*>(QStringLiteral("receiveStatus"));
    amount->setText(QStringLiteral("0.015"));
    check(receive->shownPayload() == QStringLiteral("bitcoin:%1?amount=0.015").arg(next.address), QStringLiteral("an amount goes into it as BIP 21 asks: %1").arg(receive->shownPayload()));
    const ReadBack uri = readSymbol(QrCode::encode(receive->shownPayload().toUtf8()), QrCode::Correction::Medium);
    check(uri.wrong.isEmpty() && QString::fromUtf8(uri.data) == receive->shownPayload(), QStringLiteral("and reads back: %1").arg(uri.wrong));
    for (const QString& bad : {QStringLiteral("1e5"), QStringLiteral("-1"), QStringLiteral("0,5"), QStringLiteral("0.123456789"), QStringLiteral("0"), QStringLiteral("abc")}) {
        amount->setText(bad);
        check(receive->shownPayload() == next.address && status->isVisible(), QStringLiteral("%1 is no amount: the code stays the plain address, and it says so").arg(bad));
    }
    amount->clear();

    // A label stays on this PC.
    auto* label = receive->findChild<QLineEdit*>(QStringLiteral("receiveLabel"));
    label->setText(QStringLiteral("Order 1001"));
    check(!receive->shownPayload().contains(QLatin1String("Order")), QStringLiteral("a label does not go into the code"));

    auto* list = receive->findChild<QTableWidget*>(QStringLiteral("receiveList"));
    const QString before = receive->shownAddress();
    const int listed = list->rowCount();
    buttonNamed(receive, QStringLiteral("New address"))->click();
    pause(100);
    check(receive->shownAddress() != before && !receive->shownAddress().isEmpty(), QStringLiteral("New address gives another"));
    check(label->text().isEmpty(), QStringLiteral("with its own label to come"));
    check(controller->addressLabel(before) == QLatin1String("Order 1001"), QStringLiteral("the label went with the one before it"));
    check(list->rowCount() == listed + 1 && list->item(0, 0)->text() == before && list->item(0, 1)->text() == QLatin1String("Order 1001"), QStringLiteral("which is listed as given out, the latest first"));
    check(list->item(0, 5)->text() == QLatin1String("Waiting"), QStringLiteral("waiting for its payment (%1)").arg(list->item(0, 5)->text()));

    // A click on what was given out shows it again, and moves nothing on.
    const quint32 mark = controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH).index;
    emit list->cellClicked(0, 0);
    check(receive->shownAddress() == before && receive->shownPayload() == before, QStringLiteral("what was given out can be shown again"));
    check(controller->nextReceive(shop, bitcoin, chain::ScriptType::P2WPKH).index == mark, QStringLiteral("which marks nothing"));
    check(label->text() == QLatin1String("Order 1001"), QStringLiteral("with its label"));

    // A payment to it says so: on the page, and the next address stays what it was.
    {
        const int position = model->find(shop, bitcoin, chain::ScriptType::P2WPKH, 0, 0, next.index);
        wallet::AddressRow paid = model->at(position);
        paid.used = true;
        paid.unconfirmed = 2000;
        model->upsert(paid);
        auto* line = receive->findChild<QLabel*>(QStringLiteral("receiveState"));
        check(waitFor([&] { return line->text().contains(QLatin1String("Holds")); }, 3000), QStringLiteral("a payment to it shows at once: %1").arg(line->text()));
        check(waitFor([&] { return list->item(0, 5)->text() == QLatin1String("Received"); }, 3000), QStringLiteral("and in the list"));
    }

    // Ethereum and its tokens: the same address, and what may be sent to it.
    buttonNamed(receive, QStringLiteral("New address"))->click();
    auto* coin = receive->findChild<QComboBox*>(QStringLiteral("receiveCoin"));
    const wallet::AssetInfo* usdt = wallet::findAsset(QStringLiteral("ethereum/usdt"));
    coin->setCurrentIndex(coin->findData(int(usdt->asset)));
    pause(100);
    check(receive->shownAddress().startsWith(QLatin1String("0x")), QStringLiteral("a token is received on its network's address: %1").arg(receive->shownAddress()));
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
    check(receive->shownAddress().startsWith(QLatin1String("bc1")), QStringLiteral("so it goes back to what it has: %1").arg(receive->shownAddress()));

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

    // One wallet: a step through it says nothing.
    {
        ui::Place place;
        place.asset = btc;
        window.goTo(place, ui::Place::Tab::Holdings);
    }
    check(bar->pathTexts() == QStringList({QStringLiteral("Portfolio"), QStringLiteral("Bitcoin")}) && stack->currentWidget() == addresses, QStringLiteral("with one wallet a coin opens straight on its addresses"));
    check(tabTexts(tabs) == QStringList({QStringLiteral("Addresses"), QStringLiteral("History")}), QStringLiteral("without a tab of one wallet"));
    window.showPage(QStringLiteral("portfolio"));

    check(controller->removeAllWallets(&error), QStringLiteral("and they can all go: %1").arg(error));
    pause(100);
    check(buttonNamed(portfolio, QStringLiteral("Add wallet")) != nullptr, QStringLiteral("the Portfolio is back to offering to add one"));
    window.showPage(QStringLiteral("receive"));
    check(receive->shownAddress().isEmpty(), QStringLiteral("and Receive has nothing to offer"));
    SettingsRepo().set(QStringLiteral("ui.hideZero"), QStringLiteral("1"));
}

} // namespace

void runScreensTest(const Check& check, QString* report, const QList<wallet::AccountKey>& someKeys)
{
    testQr(check, report);
    testGivenOut(check, someKeys);
    testScreens(check, someKeys);
}

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
        if (offer.state != wallet::ReceiveOffer::State::Ready || !controller->markGivenOut(shop, bitcoin, segwit, offer.index, &error) || !controller->setAddressLabel(offer.address, order, &error))
            return false;
        given = offer.index;
    }
    {
        wallet::AddressRow paid = model->at(model->find(shop, bitcoin, segwit, 0, 0, given - 2));
        paid.used = true;
        paid.confirmed = 1850000;
        model->upsert(paid);
    }
    answerAllRows();

    const auto picture = [&](const QString& name) {
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
    runScreensTest(check, &result.report, wallet::WalletStore::deriveAccounts(chain::bip39::split("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"), {}));
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
