#include "chain/SelfTest.h"
#include "ui/LanguageManager.h"
#include "ui/MainWindow.h"
#include "ui/ThemeManager.h"

#include <QApplication>
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
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("GitWallet"));
    QApplication::setOrganizationName(QStringLiteral("GitWallet"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    // Prove the derivation engine against published vectors before anything
    // trusts an address it produces.
    const chain::SelfTestResult st = chain::runSelfTest();

    if (app.arguments().contains(QStringLiteral("--selftest"))) {
        attachParentConsole();
        std::fputs(st.report.c_str(), stdout);
        std::fprintf(stdout, "GitWallet self-test: %d/%d passed\n", st.passed, st.total);
        std::fflush(stdout);
        return st.ok ? 0 : 1;
    }

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

    app::ui::MainWindow window;
    window.show();

    // --screenshot <file.png>: render the window to an image and quit
    // (documentation / visual checks; works with QT_QPA_PLATFORM=offscreen).
    const QStringList args = app.arguments();
    const qsizetype shot = args.indexOf(QStringLiteral("--screenshot"));
    if (shot >= 0 && shot + 1 < args.size()) {
        const QString path = args.at(shot + 1);
        QTimer::singleShot(1200, &window, [&window, path] {
            window.grab().save(path);
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
