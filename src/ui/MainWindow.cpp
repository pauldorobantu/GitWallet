#include "ui/MainWindow.h"

#include "app/WalletController.h"
#include "ui/DialogChrome.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/SideNav.h"
#include "ui/ThemeManager.h"
#include "ui/TitleBar.h"
#include "ui/WindowShell.h"
#include "ui/dialogs/ImportSeedDialog.h"
#include "ui/pages/AddressesPage.h"
#include "ui/pages/OverviewPage.h"
#include "ui/pages/SettingsPage.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace app::ui {

namespace {
// Page ids = the stacked-widget indices.
enum PageId { PageOverview = 0, PageAddresses, PageSend, PageSettings };
}

MainWindow::MainWindow(QWidget* parent) : QWidget(parent)
{
    initShellWindow(this); // frameless, default + minimum size
    setWindowTitle(QStringLiteral("GitWallet"));
    controller_ = new WalletController(this);
    buildUi();

    auto updateStatus = [this] { title_->setStatus(controller_->statusText()); };
    connect(controller_, &WalletController::statusChanged, this, updateStatus);
    connect(controller_, &WalletController::walletChanged, this, updateStatus);
    connect(controller_, &WalletController::errorOccurred, this,
            [](const QString& message) { qWarning().noquote() << "[wallet]" << message; });
    updateStatus();
}

void MainWindow::buildUi()
{
    root_ = new QVBoxLayout(this);
    root_->setContentsMargins(0, 0, 0, 0);
    root_->setSpacing(0);

    title_ = new TitleBar(this, this);
    title_->setUtilityButtonsVisible(true);
    root_->addWidget(title_);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);

    nav_ = new SideNav(this);
    nav_->addSection(QStringLiteral("Wallet"));
    nav_->addItem(PageOverview, QStringLiteral("Overview"), int(icon::Kind::Dashboard));
    nav_->addItem(PageAddresses, QStringLiteral("Addresses"), int(icon::Kind::ListRows));
    nav_->addItem(PageSend, QStringLiteral("Send"), int(icon::Kind::ExternalLink));
    nav_->addItem(PageSettings, QStringLiteral("Settings"), int(icon::Kind::Settings));
    body->addWidget(nav_);

    stack_ = new QStackedWidget(this);
    overview_ = new OverviewPage(controller_, stack_);
    addresses_ = new AddressesPage(controller_, stack_);
    settings_ = new SettingsPage(controller_, stack_);
    stack_->addWidget(overview_);
    stack_->addWidget(addresses_);
    // Spending comes in stage 2, after the view has been verified against
    // other wallets and block explorers.
    stack_->addWidget(page::makeEmptyState(
        stack_, int(icon::Kind::ExternalLink), QStringLiteral("Send"),
        QStringLiteral("Consolidating funds is added in the next stage, once balances are verified.")));
    stack_->addWidget(settings_);
    body->addWidget(stack_, 1);

    root_->addLayout(body, 1);

    connect(overview_, &OverviewPage::importRequested, this, &MainWindow::openImportDialog);

    connect(title_, &TitleBar::minimised, this, &QWidget::showMinimized);
    connect(title_, &TitleBar::maximised, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });
    connect(title_, &TitleBar::closed, this, &QWidget::close);
    connect(title_, &TitleBar::menuToggled, this,
            [this] { nav_->setCollapsed(!nav_->isCollapsed()); });
    connect(title_, &TitleBar::languageSelected, this,
            [](const QString& code) { LanguageManager::instance()->setLanguage(code); });
    connect(title_, &TitleBar::settingsRequested, this, [this] { nav_->select(PageSettings); });

    connect(nav_, &SideNav::navigate, this,
            [this](int pageId, int) { stack_->setCurrentIndex(pageId); });
    connect(nav_, &SideNav::toggleRequested, this,
            [this] { nav_->setCollapsed(!nav_->isCollapsed()); });

    nav_->select(PageOverview);
}

void MainWindow::showPage(const QString& name)
{
    static const QHash<QString, int> ids = {
        {QStringLiteral("overview"), PageOverview},
        {QStringLiteral("addresses"), PageAddresses},
        {QStringLiteral("send"), PageSend},
        {QStringLiteral("settings"), PageSettings},
    };
    const auto it = ids.constFind(name.toLower());
    if (it == ids.cend()) return;
    nav_->select(*it);
    stack_->setCurrentIndex(*it);
}

void MainWindow::openImportDialog()
{
    ImportSeedDialog dialog(controller_, this);
    dialog.exec();
}

void MainWindow::loadWallet()
{
    QString error;
    if (controller_->loadExisting(&error) || error.isEmpty()) return;
    // The file exists but cannot be read (another Windows user, damaged).
    QMessageBox box(QMessageBox::Warning, QStringLiteral("GitWallet"),
                    QStringLiteral("The saved wallet could not be opened.\n\n%1").arg(error),
                    QMessageBox::Ok, this);
    applyDialogChrome(&box);
    box.exec();
}

void MainWindow::refreshFrame()
{
    updateShellFrame(this, root_);
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    if (shellResizeHitTest(this, eventType, message, result)) return true;
    return QWidget::nativeEvent(eventType, message, result);
}

void MainWindow::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) refreshFrame();
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    refreshFrame();
}

void MainWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refreshFrame();
    // Load after the window is up, so a warning about an unreadable file has a
    // parent on screen and the first paint is not delayed by key parsing.
    if (!loadAttempted_) {
        loadAttempted_ = true;
        QTimer::singleShot(0, this, &MainWindow::loadWallet);
    }
}

} // namespace app::ui
