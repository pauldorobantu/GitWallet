#include "app/ScaleTest.h"
#include "app/StoreTest.h"
#include "app/WalletController.h"
#include "app/Walkthrough.h"
#include "chain/Bip39.h"
#include "chain/SelfTest.h"
#include "net/PriceFeed.h"
#include "ui/LanguageManager.h"
#include "ui/MainWindow.h"
#include "ui/ThemeManager.h"
#include "wallet/History.h"
#include "wallet/UtxoHistory.h"
#include "wallet/UtxoScanner.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QStandardPaths>
#include <QString>
#include <QTimer>

#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h> // GetSecurityInfo; not in windows.h under WIN32_LEAN_AND_MEAN
#endif

namespace {

// A WIN32 (no-console) build has no stdout when launched from a terminal;
// attach to the parent console so --selftest output is visible there.
void attachParentConsole()
{
#ifdef _WIN32
    // Already redirected to a file or pipe: the output goes there as it is.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD type = (out && out != INVALID_HANDLE_VALUE) ? GetFileType(out) : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE)
        return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
}

QString argValue(const QStringList& args, const QString& name)
{
    const qsizetype i = args.indexOf(name);
    return (i >= 0 && i + 1 < args.size()) ? args.at(i + 1) : QString();
}

// The real wallet's folder: the one WalletStore uses without --data-dir.
// Mirrors WalletStore::dataDir(), but creates nothing.
QString realDataDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/GitWallet");
    return dir;
}

// Whether two paths name the same folder. The spellings are compared the way
// NTFS compares them (case-insensitive); existing folders also by volume and
// file ID, so 8.3 short names, \\?\ paths, junctions and symbolic links match.
bool sameFolder(const QString& a, const QString& b)
{
    const auto clean = [](const QString& p) { return QDir::cleanPath(QDir(p).absolutePath()); };
    if (clean(a).compare(clean(b), Qt::CaseInsensitive) == 0) return true;
#ifdef _WIN32
    const auto fileId = [](const QString& path, BY_HANDLE_FILE_INFORMATION* info) {
        const QString native = QDir::toNativeSeparators(path);
        const HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(native.utf16()), 0,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const bool ok = GetFileInformationByHandle(h, info) != FALSE;
        CloseHandle(h);
        return ok;
    };
    BY_HANDLE_FILE_INFORMATION x{}, y{};
    if (fileId(a, &x) && fileId(b, &y))
        return x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh
               && x.nFileIndexLow == y.nFileIndexLow;
#endif
    return false;
}

// The name the open window listens on: one per Windows user and wallet folder,
// so a second start finds it and a window on another --data-dir stays apart.
// Hashed: pipe names are listed machine-wide, and a path may not fit in one.
QString instanceName()
{
    const QString dir = wallet::WalletStore::dataDir();
    const QString canonical = QFileInfo(dir).canonicalFilePath(); // links resolved
    const QString key = QDir::homePath() + QLatin1Char('\n') + (canonical.isEmpty() ? dir : canonical);
    // Lowercase: NTFS compares names case-insensitively.
    const QByteArray digest = QCryptographicHash::hash(key.toLower().toUtf8(), QCryptographicHash::Sha256);
    return QStringLiteral("GitWallet-") + QString::fromLatin1(digest.toHex());
}

#ifdef _WIN32
// Whether this Windows user created the pipe `socket` is connected to. Pipe
// names are machine-wide: another account could take GitWallet's name first,
// and a start that obeyed its pipe would never show a window.
bool ownPipe(const QLocalSocket& socket)
{
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(reinterpret_cast<HANDLE>(socket.socketDescriptor()), SE_KERNEL_OBJECT,
                        OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor)
        != ERROR_SUCCESS)
        return false;
    bool own = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
        DWORD size = 0;
        if (GetTokenInformation(token, TokenUser, user, sizeof user, &size))
            own = EqualSid(owner, reinterpret_cast<const TOKEN_USER*>(user)->User.Sid) != FALSE;
        CloseHandle(token);
    }
    LocalFree(descriptor);
    return own;
}
#endif

