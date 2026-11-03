#include "ui/MainWindow.h"

#include "app/WalletController.h"
#include "ui/DialogChrome.h"
#include "ui/Feedback.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/Holdings.h"
#include "ui/PageKit.h"
#include "ui/SideNav.h"
#include "ui/ThemeManager.h"
#include "ui/TitleBar.h"
#include "ui/WindowShell.h"
#include "ui/design/Style.h"
#include "ui/Figures.h"
#include "ui/dialogs/ImportSeedDialog.h"
#include "ui/dialogs/PaymentsDialog.h"
#include "ui/dialogs/StatusDialog.h"
#include "ui/TableUtil.h"
#include "ui/pages/WalletsPage.h"
#include "ui/pages/AddressesPage.h"
#include "ui/pages/HistoryPage.h"
#include "ui/pages/HoldingsPage.h"
#include "ui/pages/LockPage.h"
#include "ui/pages/PortfolioPage.h"
#include "ui/pages/ReceivePage.h"
#include "ui/pages/SettingsPage.h"
#include "wallet/History.h"
#include "wallet/Payments.h"
#include "wallet/Types.h"
#include "wallet/UtxoHistory.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QApplication>
#include "core/SettingsRepo.h"
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace app::ui {

namespace {

// Page ids = the stacked-widget indices.
enum PageId { PagePortfolio = 0, PageWallets, PageAddresses, PageHistory, PageReceive, PageSend, PageSettings, PageHoldings };

// What a notice depends on besides the wallet (Notice::depends). It goes when
// one of them changes: the scanners then restart and report again whatever
// still holds.
constexpr int kDependsBtcGaps = 1;     // the receive and change gaps
constexpr int kDependsEthGap = 2;
constexpr int kDependsServers = 4;     // a network's server list
constexpr int kDependsAnySetting = 16; // any scan setting: a failure, not a state

// Notice::severity.
constexpr int kSeverityInfo = 0;
constexpr int kSeverityWarning = 1;
constexpr int kSeverityIncomplete = 2; // a balance is missing from the totals

// One notice per problem (a repeat only counts up), so a handful is normal; the
// bound keeps a session that runs into many different ones within memory.
constexpr qsizetype kMaxNotices = 24;
constexpr int kNoticeLines = 3; // in the bar; its tooltip lists them all
// The same notice toasts again only after this long, and a dismissed one comes
// back no sooner; the bar counts the repeats meanwhile. Public endpoints
// rate-limit routinely, and a session can run for days.
constexpr qint64 kToastRepeatMs = 10 * 60 * 1000;
// Longer texts are cut before they are shown or logged (the scanners keep a
// server's words to 200 characters already).
constexpr qsizetype kMaxMessageChars = 1000;
// The notices' tooltip is plain text, which Qt does not wrap: its lines are
// broken at about this many characters.
constexpr qsizetype kTipLineChars = 100;

// Toast throttling must not follow wall-clock jumps (time zone, NTP).
qint64 monotonicMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

QString grouped(qint64 n)
{
    // English-first, like the controller's status line: "12,345".
    static const QLocale english(QLocale::English, QLocale::UnitedStates);
    return english.toString(n);
}

// With the date: a session runs for days, and "03:12" alone would not say which.
QString noticeTime(qint64 msSinceEpoch)
{
    static const QLocale english(QLocale::English, QLocale::UnitedStates);
    return english.toString(QDateTime::fromMSecsSinceEpoch(msSinceEpoch),
                            QStringLiteral("d MMM HH:mm"));
}

// A text as it came, a server's words included, but on one line, without
// control or bidi characters, and without the path, query or user info of any
// URL in it: a configured endpoint may carry an API key there. For the
// developer trail (qWarning), and for a server's words in the tooltip.
QString cleanLine(const QString& message)
{
    static const QRegularExpression url(
        QStringLiteral("\\b([A-Za-z][A-Za-z0-9+.-]*://)(?:[^\\s/?#@]*@)?([^\\s/?#]*)\\S*"));
    const QString cut = message.left(kMaxMessageChars);
    QString line;
    line.reserve(cut.size());
    for (const QChar c : cut) {
        if (c.isSpace())
            line.append(QLatin1Char(' '));
        else if (c.category() != QChar::Other_Control && c.category() != QChar::Other_Format)
            line.append(c);
    }
    line = line.simplified();
    line.replace(url, QStringLiteral("\\1\\2"));
    return line;
}

// The reason in a server detail that the notice sentence may name. The detail
// holds what an endpoint or server wrote, and a hostile one can write anything
// there ("your funds are at risk, visit ..."): only GitWallet's own fixed
// texts, and numbers taken out of the rest, come out. Empty: nothing to name.
QString safeDetail(const QString& detail)
{
    static const QStringList fixed = {
        QStringLiteral("timed out"),
        QStringLiteral("no reply"),
        QStringLiteral("empty reply"),
        QStringLiteral("unexpected reply"),
        QStringLiteral("malformed JSON reply"),
        QStringLiteral("reply too large"),
        QStringLiteral("no HTTP status"),
        QStringLiteral("reply is not a JSON array"),
        QStringLiteral("reply has neither result nor error"),
        QStringLiteral("no valid chain id in the reply"),
        QStringLiteral("the Multicall3 request could not be encoded"),
        QStringLiteral("unexpected Multicall3 reply"),
    };
    static const QRegularExpression http(QStringLiteral("^HTTP ([0-9]{3})\\b"));
    static const QRegularExpression rpcCode(QStringLiteral("\\(code (-?[0-9]{1,9})\\)$"));
    static const QRegularExpression hostNotFound(QStringLiteral("^Host \\S{1,253} not found$"));
    static const QRegularExpression deadline(
        QStringLiteral("^no complete reply within ([0-9]{1,4}) s$"));
    static const QRegularExpression otherNetwork(QStringLiteral("^endpoint is not on ([A-Za-z0-9 ]{1,40}) \\("));

    const QString d = detail.trimmed();
    if (fixed.contains(d))
        return d;
    if (const QRegularExpressionMatch m = http.match(d); m.hasMatch())
        return QStringLiteral("HTTP ") + m.captured(1);
    if (const QRegularExpressionMatch m = rpcCode.match(d); m.hasMatch())
        return QStringLiteral("JSON-RPC error ") + m.captured(1);
    if (const QRegularExpressionMatch m = deadline.match(d); m.hasMatch())
        return QStringLiteral("no complete reply within %1 s").arg(m.captured(1));
    if (d.startsWith(QLatin1String("reply is not a JSON array")))
        return QStringLiteral("reply is not a JSON array");
    if (const QRegularExpressionMatch m = otherNetwork.match(d); m.hasMatch()) {
        // Only a name from the list: this is a server's text, whatever it looks like.
        const QList<wallet::NetworkInfo>& known = wallet::networks();
        if (std::any_of(known.cbegin(), known.cend(), [&m](const wallet::NetworkInfo& n) { return n.name == m.captured(1); }))
            return QStringLiteral("endpoint not on %1").arg(m.captured(1));
    }
    if (hostNotFound.match(d).hasMatch())
        return QStringLiteral("host not found");
    if (d.startsWith(QLatin1String("Connection refused")))
        return QStringLiteral("connection refused");
    if (d.startsWith(QLatin1String("SSL handshake failed")))
        return QStringLiteral("TLS handshake failed");
    return QString();
}

// GitWallet's own message as one sentence, with a reason in brackets before
// its full stop: "Ethereum balance lookup failed (HTTP 429)."
QString sentence(QString text, const QString& reason)
{
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    if (!reason.isEmpty())
        text += QStringLiteral(" (") + reason + QLatin1Char(')');
    return text.isEmpty() ? text : text + QLatin1Char('.');
}

// `text` broken at spaces into lines of about kTipLineChars characters, each
// after `indent`. A longer word keeps a line to itself.
QString wrapped(const QString& text, const QString& indent)
{
    QStringList lines;
    QString line;
    for (const QString& word : text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (!line.isEmpty() && line.size() + 1 + word.size() > kTipLineChars) {
            lines.append(indent + line);
            line.clear();
        }
        if (!line.isEmpty())
            line += QLatin1Char(' ');
        line += word;
    }
    if (!line.isEmpty())
        lines.append(indent + line);
    return lines.join(QLatin1Char('\n'));
}

// Anything done with the keyboard or the mouse, anywhere in the app, puts off locking it.
class ActivityWatch : public QObject
{
public:
    ActivityWatch(QElapsedTimer* clock, QObject* parent) : QObject(parent), clock_(clock) {}

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::KeyPress:
        case QEvent::Wheel:
            clock_->restart();
            break;
        default:
            break;
        }
        return false;
    }

private:
    QElapsedTimer* clock_ = nullptr;
};

