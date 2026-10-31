#include "ui/MainWindow.h"

#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/SideNav.h"
#include "ui/ThemeManager.h"
#include "ui/TitleBar.h"
#include "ui/WindowShell.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStackedWidget>
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
    buildUi();
}

void MainWindow::buildUi()
{
    root_ = new QVBoxLayout(this);
    root_->setContentsMargins(0, 0, 0, 0);
    root_->setSpacing(0);

    title_ = new TitleBar(this, this);
    title_->setUtilityButtonsVisible(true);
    title_->setStatus(QStringLiteral("GitWallet"));
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
    auto placeholder = [this](int iconKind, const QString& title, const QString& sub) {
        return page::makeEmptyState(stack_, iconKind, title, sub);
    };
    stack_->addWidget(placeholder(int(icon::Kind::Dashboard), QStringLiteral("Overview"),
                                  QStringLiteral("Total balance across every address, in BTC, ETH and USD.")));
    stack_->addWidget(placeholder(int(icon::Kind::ListRows), QStringLiteral("Addresses"),
                                  QStringLiteral("Every address derived from your seed, with its balance.")));
    stack_->addWidget(placeholder(int(icon::Kind::ExternalLink), QStringLiteral("Send"),
                                  QStringLiteral("Consolidate funds to one address.")));
    stack_->addWidget(placeholder(int(icon::Kind::Settings), QStringLiteral("Settings"),
                                  QStringLiteral("Network, currency and appearance.")));
    body->addWidget(stack_, 1);

    root_->addLayout(body, 1);

    connect(title_, &TitleBar::minimised, this, &QWidget::showMinimized);
    connect(title_, &TitleBar::maximised, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });
    connect(title_, &TitleBar::closed, this, &QWidget::close);
    connect(title_, &TitleBar::menuToggled, this,
            [this] { nav_->setCollapsed(!nav_->isCollapsed()); });
    connect(title_, &TitleBar::languageSelected, this,
            [](const QString& code) { LanguageManager::instance()->setLanguage(code); });
    connect(title_, &TitleBar::settingsRequested, this,
            [this] { nav_->select(PageSettings); });

    connect(nav_, &SideNav::navigate, this,
            [this](int pageId, int) { stack_->setCurrentIndex(pageId); });
    connect(nav_, &SideNav::toggleRequested, this,
            [this] { nav_->setCollapsed(!nav_->isCollapsed()); });

    nav_->select(PageOverview);
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
}

} // namespace app::ui
