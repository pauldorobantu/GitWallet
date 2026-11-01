#pragma once

#include <QStringList>
#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;
class QStackedWidget;

namespace wallet {
enum class Problem;
}

namespace app {
class WalletController;
}

namespace app::ui {

class TitleBar;
class SideNav;
class OverviewPage;
class AddressesPage;
class SettingsPage;

// The application window: the frameless shell, title bar and sidebar of the
// reused FlowSuite design system, hosting the wallet pages. Owns the
// WalletController. The title bar chip shows its status line, and a notice bar
// above the pages shows the problems it reports (errorOccurred): the build has
// no console, so a log line alone would reach nobody.
class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    WalletController* controller() const { return controller_; }

    // "overview" | "addresses" | "send" | "settings" (for --page and tests).
    void showPage(const QString& name);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    // The wallet and settings a problem was reported under. Every restart of
    // the scanners (load, import, Settings > Save) reports again whatever still
    // holds, so a notice goes once an input it depends on has changed.
    struct Inputs
    {
        size_t wallet = 0; // fingerprint of the account keys; 0 without a wallet
        int receiveGap = 0;
        int changeGap = 0;
        int ethGap = 0;
        int refreshSeconds = 0;
        QStringList electrumServers;
        QStringList ethEndpoints;
    };

    // One reported problem. All of it is GitWallet's own words but `detail`,
    // which only the tooltip shows (see classify()).
    struct Notice
    {
        QString key;      // the kind of problem, plus its sentence where one kind covers several
        QString headline; // a few words: the toast
        QString text;     // the sentence in the notice bar
        QString detail;   // what the server wrote, if anything; plain text in the tooltip only
        int severity = 0; // kSeverity* (MainWindow.cpp)
        int depends = 0;  // kDepends*: the settings it is about
        bool settingsFix = false; // Settings can fix it: the bar offers to open them
        Inputs inputs;
        qint64 firstMs = 0; // first and latest report, ms since the epoch
        qint64 lastMs = 0;
        int count = 0;
        qint64 toastedMs = -1; // its latest toast, monotonic ms; -1 = none yet
        bool seen = false;     // dismissed, and not toasted again since
    };

    static Notice classify(wallet::Problem kind, const QString& message, const QString& serverDetail);

    void buildUi();
    QWidget* buildNoticeBar();
    void refreshFrame();
    void openPage(int pageId); // highlights the sidebar item and shows its page
    void openImportDialog();
    void loadWallet();
    void refreshStatus(); // statusChanged / walletChanged
    // errorOccurred
    void onWalletError(wallet::Problem kind, const QString& message, const QString& serverDetail);
    bool pruneNotices(); // true when one was dropped
    void renderNotices();
    void dismissNotices();
    void flushToast();
    Inputs currentInputs() const;

    WalletController* controller_ = nullptr;
    QVBoxLayout* root_ = nullptr;
    TitleBar* title_ = nullptr;
    SideNav* nav_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    OverviewPage* overview_ = nullptr;
    AddressesPage* addresses_ = nullptr;
    SettingsPage* settings_ = nullptr;
    QWidget* noticeBar_ = nullptr; // shown while a notice waits to be read
    QLabel* noticeChip_ = nullptr;
    QLabel* noticeText_ = nullptr;
    QPushButton* noticeSettings_ = nullptr;
    QList<Notice> notices_;  // at most kMaxNotices (MainWindow.cpp)
    QStringList toastQueue_; // keys of the notices the next toast is about
    bool toastPosted_ = false;
    bool loadAttempted_ = false;
};

} // namespace app::ui
