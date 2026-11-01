#pragma once

#include <QWidget>

class QVBoxLayout;
class QStackedWidget;

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
// WalletController; the title bar chip shows its status line.
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
    void buildUi();
    void refreshFrame();
    void openImportDialog();
    void loadWallet();

    WalletController* controller_ = nullptr;
    QVBoxLayout* root_ = nullptr;
    TitleBar* title_ = nullptr;
    SideNav* nav_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    OverviewPage* overview_ = nullptr;
    AddressesPage* addresses_ = nullptr;
    SettingsPage* settings_ = nullptr;
    bool loadAttempted_ = false;
};

} // namespace app::ui
