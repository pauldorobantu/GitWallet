#pragma once

#include "ui/PlaceBar.h"
#include "wallet/Payments.h"

#include <QElapsedTimer>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QStringList>
#include <QWidget>

class QDialog;
class QLabel;
class QPushButton;
class QSystemTrayIcon;
class QTimer;
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
class PlaceBar;
class PortfolioPage;
class HoldingsPage;
class WalletsPage;
class AddressesPage;
class HistoryPage;
class ReceivePage;
class SettingsPage;
class LockPage;

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

    // "portfolio" | "wallets" | "addresses" | "history" | "receive" | "send" | "settings" (for --page and tests).
    void showPage(const QString& name);
    // A step down from a total: where it goes, and which tab of it.
    void goTo(Place place, Place::Tab tab);
    PlaceBar* placeBar() const { return placeBar_; }
    TitleBar* titleBar() const { return title_; }
    void openPayments(); // the list of what came in
    QString lastPaymentAlert() const { return lastPaymentAlert_; } // for tests
    QString lastPaymentNotification() const { return lastPaymentNotification_; } // for tests
    bool trayShown() const;                                        // for a check on the real platform
    void sayPaymentForTest(const QString& text);
    void unlockForTest() { showUnlocked(); }

    // Covers everything with the page that asks for the password. The scans go on behind it, and
    // nothing shows of them. With no password there is nothing to lock with.
    void lockNow();
    bool isLocked() const { return locked_; }
    LockPage* lockPage() const { return lockPage_; }
    void setIdleLimitForTest(int ms);

signals:
    void unlocked();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
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
        QMap<QString, QStringList> servers;
    };

    // One reported problem. All of it is GitWallet's own words but `detail`,
    // which only the tooltip shows (see classify()).
    struct Notice
    {
        QString key;      // the kind of problem, plus its sentence where one kind covers several
        QString headline; // a few words: the toast
        QString text;     // the sentence in the notice bar
        QString detail;   // what the server wrote, if anything; plain text in the tooltip only
        QString file;     // the file a notice is about: it goes when that file does
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
    void openPage(int pageId); // highlights the sidebar item and shows its page, whole
    void leavePlace();         // the pages are no longer narrowed to one
    void openRoot(Place::Root root);
    void editAddressLabel(const QString& address);
    void swallowSecondClick(); // a click just opened something: a second one so soon was not for that
    void afterWalletChange();  // what the reader was on may be gone
    void openImportDialog(bool many); // `many`: on the list of seeds rather than one seed
    void loadWallet();
    void refreshStatus(); // statusChanged / walletChanged
    // errorOccurred
    void onWalletError(wallet::Problem kind, const QString& message, const QString& serverDetail);
    bool pruneNotices(); // true when one was dropped
    void renderNotices();
    void dismissNotices();
    void flushToast();
    void onPaymentsTold(const QList<wallet::Payment>& payments);
    void onPaymentsSettled(const wallet::PaymentNews& news);
    void sayNews(const wallet::PaymentNews& news);
    // On the spot, or as a Windows notification: which, under a password, stays on Windows' side of
    // any lock, so it says only `bare`.
    void sayPayment(const QString& text, const QString& bare, int feedbackType);
    void refreshBell();
    void showLocked();
    void showUnlocked();
    void showIconOnly(bool iconOnly);
    void tryUnlock(const QString& password);
    void forgotPassword();
    void afterPasswordChange();
    void checkIdle();
    Inputs currentInputs() const;

    WalletController* controller_ = nullptr;
    QVBoxLayout* root_ = nullptr;
    TitleBar* title_ = nullptr;
    SideNav* nav_ = nullptr;
    QWidget* bodyHost_ = nullptr; // the sidebar and the pages, all that the lock covers
    LockPage* lockPage_ = nullptr;
    bool locked_ = false;
    QTimer* idleCheck_ = nullptr;
    QElapsedTimer idleClock_;     // since the last key or mouse move anywhere in the app
    int idleLimitForTestMs_ = 0;
    QStackedWidget* stack_ = nullptr;
    PlaceBar* placeBar_ = nullptr;
    PortfolioPage* portfolio_ = nullptr;
    HoldingsPage* holdings_ = nullptr;
    WalletsPage* wallets_ = nullptr;
    AddressesPage* addresses_ = nullptr;
    HistoryPage* history_ = nullptr;
    ReceivePage* receive_ = nullptr;
    SettingsPage* settings_ = nullptr;
    QWidget* noticeBar_ = nullptr; // shown while a notice waits to be read
    QLabel* noticeChip_ = nullptr;
    QLabel* noticeText_ = nullptr;
    QPushButton* noticeSettings_ = nullptr;
    QList<Notice> notices_;  // at most kMaxNotices (MainWindow.cpp)
    QStringList toastQueue_; // keys of the notices the next toast is about
    bool toastPosted_ = false;
    bool loadAttempted_ = false;
    QTimer* clickGuard_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr; // made at the first alert with the window out of sight
    QString lastPaymentAlert_;
    QString lastPaymentNotification_; // what went to Windows, for tests
    wallet::PaymentNews heldNews_; // what became of payments while locked: told at the unlock
    QList<QPointer<QDialog>> heldDialogs_; // what could not close for the lock: back after it
};

} // namespace app::ui