// How often it looks whether the time has come.
constexpr int kIdleCheckMs = 5000;

Feedback::Type toastType(int severity)
{
    if (severity >= kSeverityIncomplete)
        return Feedback::Type::Error;
    return severity == kSeverityWarning ? Feedback::Type::Warning : Feedback::Type::Info;
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QWidget(parent)
{
    initShellWindow(this); // frameless, default + minimum size
    setWindowTitle(QStringLiteral("GitWallet"));
    controller_ = new WalletController(this);
    buildUi();

    connect(controller_, &WalletController::statusChanged, this, &MainWindow::refreshStatus);
    connect(controller_, &WalletController::walletChanged, this, &MainWindow::refreshStatus);
    // A WIN32 executable without a message handler: qWarning() alone reaches
    // only a debugger, and these messages are what explains a balance that is
    // missing or stale. They go on screen.
    connect(controller_, &WalletController::errorOccurred, this, &MainWindow::onWalletError);
    connect(controller_->payments(), &wallet::PaymentBook::told, this, &MainWindow::onPaymentsTold);
    connect(controller_->payments(), &wallet::PaymentBook::settled, this, &MainWindow::onPaymentsSettled);
    connect(controller_->payments(), &wallet::PaymentBook::changed, this, &MainWindow::refreshBell);
    connect(controller_, &WalletController::passwordChanged, this, &MainWindow::afterPasswordChange);
    connect(controller_, &WalletController::walletChanged, this, &MainWindow::afterPasswordChange);
    idleClock_.start();
    qApp->installEventFilter(new ActivityWatch(&idleClock_, this));
    idleCheck_ = new QTimer(this);
    idleCheck_->setInterval(kIdleCheckMs);
    connect(idleCheck_, &QTimer::timeout, this, &MainWindow::checkIdle);
    refreshStatus();
    refreshBell();
}

void MainWindow::buildUi()
{
    root_ = new QVBoxLayout(this);
    root_->setContentsMargins(0, 0, 0, 0);
    root_->setSpacing(0);

    title_ = new TitleBar(this, this);
    title_->setUtilityButtonsVisible(true);
    root_->addWidget(title_);

    bodyHost_ = new QWidget(this);
    auto* body = new QHBoxLayout(bodyHost_);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);

    nav_ = new SideNav(bodyHost_);
    nav_->addSection(QStringLiteral("Wallet"));
    nav_->addItem(PagePortfolio, QStringLiteral("Portfolio"), int(icon::Kind::Dashboard));
    nav_->addItem(PageWallets, QStringLiteral("Wallets"), int(icon::Kind::Key));
    nav_->addItem(PageAddresses, QStringLiteral("Addresses"), int(icon::Kind::ListRows));
    nav_->addItem(PageHistory, QStringLiteral("History"), int(icon::Kind::Traffic));
    nav_->addItem(PageReceive, QStringLiteral("Receive"), int(icon::Kind::ImportDown));
    nav_->addItem(PageSend, QStringLiteral("Send"), int(icon::Kind::ExternalLink));
    nav_->addItem(PageSettings, QStringLiteral("Settings"), int(icon::Kind::Settings));
    body->addWidget(nav_);

    stack_ = new QStackedWidget(this);
    portfolio_ = new PortfolioPage(controller_, stack_);
    wallets_ = new WalletsPage(controller_, stack_);
    addresses_ = new AddressesPage(controller_, stack_);
    history_ = new HistoryPage(controller_, stack_);
    receive_ = new ReceivePage(controller_, stack_);
    settings_ = new SettingsPage(controller_, stack_);
    holdings_ = new HoldingsPage(controller_, stack_);
    stack_->addWidget(portfolio_);
    stack_->addWidget(wallets_);
    stack_->addWidget(addresses_);
    stack_->addWidget(history_);
    stack_->addWidget(receive_);
    // Spending comes once the history has been verified against other wallets and block explorers.
    stack_->addWidget(page::makeEmptyState(
        stack_, int(icon::Kind::ExternalLink), QStringLiteral("Send"),
        QStringLiteral("GitWallet only watches: it keeps public keys and no seed, so it cannot spend. To move coins, use the wallet the seed came from.")));
    stack_->addWidget(settings_);
    stack_->addWidget(holdings_);

    // The notice bar sits above whichever page is open, and where the reader is under it.
    auto* column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(buildNoticeBar());
    placeBar_ = new PlaceBar(controller_, this);
    column->addWidget(placeBar_);
    column->addWidget(stack_, 1);
    body->addLayout(column, 1);

    root_->addWidget(bodyHost_, 1);
    lockPage_ = new LockPage(this);
    lockPage_->hide();
    root_->addWidget(lockPage_, 1);
    connect(lockPage_, &LockPage::unlockRequested, this, &MainWindow::tryUnlock);
    connect(lockPage_, &LockPage::forgotRequested, this, &MainWindow::forgotPassword);
    connect(title_, &TitleBar::lockRequested, this, &MainWindow::lockNow);
    connect(settings_, &SettingsPage::lockRequested, this, &MainWindow::lockNow);

    connect(portfolio_, &PortfolioPage::importRequested, this, [this] { openImportDialog(false); });
    connect(portfolio_, &PortfolioPage::importManyRequested, this, [this] { openImportDialog(true); });
    clickGuard_ = new QTimer(this);
    clickGuard_->setSingleShot(true);
    connect(clickGuard_, &QTimer::timeout, this, [this] { qApp->removeEventFilter(this); });
    connect(portfolio_, &PortfolioPage::coinChosen, this, [this](wallet::Asset asset) {
        swallowSecondClick();
        Place place;
        place.root = Place::Root::Portfolio;
        place.asset = asset;
        goTo(place, Place::Tab::Holdings);
    });
    connect(portfolio_, &PortfolioPage::receiveRequested, this, [this] { openPage(PageReceive); });
    connect(portfolio_, &PortfolioPage::paymentsRequested, this, &MainWindow::openPayments);
    connect(portfolio_, &PortfolioPage::paymentChosen, this, [this](const wallet::Payment& payment) {
        swallowSecondClick();
        openPaymentAddress(payment);
    });
    connect(holdings_->table(), &HoldingsTable::coinChosen, this, [this](wallet::Asset asset) {
        swallowSecondClick();
        Place place = placeBar_->place();
        place.asset = asset;
        goTo(place, Place::Tab::Addresses);
    });
    connect(holdings_->table(), &HoldingsTable::walletChosen, this, [this](quint32 wallet) {
        swallowSecondClick();
        Place place = placeBar_->place();
        place.wallet = wallet;
        goTo(place, Place::Tab::Addresses);
    });
    connect(placeBar_, &PlaceBar::placeChosen, this, [this](const Place& place) { goTo(place, place.firstTab()); });
    connect(placeBar_, &PlaceBar::tabChosen, this, [this](Place::Tab tab) { goTo(placeBar_->place(), tab); });
    connect(placeBar_, &PlaceBar::rootChosen, this, &MainWindow::openRoot);
    connect(placeBar_, &PlaceBar::receiveRequested, this, [this](const Place& place) {
        openPage(PageReceive);
        receive_->showFor(place.wallet, place.asset);
    });
    connect(placeBar_, &PlaceBar::showAddressQr, this, [this](const Place& place) {
        if (!place.address)
            return;
        openPage(PageReceive);
        // As it stands now, not as it stood when it was opened.
        const wallet::WalletModel* model = controller_->model();
        const wallet::AddressRow& opened = *place.address;
        const int position = model->find(opened.wallet, opened.network, opened.type, opened.account, opened.change, opened.index);
        receive_->showAddress(position >= 0 ? model->at(position) : opened);
    });
    connect(placeBar_, &PlaceBar::labelRequested, this, &MainWindow::editAddressLabel);
    connect(receive_, &ReceivePage::openAddressRequested, this, [this](const wallet::AddressRow& row) {
        Place place;
        place.root = Place::Root::Addresses;
        place.address = row;
        goTo(place, Place::Tab::Holdings);
    });
    connect(wallets_, &WalletsPage::addRequested, this, [this] { openImportDialog(false); });
    connect(wallets_, &WalletsPage::addManyRequested, this, [this] { openImportDialog(true); });
    const auto walletPlace = [](quint32 wallet) {
        Place place;
        place.root = Place::Root::Wallets;
        place.wallet = wallet;
        return place;
    };
    connect(wallets_, &WalletsPage::openRequested, this, [this, walletPlace](quint32 wallet) { goTo(walletPlace(wallet), Place::Tab::Holdings); });
    connect(wallets_, &WalletsPage::showAddressesRequested, this, [this, walletPlace](quint32 wallet) { goTo(walletPlace(wallet), Place::Tab::Addresses); });
    connect(wallets_, &WalletsPage::showHistoryRequested, this, [this, walletPlace](quint32 wallet) { goTo(walletPlace(wallet), Place::Tab::History); });
    const auto addressPlace = [this](const wallet::AddressRow& row) {
        // Down from where the reader is, if that is on the way to it.
        Place place = placeBar_->isVisibleTo(this) ? placeBar_->place() : Place();
        if (!placeBar_->isVisibleTo(this))
            place.root = Place::Root::Addresses;
        place.address = row;
        return place;
    };
    connect(addresses_, &AddressesPage::openAddressRequested, this, [this, addressPlace](const wallet::AddressRow& row) { goTo(addressPlace(row), Place::Tab::Holdings); });
    connect(addresses_, &AddressesPage::showHistoryRequested, this, [this, addressPlace](const wallet::AddressRow& row) { goTo(addressPlace(row), Place::Tab::History); });
    connect(controller_, &WalletController::walletChanged, this, &MainWindow::afterWalletChange);

    connect(title_, &TitleBar::minimised, this, &QWidget::showMinimized);
    connect(title_, &TitleBar::maximised, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });
    connect(title_, &TitleBar::closed, this, &QWidget::close);
    connect(title_, &TitleBar::languageSelected, this,
            [](const QString& code) { LanguageManager::instance()->setLanguage(code); });
    connect(title_, &TitleBar::paymentsRequested, this, &MainWindow::openPayments);
    connect(title_, &TitleBar::statusRequested, this, &MainWindow::openStatus);
    connect(title_, &TitleBar::refreshRequested, this, &MainWindow::refreshNow);
    refreshCooldown_ = new QTimer(this);
    refreshCooldown_->setSingleShot(true);
    refreshCooldown_->setInterval(3000);
    connect(refreshCooldown_, &QTimer::timeout, this, [this] { title_->setRefreshEnabled(true); });

    connect(nav_, &SideNav::navigate, this, [this](int pageId, int) { openPage(pageId); });
    const auto foldNav = [this] {
        navByHand_ = !nav_->isCollapsed();
        nav_->setCollapsed(navByHand_);
        theme::ThemeManager* themes = theme::ThemeManager::instance();
        theme::Appearance look = themes->appearance();
        look.navCollapsed = navByHand_;
        themes->updateAppearance(look);
    };
    connect(nav_, &SideNav::toggleRequested, this, foldNav);
    navByHand_ = theme::ThemeManager::instance()->appearance().navCollapsed;
    nav_->setCollapsed(navByHand_, false);

    // What the sidebar's and the title bar's tooltips promise.
    const auto shortcut = [this](const QKeySequence& keys, const std::function<void()>& action) {
        connect(new QShortcut(keys, this), &QShortcut::activated, this, [this, action] {
            if (!locked_)
                action();
        });
    };
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_M), foldNav);
    shortcut(QKeySequence(Qt::Key_F5), [this] { refreshNow(); });
    shortcut(QKeySequence::Find, [this] {
        if (QWidget* page = stack_->currentWidget()) {
            const QList<QLineEdit*> searches = page->findChildren<QLineEdit*>(QStringLiteral("mimSearch"));
            for (QLineEdit* search : searches) {
                if (search->isVisible()) {
                    search->setFocus();
                    search->selectAll();
                    return;
                }
            }
        }
    });

    nav_->select(PagePortfolio);
}