// Asks the window already open on this wallet folder to come to the front.
// True when one answered: this start then has nothing left to do.
bool raiseRunningWindow(const QString& name)
{
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(1000)) return false;
#ifdef _WIN32
    if (!ownPipe(socket)) return false;
    // Windows lets the other process take the focus only when this one, which
    // the user just started, passes that right on.
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    socket.write("raise\n");
    socket.waitForBytesWritten(1000);
    // The window hangs up once it is in front.
    if (socket.state() != QLocalSocket::UnconnectedState) socket.waitForDisconnected(3000);
    return true;
}

// Answers raiseRunningWindow() for `window`: restores it from the taskbar
// (still maximised if it was) and brings it to the front.
void serveRaiseRequests(QLocalServer* server, QWidget* window)
{
    QObject::connect(server, &QLocalServer::newConnection, window, [server, window] {
        while (QLocalSocket* peer = server->nextPendingConnection()) {
            QObject::connect(peer, &QLocalSocket::disconnected, peer, &QObject::deleteLater);
            QObject::connect(peer, &QLocalSocket::readyRead, peer, [peer, window] {
                // One short line; more than that without a newline is no request.
                if (!peer->canReadLine() && peer->bytesAvailable() < 64) return;
                if (peer->readLine(64).trimmed() == "raise") {
                    window->setWindowState((window->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
                    window->raise();
                    window->activateWindow();
                }
                peer->disconnectFromServer();
            });
        }
    });
    // A window that is closing cannot come forward: later starts open their own.
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, server, &QLocalServer::close);
}

// The public BIP39 test mnemonic (published in BIP84 and every wallet test
// suite). Only ever imported into a throw-away --data-dir for tests.
const char* kPublicTestMnemonic =
    "abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon abandon abandon about";

// Published BIP39 test vectors, for a demonstration with real addresses and real
// network answers on a throw-away --data-dir. Anyone can spend from these: never send
// anything to them.
const char* const kDemoMnemonics[] = {
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about",
    "legal winner thank year wave sausage worth useful legal winner thank yellow",
    "letter advice cage absurd amount doctor acoustic avoid letter advice cage above",
    "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong",
    "ozone drill grab fiber curtain grace pudding thank cruise elder eight picnic",
    "scheme spot photo card baby mountain device kick cradle pact join borrow",
    "cat swing flag economy stadium alone churn speed unique patch report train",
    "vessel ladder alter error federal sibling chat ability sun glass valve picture",
    "gravity machine north sort system female filter attitude volume fold club stay feature office ecology stable "
    "narrow fog",
    "horn tenant knee talent sponsor spell gate clip pulse soap slush warm silver nephew swap uncle crack brave",
    "hamster diagram private dutch cause delay private meat slide toddler razor book happy fancy gospel tennis maple "
    "dilemma loan word shrug inflict delay length",
    "panda eyebrow bullet gorilla call smoke muffin taste mesh discover soft ostrich alcohol speed nation flash devote "
    "level hobby quick inner drive ghost inside",
};

// --scan-report: every row and the totals as JSON, for comparing the scan with
// block explorers and other wallets.
bool writeScanReport(const QString& path, app::WalletController* c, bool timedOut)
{
    const wallet::WalletModel* m = c->model();
    QJsonArray rows;
    for (int i = 0; i < m->size(); ++i) {
        const wallet::AddressRow& r = m->at(i);
        QJsonObject o{
            {QStringLiteral("wallet"), qint64(r.wallet)},
            {QStringLiteral("network"), wallet::networkInfo(r.network).id},
            {QStringLiteral("path"), r.path()},
            {QStringLiteral("address"), r.address},
            {QStringLiteral("checked"), r.checked},
            {QStringLiteral("used"), r.used},
        };
        if (r.isUtxo()) {
            o.insert(QStringLiteral("status"), r.status);
            o.insert(QStringLiteral("confirmed"), QString::number(r.confirmed));
            o.insert(QStringLiteral("unconfirmed"), QString::number(r.unconfirmed));
        } else {
            o.insert(QStringLiteral("nonce"), QString::number(r.nonce));
            for (const wallet::Asset asset : wallet::networkInfo(r.network).assets)
                o.insert(wallet::assetInfo(asset).symbol, QString::fromStdString(r.amount(asset).toDecimalString()));
        }
        rows.append(o);
    }
    QJsonObject totals;
    for (const wallet::AssetInfo& a : wallet::assets())
        totals.insert(a.key, QString::fromStdString(m->total(a.asset).format(a.decimals, a.decimals)));
    QJsonObject prices;
    for (const wallet::AssetInfo& a : wallet::assets())
        prices.insert(a.key, c->prices()->usd(a.asset));
    QJsonArray networks;
    for (const wallet::NetworkInfo& info : wallet::networks()) {
        const wallet::NetworkStatus st = c->networkStatus(info.network);
        networks.append(QJsonObject{{QStringLiteral("id"), info.id},
                                    {QStringLiteral("server"), st.server},
                                    {QStringLiteral("rows"), m->rowsOn(info.network)},
                                    {QStringLiteral("unchecked"), m->uncheckedOn(info.network)},
                                    {QStringLiteral("scanning"), st.scanning},
                                    {QStringLiteral("wrongChain"), st.wrongChain},
                                    {QStringLiteral("failing"), st.failing},
                                    {QStringLiteral("balancesAt"), st.balancesAt.toString(Qt::ISODate)},
                                    {QStringLiteral("live"), st.tiers.live},
                                    {QStringLiteral("rounds"), st.tiers.rounds}});
    }
    QJsonArray accounts; // public keys only — lets a test compare with Electrum watch-only
    const QList<wallet::AccountKey> keys = c->store().allAccounts();
    for (const wallet::AccountKey& k : keys)
        accounts.append(QJsonObject{{QStringLiteral("wallet"), qint64(k.wallet)},
                                    {QStringLiteral("type"), QString::fromLatin1(chain::scriptTypeName(k.type))},
                                    {QStringLiteral("account"), int(k.account)},
                                    {QStringLiteral("xpub"), k.xpub}});
    // Every transaction, and what it did to each address: for comparing with a block explorer.
    QJsonArray history;
    QJsonArray historyState;
    const QList<wallet::UtxoHistory*> histories = c->histories();
    for (const wallet::UtxoHistory* source : histories) {
        const wallet::HistoryBook* book = source->book();
        const wallet::HistoryProgress progress = source->progress();
        historyState.append(QJsonObject{{QStringLiteral("network"), wallet::networkInfo(source->network()).id},
                                        {QStringLiteral("transactions"), book->size()},
                                        {QStringLiteral("addressesLeft"), progress.addressesLeft},
                                        {QStringLiteral("transactionsLeft"), progress.transactionsLeft},
                                        {QStringLiteral("proofsLeft"), progress.proofsLeft},
                                        {QStringLiteral("refused"), progress.refused},
                                        {QStringLiteral("differing"), progress.differing},
                                        {QStringLiteral("unproven"), progress.unproven},
                                        {QStringLiteral("tip"), source->tip()}});
        for (int position = 0; position < book->size(); ++position) {
            const wallet::TxRecord& tx = book->at(position);
            QJsonArray effects;
            for (const wallet::TxEffect& effect : book->effectsOf(position))
                effects.append(QJsonObject{{QStringLiteral("address"), book->addressAt(effect.address).address},
                                           {QStringLiteral("wallet"), qint64(book->addressAt(effect.address).wallet)},
                                           {QStringLiteral("received"), QString::number(effect.received)},
                                           {QStringLiteral("spent"), QString::number(effect.spent)}});
            history.append(QJsonObject{{QStringLiteral("network"), wallet::networkInfo(source->network()).id},
                                       {QStringLiteral("txid"), QString::fromStdString(chain::hashHex(tx.txid))},
                                       {QStringLiteral("height"), tx.height},
                                       {QStringLiteral("time"), QString::number(tx.time)},
                                       {QStringLiteral("fee"), QString::number(tx.fee)},
                                       {QStringLiteral("vsize"), tx.vsize},
                                       {QStringLiteral("seen"), int(tx.seen)},
                                       {QStringLiteral("proven"), tx.proven},
                                       {QStringLiteral("usdThen"), tx.usdThen},
                                       {QStringLiteral("usdThenAt"), QString::number(tx.usdThenAt)},
                                       {QStringLiteral("effects"), effects}});
        }
    }
    const QJsonObject report{
        {QStringLiteral("history"), history},
        {QStringLiteral("historyState"), historyState},
        {QStringLiteral("accounts"), accounts},
        {QStringLiteral("timedOut"), timedOut},
        {QStringLiteral("networks"), networks},
        {QStringLiteral("status"), c->statusText()},
        // Whether the figures below are complete and current, as the UI judges it.
        {QStringLiteral("balancesComplete"), c->balancesComplete()},
        {QStringLiteral("addressLimitReached"), c->addressLimitReached()},
        {QStringLiteral("rowCount"), m->size()},
        {QStringLiteral("usedCount"), m->usedCount()},
        {QStringLiteral("fundedCount"), m->fundedCount()},
        {QStringLiteral("totals"), totals},
        {QStringLiteral("pricesUsd"), prices},
        {QStringLiteral("totalUsd"), c->totalUsd()},
        {QStringLiteral("rows"), rows},
    };
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("GitWallet"));
    QApplication::setOrganizationName(QStringLiteral("GitWallet"));
    QApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    const QStringList args = app.arguments();

    // Prove the derivation engine against published vectors before anything
    // trusts an address it produces.
    const chain::SelfTestResult st = chain::runSelfTest();

    // The list of coins, read here while nothing else runs. A file instead of the
    // built-in list is for tests: refused below without a separate --data-dir.
    const QString coinsFile = argValue(args, QStringLiteral("--coins-file"));
    if (!coinsFile.isEmpty()) wallet::useRegistryFile(coinsFile);
    const QString coinsProblem = wallet::registryProblem();

    if (args.contains(QStringLiteral("--selftest"))) {
        attachParentConsole();
        std::fputs(st.report.c_str(), stdout);
        std::fprintf(stdout, "GitWallet self-test: %d/%d passed\n", st.passed, st.total);
        if (!coinsProblem.isEmpty())
            std::fprintf(stdout, "The list of coins does not load: %s\n", coinsProblem.toUtf8().constData());
        std::fflush(stdout);
        return st.ok && coinsProblem.isEmpty() ? 0 : 1;
    }

    const QString qrSamples = argValue(args, QStringLiteral("--qr-samples"));
    if (!qrSamples.isEmpty())
        return app::writeQrSamples(qrSamples) ? 0 : 1;

    // A separate data folder (tests, screenshots, a second wallet). Trimmed as
    // WalletStore::setDataDir trims it, so a blank value counts as no
    // --data-dir (the real wallet) in the check below as well.
    const QString dataDir = argValue(args, QStringLiteral("--data-dir")).trimmed();
    if (!dataDir.isEmpty()) wallet::WalletStore::setDataDir(dataDir);

    // Test-only switches save, replace or delete wallet.dat, or put the xpubs,
    // addresses and balances into unencrypted files (report, screenshots), so
    // they only run on a separate --data-dir: refused without one, and when the
    // folder WalletStore actually resolved is the real wallet's after all.
    for (const QString& name : {QStringLiteral("--walkthrough"), QStringLiteral("--import-test-mnemonic"),
                                QStringLiteral("--scan-report"), QStringLiteral("--screenshot"),
                                QStringLiteral("--store-test"), QStringLiteral("--screens-test"), QStringLiteral("--screen-pictures"), QStringLiteral("--demo-wallets"),
                                QStringLiteral("--coins-file"), QStringLiteral("--live-places"),
                                QStringLiteral("--scale-test")}) {
        if (!args.contains(name)) continue;
        if (dataDir.isEmpty() || sameFolder(wallet::WalletStore::dataDir(), realDataDir())) {
            attachParentConsole();
            std::fprintf(stderr, "%s needs --data-dir <folder>, a folder other than the real wallet's\n",
                         qPrintable(name));
            return 2;
        }
    }
    // These two write and delete wallet.dat as part of the test, so a folder that
    // already holds one (--data-dir can be a second wallet) is not theirs to use.
    for (const QString& name : {QStringLiteral("--walkthrough"), QStringLiteral("--store-test"), QStringLiteral("--screens-test"), QStringLiteral("--screen-pictures"), QStringLiteral("--scale-test")}) {
        if (args.contains(name) && QFile::exists(wallet::WalletStore::filePath())) {
            attachParentConsole();
            std::fprintf(stderr, "%s needs a --data-dir without a wallet.dat\n", qPrintable(name));
            return 2;
        }
    }

    // --live-places <n>: a whole scan read in rounds, to compare with one followed live.
    const int livePlaces = argValue(args, QStringLiteral("--live-places")).toInt();
    if (livePlaces > 0) wallet::UtxoScanner::setLivePlacesForTest(livePlaces);

    // --store-test: the wallet file and the address model without a window or a
    // network, on the separate --data-dir checked above.
    if (args.contains(QStringLiteral("--store-test")) || args.contains(QStringLiteral("--screens-test"))) {
        attachParentConsole();
        const app::StoreTestResult result = args.contains(QStringLiteral("--store-test")) ? app::runStoreTest() : app::runScreensTestAlone();
        std::fputs(result.report.toUtf8().constData(), stdout);
        std::fprintf(stdout, "GitWallet store test: %d/%d passed\n", result.passed, result.total);
        std::fflush(stdout);
        return result.passed == result.total ? 0 : 1;
    }

    app::ui::theme::ThemeManager::instance()->loadFromSettings();
    app::ui::theme::ThemeManager::instance()->apply();
    const QString screenPictures = argValue(args, QStringLiteral("--screen-pictures"));
    if (!screenPictures.isEmpty())
        return app::writeScreenPictures(screenPictures) ? 0 : 1;
    // The walkthrough finds buttons by their English text, whatever this user chose.
    if (!args.contains(QStringLiteral("--walkthrough")))
        app::ui::LanguageManager::instance()->loadFromSettings();

    if (!coinsProblem.isEmpty()) {
        // A run with nobody in front of it says so and ends: a dialog would wait for ever.
        if (args.contains(QStringLiteral("--scan-report")) || args.contains(QStringLiteral("--walkthrough")) || args.contains(QStringLiteral("--scale-test"))) {
            attachParentConsole();
            std::fprintf(stderr, "The list of coins does not load: %s\n", coinsProblem.toUtf8().constData());
            return 1;
        }
        QMessageBox::critical(nullptr, QStringLiteral("GitWallet"),
                              QStringLiteral("The list of coins does not load, so nothing can be shown.\n\n%1").arg(coinsProblem));
        return 1;
    }
    if (!st.ok) {
        QMessageBox::critical(
            nullptr, QStringLiteral("GitWallet"),
            QStringLiteral("Address-derivation self-test failed (%1/%2). Refusing to start so "
                           "no untrusted address is shown.\n\n%3")
                .arg(st.passed)
                .arg(st.total)
                .arg(QString::fromStdString(st.report)));
        return 1;
    }

    // --walkthrough <dir>: drive the real UI end to end (import with mistakes,
    // scan, pages, settings, reopen, remove) with a screenshot per step. Only on
    // a separate --data-dir (checked above), so it never touches the real wallet.
    const QString walkthroughDir = argValue(args, QStringLiteral("--walkthrough"));
    if (!walkthroughDir.isEmpty()) {
        app::Walkthrough walkthrough(
            [] {
                auto* w = new app::ui::MainWindow;
                w->show();
                return w;
            },
            walkthroughDir);
        walkthrough.start();
        return app.exec();
    }

    // --scale-test <rows>: the real window with that many made-up rows, offline. Only on
    // a separate --data-dir without a wallet (checked above).
    const int scaleRows = argValue(args, QStringLiteral("--scale-test")).toInt();
    if (scaleRows > 0) {
        attachParentConsole();
        app::ui::MainWindow window;
        window.show();
        const QString timings = app::runWindowScaleTest(&window, scaleRows);
        std::fputs(timings.toUtf8().constData(), stdout);
        std::fflush(stdout);
        return timings.contains(QLatin1String("FAIL")) ? 1 : 0;
    }

    // One window per wallet folder: a second start brings the open one to the
    // front instead (WalletController's lock already keeps a second window from
    // writing). Test runs and --page open their own window on a throw-away
    // folder, often beside a normal one, so they skip this. --demo-wallets does not:
    // its shortcut, started twice, should raise the demo it already opened.
    bool singleWindow = true;
    for (const QString& name : {QStringLiteral("--walkthrough"), QStringLiteral("--import-test-mnemonic"),
                                QStringLiteral("--scan-report"), QStringLiteral("--screenshot"),
                                QStringLiteral("--page")})
        singleWindow = singleWindow && !args.contains(name);
    QLocalServer instance; // listens before the window is built: a start meanwhile finds it
    if (singleWindow) {
        const QString name = instanceName();
        if (raiseRunningWindow(name)) return 0;
        instance.setSocketOptions(QLocalServer::UserAccessOption); // this Windows user only
        if (!instance.listen(name))
            qWarning().noquote() << "[instance] not listening:" << instance.errorString();
    }

    app::ui::MainWindow window;
    app::WalletController* controller = window.controller();
    if (instance.isListening()) serveRaiseRequests(&instance, &window);
    window.show(); // the saved wallet loads on the first event-loop pass

    // --import-test-mnemonic: the PUBLIC test seed into a throw-away --data-dir
    // (checked above), so it never lands in the real wallet.
    if (args.contains(QStringLiteral("--import-test-mnemonic"))) {
        QTimer::singleShot(0, controller, [controller] {
            if (controller->hasWallet() || controller->needsPassword()) return;
            QString error;
            if (!controller->addMnemonicWallet(chain::bip39::split(kPublicTestMnemonic), {},
                                               QStringLiteral("Test"), &error))
                qWarning().noquote() << "[test] import failed:" << error;
        });
    }

    // --demo-wallets: the published test seeds as wallets, on a throw-away --data-dir
    // (checked above), so the app can be seen working on real data without a real seed.
    if (args.contains(QStringLiteral("--demo-wallets"))) {
        QTimer::singleShot(0, controller, [&window, controller] {
            if (controller->hasWallet() || controller->needsPassword()) return;
            QList<wallet::WalletRequest> requests;
            try {
                int number = 0;
                for (const char* mnemonic : kDemoMnemonics) {
                    wallet::WalletRequest request;
                    const std::vector<std::string> words = chain::bip39::split(mnemonic);
                    if (!chain::bip39::validate(words)) {
                        qWarning().noquote() << "[demo] not a valid phrase:" << mnemonic;
                        continue;
                    }
                    request.accounts = wallet::WalletStore::deriveAccounts(words, {});
                    request.label = QStringLiteral("Demo %1").arg(++number);
                    requests.append(request);
                }
            } catch (const std::exception& e) {
                qWarning().noquote() << "[demo] could not derive the keys:" << e.what();
            }
            QString error;
            if (controller->addWallets(&requests, &error) == 0)
                qWarning().noquote() << "[demo] not added:" << error;
            window.showPage(QStringLiteral("wallets"));
        });
    }

    const QString page = argValue(args, QStringLiteral("--page"));
    if (!page.isEmpty()) window.showPage(page);

    // --place coin:<key>,wallet:<number>,tab:<holdings|addresses|history>: for a picture of one.
    const QString placeSpec = argValue(args, QStringLiteral("--place"));
    if (!placeSpec.isEmpty()) {
        QTimer::singleShot(2000, &window, [&window, controller, placeSpec] {
            app::ui::Place place;
            app::ui::Place::Tab tab = app::ui::Place::Tab::Holdings;
            for (const QString& part : placeSpec.split(QLatin1Char(','))) {
                const QString value = part.section(QLatin1Char(':'), 1);
                if (part.startsWith(QLatin1String("coin:"))) {
                    if (const wallet::AssetInfo* asset = wallet::findAsset(value))
                        place.asset = asset->asset;
                } else if (part.startsWith(QLatin1String("wallet:"))) {
                    place.wallet = controller->store().wallets.value(value.toInt()).id;
                    if (!place.asset)
                        place.root = app::ui::Place::Root::Wallets;
                } else if (part.startsWith(QLatin1String("tab:"))) {
                    tab = value == QLatin1String("addresses") ? app::ui::Place::Tab::Addresses : value == QLatin1String("history") ? app::ui::Place::Tab::History : app::ui::Place::Tab::Holdings;
                }
            }
            window.goTo(place, tab);
        });
    }

    const QString shot = argValue(args, QStringLiteral("--screenshot"));
    const QString reportPath = argValue(args, QStringLiteral("--scan-report"));

    if (!reportPath.isEmpty()) {
        // Wait for both chains' initial scan (or the timeout), then write the
        // report (and the screenshot, if asked) and quit. Only on a separate
        // --data-dir (checked above): the report holds the xpubs in plain text.
        const int timeoutMs = qBound(10, argValue(args, QStringLiteral("--timeout")).toInt(), 3600) * 1000;
        auto finish = [&window, controller, reportPath, shot](bool timedOut) {
            static bool done = false;
            if (done) return;
            done = true;
            if (!shot.isEmpty()) window.grab().save(shot);
            const bool ok = writeScanReport(reportPath, controller, timedOut);
            QCoreApplication::exit(ok ? (timedOut ? 3 : 0) : 4);
        };
        // The balances first, then the histories they lead to, then what those were worth then, for as
        // long as a source takes to say: all of it has to hold when the report is written.
        const auto historyDone = [controller] {
            const QList<wallet::UtxoHistory*> histories = controller->histories();
            for (const wallet::UtxoHistory* history : histories) {
                if (history->running() && history->progress().working())
                    return false;
            }
            return true;
        };
        // What a server refused for a moment is asked again in a moment: a minute at most, and what
        // is left then is what the report is to say.
        const auto nothingRefused = [controller] {
            const QList<wallet::UtxoHistory*> histories = controller->histories();
            for (const wallet::UtxoHistory* history : histories) {
                const wallet::HistoryProgress progress = history->progress();
                if (progress.refused > 0 || progress.unproven > 0)
                    return false;
            }
            return true;
        };
        const auto pricesDone = [controller] {
            const QList<wallet::UtxoHistory*> histories = controller->histories();
            for (const wallet::UtxoHistory* history : histories) {
                const wallet::HistoryBook* book = history->book();
                for (int position = 0; position < book->size(); ++position) {
                    if (book->at(position).proven && book->at(position).usdThen <= 0.0)
                        return false;
                }
            }
            return true;
        };
        QObject::connect(controller, &app::WalletController::scanFinished, controller, [finish, historyDone, nothingRefused, pricesDone, controller] {
            auto* wait = new QTimer(controller);
            wait->setInterval(500);
            auto clock = std::make_shared<QElapsedTimer>();
            auto settled = std::make_shared<int>(0);
            clock->start();
            QObject::connect(wait, &QTimer::timeout, controller, [finish, historyDone, nothingRefused, pricesDone, wait, clock, settled] {
                if (!historyDone()) {
                    *settled = 0;
                    clock->restart();
                    return;
                }
                if (!nothingRefused() && clock->elapsed() < 60000)
                    return;
                // Long enough for what adds up to be looked at twice. Then prices have up to 30 s:
                // what predates them never gets one.
                if (++*settled < 22 || (!pricesDone() && clock->elapsed() < 30000))
                    return;
                wait->stop();
                finish(false);
            });
            wait->start();
        });
        QTimer::singleShot(argValue(args, QStringLiteral("--timeout")).isEmpty() ? 300000 : timeoutMs,
                           controller, [finish] { finish(true); });
    } else if (!shot.isEmpty()) {
        // --screenshot <file.png>: render the window and quit (documentation /
        // visual checks on a separate --data-dir, checked above; works with
        // QT_QPA_PLATFORM=offscreen).
        QTimer::singleShot(1200, &window, [&window, shot] {
            window.grab().save(shot);
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
