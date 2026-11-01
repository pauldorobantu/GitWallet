#include "app/WalletController.h"
#include "app/Walkthrough.h"
#include "chain/Bip39.h"
#include "chain/SelfTest.h"
#include "net/PriceFeed.h"
#include "ui/LanguageManager.h"
#include "ui/MainWindow.h"
#include "ui/ThemeManager.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QString>
#include <QTimer>

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
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

// The public BIP39 test mnemonic (published in BIP84 and every wallet test
// suite). Only ever imported into a throw-away --data-dir for tests.
const char* kPublicTestMnemonic =
    "abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon abandon abandon about";

// --scan-report: every row and the totals as JSON, for comparing the scan with
// block explorers and other wallets.
bool writeScanReport(const QString& path, app::WalletController* c, bool timedOut)
{
    const wallet::WalletModel* m = c->model();
    QJsonArray rows;
    for (int i = 0; i < m->size(); ++i) {
        const wallet::AddressRow& r = m->at(i);
        QJsonObject o{
            {QStringLiteral("path"), r.path},
            {QStringLiteral("address"), r.address},
            {QStringLiteral("checked"), r.checked},
            {QStringLiteral("used"), r.used},
        };
        if (r.isBitcoin()) {
            o.insert(QStringLiteral("btcConfirmed"), QString::number(r.btcConfirmed));
            o.insert(QStringLiteral("btcUnconfirmed"), QString::number(r.btcUnconfirmed));
        } else {
            o.insert(QStringLiteral("nonce"), QString::number(r.nonce));
            o.insert(QStringLiteral("ethWei"), QString::fromStdString(r.ethWei.toDecimalString()));
            for (const wallet::AssetInfo& a : wallet::erc20Assets())
                o.insert(a.symbol, QString::fromStdString(r.tokens.value(a.asset).toDecimalString()));
        }
        rows.append(o);
    }
    QJsonObject totals;
    for (const wallet::AssetInfo& a : wallet::assets())
        totals.insert(a.symbol, QString::fromStdString(m->total(a.asset).format(a.decimals, a.decimals)));
    QJsonObject prices;
    for (const wallet::AssetInfo& a : wallet::assets())
        prices.insert(a.symbol, c->prices()->usd(a.symbol));
    QJsonArray accounts; // public keys only — lets a test compare with Electrum watch-only
    for (const wallet::AccountKey& k : c->store().accounts)
        accounts.append(QJsonObject{{QStringLiteral("type"), QString::fromLatin1(chain::scriptTypeName(k.type))},
                                    {QStringLiteral("account"), int(k.account)},
                                    {QStringLiteral("xpub"), k.xpub}});
    const QJsonObject report{
        {QStringLiteral("accounts"), accounts},
        {QStringLiteral("timedOut"), timedOut},
        {QStringLiteral("electrumServer"), c->electrumServer()},
        {QStringLiteral("ethEndpoint"), c->ethEndpoint()},
        {QStringLiteral("status"), c->statusText()},
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
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    const QStringList args = app.arguments();

    // Prove the derivation engine against published vectors before anything
    // trusts an address it produces.
    const chain::SelfTestResult st = chain::runSelfTest();

    if (args.contains(QStringLiteral("--selftest"))) {
        attachParentConsole();
        std::fputs(st.report.c_str(), stdout);
        std::fprintf(stdout, "GitWallet self-test: %d/%d passed\n", st.passed, st.total);
        std::fflush(stdout);
        return st.ok ? 0 : 1;
    }

    // A separate data folder (tests, screenshots, a second wallet).
    const QString dataDir = argValue(args, QStringLiteral("--data-dir"));
    if (!dataDir.isEmpty()) wallet::WalletStore::setDataDir(dataDir);

    app::ui::theme::ThemeManager::instance()->loadFromSettings();
    app::ui::theme::ThemeManager::instance()->apply();
    app::ui::LanguageManager::instance()->loadFromSettings();

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
    // scan, pages, settings, reopen, remove) with a screenshot per step. Only
    // with --data-dir, so it can never touch the real wallet.
    const QString walkthroughDir = argValue(args, QStringLiteral("--walkthrough"));
    if (!walkthroughDir.isEmpty()) {
        if (dataDir.isEmpty()) {
            attachParentConsole();
            std::fputs("--walkthrough needs --data-dir <folder>\n", stderr);
            return 2;
        }
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

    app::ui::MainWindow window;
    app::WalletController* controller = window.controller();
    window.show(); // the saved wallet loads on the first event-loop pass

    // --import-test-mnemonic: the PUBLIC test seed into a throw-away --data-dir.
    // Refused without --data-dir so it can never touch the real wallet.
    if (args.contains(QStringLiteral("--import-test-mnemonic"))) {
        if (dataDir.isEmpty()) {
            attachParentConsole();
            std::fputs("--import-test-mnemonic needs --data-dir <folder>\n", stderr);
            return 2;
        }
        QTimer::singleShot(0, controller, [controller] {
            if (controller->hasWallet()) return;
            QString error;
            if (!controller->importMnemonic(chain::bip39::split(kPublicTestMnemonic), {}, &error))
                qWarning().noquote() << "[test] import failed:" << error;
        });
    }

    const QString page = argValue(args, QStringLiteral("--page"));
    if (!page.isEmpty()) window.showPage(page);

    const QString shot = argValue(args, QStringLiteral("--screenshot"));
    const QString reportPath = argValue(args, QStringLiteral("--scan-report"));

    if (!reportPath.isEmpty()) {
        // Wait for both chains' initial scan (or the timeout), then write the
        // report (and the screenshot, if asked) and quit.
        const int timeoutMs = qBound(10, argValue(args, QStringLiteral("--timeout")).toInt(), 3600) * 1000;
        auto finish = [&window, controller, reportPath, shot](bool timedOut) {
            static bool done = false;
            if (done) return;
            done = true;
            if (!shot.isEmpty()) window.grab().save(shot);
            const bool ok = writeScanReport(reportPath, controller, timedOut);
            QCoreApplication::exit(ok ? (timedOut ? 3 : 0) : 4);
        };
        QObject::connect(controller, &app::WalletController::scanFinished, controller,
                         [finish] { QTimer::singleShot(1500, [finish] { finish(false); }); });
        QTimer::singleShot(argValue(args, QStringLiteral("--timeout")).isEmpty() ? 300000 : timeoutMs,
                           controller, [finish] { finish(true); });
    } else if (!shot.isEmpty()) {
        // --screenshot <file.png>: render the window and quit
        // (documentation / visual checks; works with QT_QPA_PLATFORM=offscreen).
        QTimer::singleShot(1200, &window, [&window, shot] {
            window.grab().save(shot);
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