QWidget* MainWindow::buildNoticeBar()
{
    // Inside the pages' side margins, so it reads as part of whichever page is
    // open; their own top margin keeps it apart from the content.
    noticeBar_ = new QWidget(this);
    auto* margins = new QVBoxLayout(noticeBar_);
    margins->setContentsMargins(page::kPageMarginX, page::kPageMarginTop, page::kPageMarginX, 0);
    margins->setSpacing(0);

    auto* card = new QFrame(noticeBar_);
    card->setObjectName(QStringLiteral("cardPanel")); // the kit's card
    auto* row = new QHBoxLayout(card);
    row->setContentsMargins(page::kSectionPadX, page::kFormVSpacing, page::kSectionPadX,
                            page::kFormVSpacing);
    row->setSpacing(page::kFormHSpacing);

    noticeChip_ = new QLabel(card);
    noticeChip_->setTextFormat(Qt::PlainText);
    noticeChip_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed); // a chip hugs its text
    row->addWidget(noticeChip_, 0, Qt::AlignTop);

    noticeText_ = new QLabel(card);
    noticeText_->setTextFormat(Qt::PlainText); // never markup or links, whatever the text
    noticeText_->setWordWrap(true);
    noticeText_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(noticeText_, 1);

    noticeSettings_ = new QPushButton(QStringLiteral("Open Settings"), card);
    connect(noticeSettings_, &QPushButton::clicked, this, [this] { openPage(PageSettings); });
    row->addWidget(noticeSettings_, 0, Qt::AlignTop);

    auto* dismiss = new QPushButton(QStringLiteral("Dismiss"), card);
    dismiss->setToolTip(
        QStringLiteral("Hide these notices. The title bar keeps saying when a balance is missing."));
    connect(dismiss, &QPushButton::clicked, this, &MainWindow::dismissNotices);
    row->addWidget(dismiss, 0, Qt::AlignTop);

    margins->addWidget(card);
    noticeBar_->hide();
    return noticeBar_;
}

void MainWindow::showPage(const QString& name)
{
    static const QHash<QString, int> ids = {
        {QStringLiteral("portfolio"), PagePortfolio},
        {QStringLiteral("overview"), PagePortfolio},
        {QStringLiteral("wallets"), PageWallets},
        {QStringLiteral("addresses"), PageAddresses},
        {QStringLiteral("history"), PageHistory},
        {QStringLiteral("receive"), PageReceive},
        {QStringLiteral("send"), PageSend},
        {QStringLiteral("settings"), PageSettings},
    };
    const auto it = ids.constFind(name.toLower());
    if (it == ids.cend()) return;
    openPage(*it);
}

void MainWindow::openPage(int pageId)
{
    if (stack_->currentWidget() == receive_)
        receive_->leaving();
    // SideNav::select() only moves the highlight; the page is switched here. The place goes after it: a
    // page that is no longer on screen has no reason to go over its rows for it now.
    nav_->select(pageId);
    stack_->setCurrentIndex(pageId);
    leavePlace();
    if (pageId == PageReceive)
        receive_->showFor(0, std::nullopt); // what is next, rather than the one last looked at
}

void MainWindow::swallowSecondClick()
{
    clickGuard_->start(QApplication::doubleClickInterval());
    qApp->installEventFilter(this); // for that long only; again is harmless
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
        if (clickGuard_->isActive()) {
            if (const auto* widget = qobject_cast<QWidget*>(watched); widget && stack_->isAncestorOf(widget))
                return true;
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void MainWindow::leavePlace()
{
    if (!placeBar_->isVisibleTo(this))
        return;
    placeBar_->hide();
    addresses_->clearPlace();
    history_->clearPlace();
}

void MainWindow::openRoot(Place::Root root)
{
    switch (root) {
    case Place::Root::Portfolio: openPage(PagePortfolio); return;
    case Place::Root::Wallets: openPage(PageWallets); return;
    case Place::Root::Addresses: openPage(PageAddresses); return;
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (stack_->currentWidget() == receive_)
        receive_->leaving();
    QWidget::closeEvent(event);
}

void MainWindow::goTo(Place place, Place::Tab tab)
{
    if (stack_->currentWidget() == receive_)
        receive_->leaving();
    // One wallet: a step through it says nothing.
    const QList<wallet::WalletEntry>& wallets = controller_->store().wallets;
    if (place.asset && place.wallet == 0 && wallets.size() == 1) {
        place.wallet = wallets.first().id;
        place.onlyWallet = true;
    }
    if ((tab == Place::Tab::Holdings && !place.hasHoldings()) || (tab == Place::Tab::Addresses && !place.hasAddressesTab()))
        tab = place.firstTab();

    switch (place.root) {
    case Place::Root::Portfolio: nav_->select(PagePortfolio); break;
    case Place::Root::Wallets: nav_->select(PageWallets); break;
    case Place::Root::Addresses: nav_->select(PageAddresses); break;
    }
    // Each narrowed as it comes up, and first, so that what is left of the last place never shows.
    switch (tab) {
    case Place::Tab::Holdings:
        if (place.address)
            holdings_->table()->showAddress(*place.address);
        else if (place.asset)
            holdings_->table()->showWallets(*place.asset);
        else
            holdings_->table()->showCoins(place.wallet);
        stack_->setCurrentWidget(holdings_);
        break;
    case Place::Tab::Addresses:
        addresses_->showPlace(place.wallet, place.asset);
        stack_->setCurrentWidget(addresses_);
        break;
    case Place::Tab::History:
        if (place.address)
            history_->showPlace(place.address->wallet, place.address->network, place.address->scripthash, place.address->address);
        else
            history_->showPlace(place.wallet, place.network(), QString(), QString());
        stack_->setCurrentWidget(history_);
        break;
    }
    placeBar_->showPlace(place, tab);
}

void MainWindow::afterWalletChange()
{
    if (!placeBar_->isVisibleTo(this))
        return;
    Place place = placeBar_->place();
    const wallet::WalletStore& store = controller_->store();
    const bool walletGone = place.wallet != 0 && !store.find(place.wallet);
    const bool addressGone = place.address && !store.find(place.address->wallet);
    if (walletGone || addressGone) {
        openRoot(place.root);
        return;
    }
    // Its name may have changed; one wallet may have become two, or the reverse.
    if (place.onlyWallet && store.wallets.size() != 1) {
        place.onlyWallet = false;
        goTo(place, placeBar_->tab());
    }
    placeBar_->showPlace(placeBar_->place(), placeBar_->tab());
}

// ---------------------------------------------------------------- payments ----

void MainWindow::refreshBell()
{
    title_->setUnreadPayments(controller_->payments()->unread());
}

void MainWindow::openPayments()
{
    // A notification can be clicked with the window locked.
    if (locked_)
        return;
    auto* dialog = new PaymentsDialog(controller_, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &PaymentsDialog::openAddressRequested, this, [this](const wallet::AddressRow& row) {
        Place place;
        place.root = Place::Root::Addresses;
        place.address = row;
        // A network with no history yet opens on what the address holds.
        goTo(place, controller_->historyOf(row.network) ? Place::Tab::History : Place::Tab::Holdings);
    });
    // open(), not exec(): the scanners go on, and so does the list.
    dialog->open();
}

void MainWindow::openStatus()
{
    if (locked_)
        return;
    auto* dialog = new StatusDialog(controller_, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void MainWindow::refreshNow()
{
    if (locked_ || !controller_->hasWallet() || refreshCooldown_->isActive())
        return;
    controller_->refresh();
    Feedback::show(this, Feedback::Type::Info, trText(QStringLiteral("Refreshing prices and balances")));
    title_->setRefreshEnabled(false);
    refreshCooldown_->start();
}

void MainWindow::openPaymentAddress(const wallet::Payment& payment)
{
    const wallet::AssetInfo* info = wallet::findAsset(payment.asset);
    if (!info)
        return;
    const wallet::WalletModel* rows = controller_->model();
    for (int row = 0; row < rows->size(); ++row) {
        const wallet::AddressRow& it = rows->at(row);
        if (it.network == info->network && it.wallet == payment.wallet && it.address == payment.address) {
            Place place;
            place.root = Place::Root::Addresses;
            place.address = it;
            // A network with no history yet opens on what the address holds.
            goTo(place, controller_->historyOf(it.network) ? Place::Tab::History : Place::Tab::Holdings);
            return;
        }
    }
    Feedback::show(this, Feedback::Type::Info, trText(QStringLiteral("That address is not among those followed now")));
}

namespace {

// How far a payment can be counted on, by what this PC has checked.
enum class Stands { Balance, Pending, Unverified, Proven };

Stands standingOf(const WalletController* controller, const wallet::Payment& payment)
{
    if (!payment.hasTx)
        return Stands::Balance;
    const wallet::AssetInfo* info = wallet::findAsset(payment.asset);
    const wallet::UtxoHistory* history = info ? controller->historyOf(info->network) : nullptr;
    const int position = history ? history->book()->find(payment.txid) : -1;
    if (position < 0 || history->book()->at(position).height <= 0)
        return Stands::Pending;
    return history->book()->at(position).proven ? Stands::Proven : Stands::Unverified;
}

QString worthOf(const wallet::Payment& payment)
{
    const wallet::AssetInfo* info = wallet::findAsset(payment.asset);
    QString text = info ? figures::formatAmount(payment.amount, *info) + QLatin1Char(' ') + info->symbol : payment.asset;
    if (payment.usd > 0.0)
        text += QStringLiteral(" (%1)").arg(figures::formatUsd(payment.usd));
    return text;
}

// How much, and for whom: the address's label says that better than the wallet's name.
QString namedOf(const WalletController* controller, const wallet::Payment& payment)
{
    QString who = controller->addressLabel(payment.address);
    if (who.isEmpty()) {
        const wallet::WalletEntry* entry = controller->store().find(payment.wallet);
        who = entry ? entry->label : QString();
    }
    return who.isEmpty() ? worthOf(payment) : QStringLiteral("%1 · %2").arg(worthOf(payment), who);
}

constexpr int kPaymentAlertMs = 8000;

} // namespace

void MainWindow::onPaymentsTold(const QList<wallet::Payment>& payments)
{
    if (payments.isEmpty() || SettingsRepo().get(QStringLiteral("ui.paymentAlerts")) == QLatin1String("0"))
        return;
    QString text;
    bool allProven = true;
    if (payments.size() == 1) {
        const wallet::Payment& one = payments.first();
        const QString named = namedOf(controller_, one);
        const Stands stands = standingOf(controller_, one);
        allProven = stands == Stands::Proven;
        // What is in no block, or in one nobody has proven, can still go: it is not told of as received.
        switch (stands) {
        case Stands::Balance:
            text = (one.whileClosed ? trText(QStringLiteral("Balance went up while GitWallet was closed: %1")) : trText(QStringLiteral("Balance went up: %1"))).arg(named);
            break;
        case Stands::Pending:
            text = (one.whileClosed ? trText(QStringLiteral("Came while GitWallet was closed, not confirmed yet: %1")) : trText(QStringLiteral("Coming in, not confirmed yet: %1"))).arg(named);
            break;
        case Stands::Unverified:
            text = (one.whileClosed ? trText(QStringLiteral("Came while GitWallet was closed, not verified yet: %1")) : trText(QStringLiteral("Coming in, not verified yet: %1"))).arg(named);
            break;
        case Stands::Proven:
            text = (one.whileClosed ? trText(QStringLiteral("Came in while GitWallet was closed: %1")) : trText(QStringLiteral("Received %1"))).arg(named);
            break;
        }
    } else {
        double usd = 0.0;
        bool priced = true;
        int balances = 0;
        int notYet = 0;
        int closed = 0;
        for (const wallet::Payment& payment : payments) {
            usd += payment.usd;
            priced = priced && payment.usd > 0.0;
            const Stands stands = standingOf(controller_, payment);
            balances += stands == Stands::Balance ? 1 : 0;
            notYet += stands == Stands::Pending || stands == Stands::Unverified ? 1 : 0;
            closed += payment.whileClosed ? 1 : 0;
            allProven = allProven && stands == Stands::Proven;
        }
        const int count = int(payments.size());
        const bool onlyBalances = balances == count;
        if (closed == count) {
            text = (onlyBalances ? trText(QStringLiteral("%1 balances went up while GitWallet was closed")) : trText(QStringLiteral("%1 payments came in while GitWallet was closed"))).arg(figures::formatCount(count));
        } else {
            text = (onlyBalances ? trText(QStringLiteral("%1 balances went up")) : trText(QStringLiteral("%1 payments came in"))).arg(figures::formatCount(count));
        }
        if (priced)
            text += QStringLiteral(" · ") + figures::formatUsd(usd);
        if (closed > 0 && closed < count)
            text += QStringLiteral(" · ") + trText(QStringLiteral("%1 while GitWallet was closed")).arg(figures::formatCount(closed));
        if (notYet > 0)
            text += QStringLiteral(" · ") + trText(QStringLiteral("%1 pending")).arg(figures::formatCount(notYet));
    }
    const QString bare = trText(QStringLiteral("A payment came in"));
    // Nothing of it to whoever is looking at a locked window.
    sayPayment(locked_ ? bare : text, bare, int(allProven ? Feedback::Type::Success : Feedback::Type::Info));
}

void MainWindow::onPaymentsSettled(const wallet::PaymentNews& news)
{
    if (SettingsRepo().get(QStringLiteral("ui.paymentAlerts")) == QLatin1String("0"))
        return;
    // Nothing of it to whoever is looking at a locked window: it waits for whoever opens it.
    if (locked_) {
        heldNews_.add(news);
        return;
    }
    sayNews(news);
}

void MainWindow::sayNews(const wallet::PaymentNews& news)
{
    if (news.isEmpty())
        return;
    const auto named = [this](const wallet::Payment& payment) { return namedOf(controller_, payment); };
    const qsizetype all = news.gone.size() + news.noProof.size() + news.leftABlock.size() + news.wentBack.size() + news.listedAgain.size() + news.inABlock.size();
    QString text;
    if (all == 1) {
        if (!news.gone.isEmpty())
            text = trText(QStringLiteral("Replaced or dropped, so not paid: %1")).arg(named(news.gone.first()));
        else if (!news.noProof.isEmpty())
            text = trText(QStringLiteral("Could not be verified, so do not count it: %1")).arg(named(news.noProof.first()));
        else if (!news.leftABlock.isEmpty())
            text = trText(QStringLiteral("No longer confirmed: %1")).arg(named(news.leftABlock.first()));
        else if (!news.wentBack.isEmpty())
            text = trText(QStringLiteral("A balance went back down: %1. Check before counting on it")).arg(named(news.wentBack.first()));
        else if (!news.listedAgain.isEmpty())
            text = trText(QStringLiteral("Listed again, not confirmed yet: %1")).arg(named(news.listedAgain.first()));
        else
            text = trText(QStringLiteral("First confirmation: %1")).arg(named(news.inABlock.first()));
    } else {
        // What was counted on and will not come goes first: it is what someone has to act on.
        QStringList parts;
        const auto add = [&parts](const QList<wallet::Payment>& some, const QString& words) {
            if (!some.isEmpty())
                parts << trText(words).arg(figures::formatCount(int(some.size())));
        };
        add(news.gone, QStringLiteral("%1 not paid"));
        add(news.noProof, QStringLiteral("%1 could not be verified"));
        add(news.leftABlock, QStringLiteral("%1 no longer confirmed"));
        add(news.wentBack, QStringLiteral("%1 balances went back down"));
        add(news.listedAgain, QStringLiteral("%1 listed again"));
        add(news.inABlock, QStringLiteral("%1 confirmed"));
        text = trText(QStringLiteral("Payments: %1")).arg(parts.join(QStringLiteral(" · ")));
    }
    sayPayment(text, trText(QStringLiteral("News of a payment: see the bell")), int(news.worrying() ? Feedback::Type::Warning : Feedback::Type::Success));
}

void MainWindow::sayPaymentForTest(const QString& text)
{
    sayPayment(text, text, int(Feedback::Type::Info));
}

bool MainWindow::trayShown() const
{
    return tray_ && tray_->isVisible();
}

void MainWindow::sayPayment(const QString& text, const QString& bare, int feedbackType)
{
    lastPaymentAlert_ = text;
    if (SettingsRepo().get(QStringLiteral("ui.paymentAlertsSound")) == QLatin1String("1"))
        QApplication::beep();
    if (QGuiApplication::applicationState() == Qt::ApplicationActive) {
        Feedback::show(this, Feedback::Type(feedbackType), text, kPaymentAlertMs);
        return;
    }
    // Windows keeps what it is told, in its own list, whatever locks here later.
    lastPaymentNotification_ = controller_->hasPassword() ? bare : text;
    if (SettingsRepo().get(QStringLiteral("ui.paymentAlertsWindows")) != QLatin1String("0") && QSystemTrayIcon::isSystemTrayAvailable()) {
        if (!tray_) {
            tray_ = new QSystemTrayIcon(QApplication::windowIcon(), this);
            tray_->setToolTip(QStringLiteral("GitWallet"));
            const auto bringBack = [this] {
                setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
                raise();
                activateWindow();
            };
            connect(tray_, &QSystemTrayIcon::activated, this, bringBack);
            connect(tray_, &QSystemTrayIcon::messageClicked, this, [this, bringBack] {
                bringBack();
                openPayments();
            });
            tray_->show();
        }
        tray_->showMessage(QStringLiteral("GitWallet"), lastPaymentNotification_, QSystemTrayIcon::Information);
    }
    QApplication::alert(this); // the taskbar button says so too
}

// ------------------------------------------------------------------ the lock ----

void MainWindow::lockNow()
{
    if (locked_ || !controller_->hasPassword())
        return;
    // What is open goes first, a seed being typed included: it must not stay on top of the lock. What
    // will not close, a note that cannot be written or work under way, goes out of sight all the same,
    // and comes back with the unlock: the window locks, whatever is open. Only what open() runs comes
    // to that, as hiding what exec() runs would end it.
    const QWidgetList windows = QApplication::topLevelWidgets();
    for (QWidget* widget : windows) {
        auto* dialog = qobject_cast<QDialog*>(widget);
        if (!dialog || !dialog->isVisible())
            continue;
        if (auto* list = qobject_cast<PaymentsDialog*>(dialog))
            list->closingForLock();
        dialog->done(QDialog::Rejected);
        if (dialog->isVisible()) {
            dialog->hide();
            const QPointer<QDialog> held(dialog);
            heldDialogs_.append(held);
            connect(dialog, &QDialog::finished, this, [this, held] { heldDialogs_.removeAll(held); });
        }
    }
    if (QWidget* popup = QApplication::activePopupWidget())
        popup->close();
    showLocked();
}

void MainWindow::showLocked()
{
    if (stack_->currentWidget() == receive_)
        receive_->leaving();
    locked_ = true;
    idleCheck_->stop();
    forgetOwnClipboard();
    showIconOnly(true);
    bodyHost_->hide();
    title_->setUtilityButtonsVisible(false);
    lockPage_->reset();
    lockPage_->show();
    lockPage_->field()->setFocus();
    refreshStatus();
}

void MainWindow::showIconOnly(bool iconOnly)
{
#ifdef Q_OS_WIN
    // A window left minimized is not drawn again, and Windows goes on showing its last picture in the
    // taskbar and in Alt+Tab, whatever it was on when it locked.
    if (QGuiApplication::platformName() == QLatin1String("windows")) {
        const BOOL value = iconOnly ? TRUE : FALSE;
        DwmSetWindowAttribute(HWND(winId()), DWMWA_FORCE_ICONIC_REPRESENTATION, &value, sizeof value);
    }
#else
    Q_UNUSED(iconOnly);
#endif
}

void MainWindow::showUnlocked()
{
    locked_ = false;
    showIconOnly(false);
    lockPage_->reset();
    lockPage_->hide();
    bodyHost_->show();
    title_->setUtilityButtonsVisible(true);
    idleClock_.restart();
    afterPasswordChange();
    refreshStatus();
    const QList<QPointer<QDialog>> held = std::exchange(heldDialogs_, {});
    for (const QPointer<QDialog>& dialog : held) {
        if (dialog)
            dialog->open();
    }
    sayNews(std::exchange(heldNews_, {}));
    emit unlocked();
}

void MainWindow::tryUnlock(const QString& password)
{
    lockPage_->setBusy(true);
    const auto finish = [this](const QString& error) {
        if (error.isEmpty()) {
            showUnlocked();
            return;
        }
        lockPage_->reset();
        lockPage_->setError(error);
    };
    if (controller_->needsPassword()) {
        controller_->openWithPassword(password, this, finish);
        return;
    }
    controller_->checkPassword(password, this, finish);
}

void MainWindow::forgotPassword()
{
    auto* box = new QMessageBox(QMessageBox::Information, trText(QStringLiteral("Forgot your password")),
                                trText(QStringLiteral("Nobody can open this without the password, GitWallet included: there is no way around it, and no one to ask.")),
                                QMessageBox::NoButton, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setObjectName(QStringLiteral("forgotBox"));
    if (!controller_->needsPassword()) {
        // All of it is in use behind the lock: starting over begins with a start.
        box->setInformativeText(trText(QStringLiteral("Your coins are safe: they are on their seeds, which were never stored here. Close GitWallet and open it again, and this same link offers to start over.")));
        box->addButton(QMessageBox::Ok);
        applyDialogChrome(box);
        box->open();
        return;
    }
    box->setInformativeText(trText(QStringLiteral("Your coins are safe: they are on their seeds, which were never stored here. You can start over: the file stays beside the new one under another name, in case the password comes back to you, and you add your seeds again. Labels, notes and addresses given out come back only from a backup.")));
    QPushButton* startOver = box->addButton(trText(QStringLiteral("Set the file aside and start over")), QMessageBox::DestructiveRole);
    startOver->setObjectName(QStringLiteral("dangerButton"));
    QPushButton* cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    applyDialogChrome(box);
    connect(box, &QDialog::finished, this, [this, box, startOver] {
        if (box->clickedButton() != startOver)
            return;
        QString error;
        if (controller_->setAsideLockedFile(&error))
            showUnlocked();
        else
            lockPage_->setError(error);
    });
    box->open();
}

void MainWindow::afterPasswordChange()
{
    const bool has = controller_->hasPassword();
    title_->setLockAvailable(has);
    const bool watching = has && !locked_ && (controller_->lockMinutes() > 0 || idleLimitForTestMs_ > 0);
    if (watching && !idleCheck_->isActive())
        idleCheck_->start();
    else if (!watching)
        idleCheck_->stop();
}

void MainWindow::checkIdle()
{
    const qint64 limit = idleLimitForTestMs_ > 0 ? idleLimitForTestMs_ : qint64(controller_->lockMinutes()) * 60 * 1000;
    if (limit > 0 && idleClock_.elapsed() >= limit)
        lockNow();
}

void MainWindow::setIdleLimitForTest(int ms)
{
    idleLimitForTestMs_ = ms;
    idleCheck_->setInterval(ms > 0 ? qMax(10, ms / 5) : kIdleCheckMs);
    afterPasswordChange();
}

void MainWindow::editAddressLabel(const QString& address)
{
    addresses_->editLabel(address);
}

void MainWindow::openImportDialog(bool many)
{
    ImportSeedDialog dialog(controller_, this, many);
    dialog.exec();
    // What was just added is on the page that lists the wallets.
    const QString result = dialog.resultText();
    if (result.isEmpty())
        return;
    openPage(PageWallets);
    Feedback::show(this, Feedback::Type::Success, result);
}

void MainWindow::loadWallet()
{
    QString error;
    if (controller_->loadExisting(&error) || error.isEmpty()) {
        if (controller_->needsPassword())
            showLocked();
        afterPasswordChange();
        return;
    }
    // The file exists but cannot be read (another Windows user, damaged).
    QMessageBox box(QMessageBox::Warning, QStringLiteral("GitWallet"),
                    QStringLiteral("The saved wallets could not be opened.\n\n%1\n\n%2")
                        .arg(error, controller_->unreadFileNote()),
                    QMessageBox::Ok, this);
    applyDialogChrome(&box);
    box.exec();
}

// ---------------------------------------------------------------- problems ----

// A problem from errorOccurred, as the notice bar shows it. Its kind decides
// how it is kept and how serious it is; its words are the message itself,
// GitWallet's own sentence, which the scanners and the controller keep free of
// anything a server wrote. What a server did write (serverDetail) never
// reaches the bar or a toast, since a hostile server can put anything there:
// a "security update" to download, a request for the recovery phrase. Only
// the tooltip lists it, as plain text marked as the server's words, and only
// a reason safeDetail() recognises joins the sentence, in GitWallet's words.
MainWindow::Notice MainWindow::classify(wallet::Problem kind, const QString& message,
                                        const QString& serverDetail)
{
    const QString own = cleanLine(message); // one line, as the bar lists it
    Notice n;
    n.text = sentence(own, safeDetail(serverDetail.left(kMaxMessageChars)));
    n.detail = cleanLine(serverDetail);
    // A failed lookup unless the kind says more: one notice per sentence,
    // whatever the server answered, so a server that varies its error texts
    // still raises one toast per kToastRepeatMs.
    n.key = QStringLiteral("failure:") + own;
    n.headline = trText(QStringLiteral("A wallet server lookup failed"));
    n.severity = kSeverityWarning;
    n.depends = kDependsAnySetting; // the next restart asks again anyway
    switch (kind) {
    case wallet::Problem::Failure:
        break;
    case wallet::Problem::Limit:
        // Bitcoin for the whole wallet, Ethereum per account. Another server,
        // endpoint or gap can end it, and a restart reports again whatever
        // still holds.
        n.key = QStringLiteral("limit:") + own;
        n.headline = trText(QStringLiteral("Address limit reached"));
        n.severity = kSeverityIncomplete;
        n.depends = kDependsBtcGaps | kDependsEthGap | kDependsServers;
        n.settingsFix = true;
        break;
    case wallet::Problem::WrongChain:
        // One notice whatever the evidence (a chain id, no Multicall3, a token
        // contract missing); the latest sentence names it.
        n.key = QStringLiteral("wrong-chain");
        n.headline = trText(QStringLiteral("A server answers for another network"));
        n.severity = kSeverityIncomplete;
        n.depends = kDependsServers;
        n.settingsFix = true;
        break;
    case wallet::Problem::BuiltInServers:
        n.key = QStringLiteral("built-in-servers");
        n.headline = trText(QStringLiteral("Using the built-in Electrum servers"));
        n.depends = kDependsServers;
        n.settingsFix = true;
        break;
    case wallet::Problem::Skipped:
        // An account or chain that cannot be scanned at all: its balances stay
        // missing from the totals.
        n.key = QStringLiteral("skipped:") + own;
        n.headline = trText(QStringLiteral("Some addresses are not scanned"));
        n.severity = kSeverityIncomplete;
        n.depends = 0; // the wallet's own keys: only another wallet changes that
        break;
    case wallet::Problem::FileSetAside:
        n.key = QStringLiteral("file-set-aside");
        n.headline = trText(QStringLiteral("The old wallet file was kept aside"));
        n.depends = 0;
        break;
    case wallet::Problem::SavedData:
        // Nothing on screen is wrong: the next start only has to read everything again.
        n.key = QStringLiteral("saved-data");
        n.headline = trText(QStringLiteral("Saved balances are not being kept"));
        n.depends = 0;
        break;
    case wallet::Problem::FolderHeld:
        n.key = QStringLiteral("folder-held");
        n.headline = trText(QStringLiteral("Saved balances are not being kept"));
        n.depends = 0;
        break;
    case wallet::Problem::LabelsNotKept:
        n.key = QStringLiteral("labels-not-kept");
        n.headline = trText(QStringLiteral("Labels and notes are not being saved"));
        n.depends = 0;
        break;
    case wallet::Problem::LabelsOwed:
        n.key = QStringLiteral("labels-owed");
        n.headline = trText(QStringLiteral("Labels and notes from an older file have not come over yet"));
        n.depends = 0;
        break;
    case wallet::Problem::GivenOutNotKept:
        n.key = QStringLiteral("given-out-not-kept");
        n.headline = trText(QStringLiteral("An address given out could not be saved"));
        n.severity = kSeverityWarning;
        n.depends = 0;
        break;
    case wallet::Problem::FilesLeft:
        n.key = QStringLiteral("files-left");
        n.headline = trText(QStringLiteral("Some files could not be deleted"));
        n.depends = 0;
        break;
    case wallet::Problem::SavedDataAside:
        // The same: a new database is in use, and this says what became of the old one.
        n.key = QStringLiteral("saved-data-aside");
        n.headline = trText(QStringLiteral("Saved balances are being read again"));
        n.depends = 0;
        break;
    }
    return n;
}

void MainWindow::refreshStatus()
{
    // Load, import, removal and Settings > Save each end with one of these
    // signals, so this is where notices about the old inputs go.
    if (pruneNotices())
        renderNotices();
    // A lookup that failed and has since been made good needs nobody's attention: the figures say
    // so by being current. Marked, not dropped, so a repeat within kToastRepeatMs stays quiet.
    if (controller_->figuresCurrent()) {
        bool cleared = false;
        for (Notice& n : notices_) {
            if (!n.seen && n.key.startsWith(QLatin1String("failure:"))) {
                n.seen = true;
                cleared = true;
            }
        }
        if (cleared)
            renderNotices();
    }
    // The controller's line says by itself when a balance is missing or stale;
    // dismissing the notices does not hide that.
    if (locked_) {
        title_->setStatus(trText(QStringLiteral("Locked")), QString(), toneColor(Tone::Muted));
    } else {
        const Tone tones[] = {Tone::Success, Tone::Accent, Tone::Warning, Tone::Error, Tone::Muted};
        title_->setStatus(controller_->statusLead(), controller_->statusText() + QLatin1Char('\n') + trText(QStringLiteral("Click for the scan status and servers")), toneColor(tones[int(controller_->statusTone())]));
    }
    title_->setRefreshAvailable(controller_->hasWallet());
}

void MainWindow::onWalletError(wallet::Problem kind, const QString& message, const QString& serverDetail)
{
    // The developer trail keeps both parts as they came, on one line; the
    // screen only ever gets classify()'s wording.
    QString trail = cleanLine(message);
    if (!serverDetail.isEmpty())
        trail += QStringLiteral(" | server: ") + cleanLine(serverDetail);
    qWarning().noquote() << "[wallet]" << trail;
    if (message.trimmed().isEmpty())
        return;

    // A restart reports before it signals (startServices() runs before the
    // controller's statusChanged / walletChanged), and the store already holds
    // the new wallet and settings by then: notices about the old ones go first,
    // so a repeat after a change starts afresh instead of extending their count.
    pruneNotices();

    // Reported again while still listed (an identical restart, or a new
    // outage): the latest wording, inputs and time, one more in the count.
    Notice reported = classify(kind, message, serverDetail);
    if (kind == wallet::Problem::FileSetAside)
        reported.file = controller_->fileSetAside(); // what pruneNotices() looks for
    reported.inputs = currentInputs();
    const qint64 wall = QDateTime::currentMSecsSinceEpoch();
    qsizetype i = 0;
    while (i < notices_.size() && notices_.at(i).key != reported.key)
        ++i;
    if (i == notices_.size()) {
        if (notices_.size() >= kMaxNotices) {
            // The least serious goes first, the oldest of those: a notice that
            // leaves balances incomplete only once nothing else is left.
            const auto dropsFirst = [](const Notice& a, const Notice& b) {
                return a.severity != b.severity ? a.severity < b.severity : a.lastMs < b.lastMs;
            };
            notices_.erase(std::min_element(notices_.begin(), notices_.end(), dropsFirst));
        }
        reported.firstMs = wall;
        notices_.append(reported);
        i = notices_.size() - 1;
    }
    Notice& n = notices_[i];
    if (n.count > 0) {
        n.headline = reported.headline;
        n.text = reported.text;
        n.detail = reported.detail;
        n.file = reported.file;
        n.severity = reported.severity;
        n.depends = reported.depends;
        n.settingsFix = reported.settingsFix;
        n.inputs = reported.inputs;
    }
    n.lastMs = wall;
    if (n.count < std::numeric_limits<int>::max())
        ++n.count;

    // A burst (BtcScanner reports up to eight problems at once) makes one toast.
    // The same notice toasts again only after kToastRepeatMs, and one dismissed
    // meanwhile stays hidden until then, counting on.
    const qint64 now = monotonicMs();
    if (n.toastedMs < 0 || now - n.toastedMs >= kToastRepeatMs) {
        n.toastedMs = now;
        n.seen = false;
        if (!toastQueue_.contains(n.key))
            toastQueue_.append(n.key);
        if (!toastPosted_) {
            toastPosted_ = true;
            // Posted, like the controller's own warning: it lands after the
            // caller's feedback ("Settings saved") instead of under it.
            QTimer::singleShot(0, this, &MainWindow::flushToast);
        }
    }
    renderNotices();
}

bool MainWindow::pruneNotices()
{
    if (notices_.isEmpty())
        return false;
    const Inputs now = currentInputs();
    const qsizetype dropped = notices_.removeIf([this, &now](const Notice& n) {
        const Inputs& was = n.inputs;
        // The only line that names the file kept aside: it stays as long as that file does.
        if (n.key == QLatin1String("file-set-aside"))
            return !QFile::exists(n.file);
        if (n.key == QLatin1String("folder-held"))
            return controller_->keepingSaved();
        if (n.key == QLatin1String("labels-owed"))
            return !controller_->labelsOwed();
        if (n.key == QLatin1String("given-out-not-kept"))
            return !controller_->givenOutOwed();
        if (was.wallet != now.wallet)
            return true; // another wallet, or none
        const bool btcGaps = was.receiveGap != now.receiveGap || was.changeGap != now.changeGap;
        const bool ethGap = was.ethGap != now.ethGap;
        const bool servers = was.servers != now.servers;
        const bool anySetting = btcGaps || ethGap || servers || was.refreshSeconds != now.refreshSeconds;
        return ((n.depends & kDependsBtcGaps) && btcGaps) || ((n.depends & kDependsEthGap) && ethGap)
               || ((n.depends & kDependsServers) && servers)
               || ((n.depends & kDependsAnySetting) && anySetting);
    });
    return dropped > 0;
}

void MainWindow::renderNotices()
{
    const bool unread =
        std::any_of(notices_.cbegin(), notices_.cend(), [](const Notice& n) { return !n.seen; });
    if (!unread) {
        noticeBar_->hide();
        return;
    }

    // The most serious first, then the most recent.
    QList<const Notice*> order;
    order.reserve(notices_.size());
    for (const Notice& n : std::as_const(notices_))
        order.append(&n);
    std::stable_sort(order.begin(), order.end(), [](const Notice* a, const Notice* b) {
        return a->severity != b->severity ? a->severity > b->severity : a->lastMs > b->lastMs;
    });

    // The bar shows GitWallet's own words only. The tooltip lists every notice,
    // each followed by what its server wrote, if anything, marked as such. It
    // stays plain text, never markup: Qt reads a tooltip as rich text only when
    // its first line looks like markup, and that line is GitWallet's own.
    QStringList lines;
    QStringList tip{trText(QStringLiteral("Every notice, the most serious first:"))};
    bool settingsFix = false;
    for (const Notice* n : std::as_const(order)) {
        QString line = QStringLiteral("%1 · %2").arg(noticeTime(n->lastMs), n->text);
        if (n->count > 1)
            line += QLatin1Char(' ')
                    + trText(QStringLiteral("(%1 times since %2)"))
                          .arg(grouped(n->count), noticeTime(n->firstMs));
        lines.append(line);
        tip.append(wrapped(line, QString()));
        if (!n->detail.isEmpty())
            tip.append(wrapped(trText(QStringLiteral("Server's words, unverified: %1")).arg(n->detail),
                               QStringLiteral("    ")));
        settingsFix = settingsFix || n->settingsFix;
    }
    QStringList shown = lines.mid(0, kNoticeLines);
    if (lines.size() > kNoticeLines)
        shown.append(trText(QStringLiteral("…and %1 more; hover here to read them all"))
                         .arg(grouped(lines.size() - kNoticeLines)));
    noticeText_->setText(shown.join(QLatin1Char('\n')));
    noticeText_->setToolTip(tip.join(QLatin1Char('\n')));

    const int top = order.constFirst()->severity;
    if (top >= kSeverityIncomplete)
        setChip(noticeChip_, trText(QStringLiteral("Balances incomplete")), Tone::Error);
    else if (top == kSeverityWarning)
        setChip(noticeChip_, trText(QStringLiteral("Attention")), Tone::Warning);
    else
        setChip(noticeChip_, trText(QStringLiteral("Notice")), Tone::Muted);
    noticeSettings_->setVisible(settingsFix);
    noticeBar_->show();
}

void MainWindow::dismissNotices()
{
    // Kept, not dropped, so a repeat within kToastRepeatMs stays quiet. The
    // title chip says by itself when a balance is missing.
    for (Notice& n : notices_)
        n.seen = true;
    renderNotices();
}

void MainWindow::flushToast()
{
    toastPosted_ = false;
    const QStringList keys = std::exchange(toastQueue_, QStringList());
    const Notice* top = nullptr;
    int count = 0;
    for (const Notice& n : std::as_const(notices_)) {
        if (!keys.contains(n.key))
            continue;
        ++count;
        if (!top || n.severity > top->severity)
            top = &n;
    }
    if (!top)
        return; // dropped meanwhile: its wallet or settings changed
    const QString text =
        count > 1 ? trText(QStringLiteral("%1 (+%2 more)")).arg(top->headline, QString::number(count - 1))
                  : top->headline;
    Feedback::show(this, toastType(top->severity), text);
}

MainWindow::Inputs MainWindow::currentInputs() const
{
    const wallet::WalletStore& store = controller_->store();
    Inputs in;
    const QList<wallet::AccountKey> accounts = store.allAccounts();
    for (const wallet::AccountKey& key : accounts)
        in.wallet = qHashMulti(in.wallet, key.wallet, int(key.type), key.account, key.xpub);
    if (!accounts.isEmpty() && in.wallet == 0)
        in.wallet = 1; // 0 stands for "no wallet"
    const wallet::ScanSettings& s = store.settings;
    in.receiveGap = s.utxoReceiveGap;
    in.changeGap = s.utxoChangeGap;
    in.ethGap = s.evmGap;
    in.refreshSeconds = s.evmRefreshSeconds;
    in.servers = s.servers; // implicitly shared: comparing is cheap while unchanged
    return in;
}

// ------------------------------------------------------------------ window ----

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
    // A narrow window goes to the rail for the room, and back to what was chosen.
    constexpr int kNarrowBelow = 1200;
    const bool narrow = width() < kNarrowBelow;
    if (narrow != wasNarrow_ && nav_) {
        wasNarrow_ = narrow;
        nav_->setCollapsed(narrow || navByHand_, false);
    }
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
