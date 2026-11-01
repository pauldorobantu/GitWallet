#include "ui/MainWindow.h"

#include "app/WalletController.h"
#include "ui/DialogChrome.h"
#include "ui/Feedback.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/SideNav.h"
#include "ui/ThemeManager.h"
#include "ui/TitleBar.h"
#include "ui/WindowShell.h"
#include "ui/design/Style.h"
#include "ui/dialogs/ImportSeedDialog.h"
#include "ui/pages/WalletsPage.h"
#include "ui/pages/AddressesPage.h"
#include "ui/pages/OverviewPage.h"
#include "ui/pages/SettingsPage.h"
#include "wallet/Types.h"
#include "wallet/WalletStore.h"

#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace app::ui {

namespace {

// Page ids = the stacked-widget indices.
enum PageId { PageOverview = 0, PageWallets, PageAddresses, PageSend, PageSettings };

// What a notice depends on besides the wallet (Notice::depends). It goes when
// one of them changes: the scanners then restart and report again whatever
// still holds.
constexpr int kDependsBtcGaps = 1;     // the Bitcoin receive and change gaps
constexpr int kDependsEthGap = 2;
constexpr int kDependsEthEndpoints = 4;
constexpr int kDependsElectrum = 8;    // the Electrum server list
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
    if (d.startsWith(QLatin1String("endpoint is not on Ethereum mainnet")))
        return QStringLiteral("endpoint not on Ethereum mainnet");
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
    refreshStatus();
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
    nav_->addItem(PageWallets, QStringLiteral("Wallets"), int(icon::Kind::Key));
    nav_->addItem(PageAddresses, QStringLiteral("Addresses"), int(icon::Kind::ListRows));
    nav_->addItem(PageSend, QStringLiteral("Send"), int(icon::Kind::ExternalLink));
    nav_->addItem(PageSettings, QStringLiteral("Settings"), int(icon::Kind::Settings));
    body->addWidget(nav_);

    stack_ = new QStackedWidget(this);
    overview_ = new OverviewPage(controller_, stack_);
    wallets_ = new WalletsPage(controller_, stack_);
    addresses_ = new AddressesPage(controller_, stack_);
    settings_ = new SettingsPage(controller_, stack_);
    stack_->addWidget(overview_);
    stack_->addWidget(wallets_);
    stack_->addWidget(addresses_);
    // Spending comes in stage 2, after the view has been verified against
    // other wallets and block explorers.
    stack_->addWidget(page::makeEmptyState(
        stack_, int(icon::Kind::ExternalLink), QStringLiteral("Send"),
        QStringLiteral("Consolidating funds is added in the next stage, once balances are verified.")));
    stack_->addWidget(settings_);

    // The notice bar sits above whichever page is open.
    auto* column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(buildNoticeBar());
    column->addWidget(stack_, 1);
    body->addLayout(column, 1);

    root_->addLayout(body, 1);

    connect(overview_, &OverviewPage::importRequested, this, [this] { openImportDialog(false); });
    connect(overview_, &OverviewPage::importManyRequested, this, [this] { openImportDialog(true); });
    connect(wallets_, &WalletsPage::addRequested, this, [this] { openImportDialog(false); });
    connect(wallets_, &WalletsPage::addManyRequested, this, [this] { openImportDialog(true); });
    connect(wallets_, &WalletsPage::showAddressesRequested, this, [this](quint32 wallet) {
        addresses_->showWallet(wallet);
        openPage(PageAddresses);
    });

    connect(title_, &TitleBar::minimised, this, &QWidget::showMinimized);
    connect(title_, &TitleBar::maximised, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });
    connect(title_, &TitleBar::closed, this, &QWidget::close);
    connect(title_, &TitleBar::menuToggled, this,
            [this] { nav_->setCollapsed(!nav_->isCollapsed()); });
    connect(title_, &TitleBar::languageSelected, this,
            [](const QString& code) { LanguageManager::instance()->setLanguage(code); });
    connect(title_, &TitleBar::settingsRequested, this, [this] { openPage(PageSettings); });

    connect(nav_, &SideNav::navigate, this,
            [this](int pageId, int) { stack_->setCurrentIndex(pageId); });
    connect(nav_, &SideNav::toggleRequested, this,
            [this] { nav_->setCollapsed(!nav_->isCollapsed()); });

    nav_->select(PageOverview);
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
        {QStringLiteral("overview"), PageOverview},
        {QStringLiteral("wallets"), PageWallets},
        {QStringLiteral("addresses"), PageAddresses},
        {QStringLiteral("send"), PageSend},
        {QStringLiteral("settings"), PageSettings},
    };
    const auto it = ids.constFind(name.toLower());
    if (it == ids.cend()) return;
    openPage(*it);
}

void MainWindow::openPage(int pageId)
{
    // SideNav::select() only moves the highlight; the page is switched here.
    nav_->select(pageId);
    stack_->setCurrentIndex(pageId);
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
    if (controller_->loadExisting(&error) || error.isEmpty()) return;
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
        n.depends = kDependsBtcGaps | kDependsEthGap | kDependsElectrum | kDependsEthEndpoints;
        n.settingsFix = true;
        break;
    case wallet::Problem::WrongChain:
        // One notice whatever the evidence (a chain id, no Multicall3, a token
        // contract missing); the latest sentence names it.
        n.key = QStringLiteral("wrong-chain");
        n.headline = trText(QStringLiteral("Ethereum endpoint is not on mainnet"));
        n.severity = kSeverityIncomplete;
        n.depends = kDependsEthEndpoints;
        n.settingsFix = true;
        break;
    case wallet::Problem::BuiltInServers:
        n.key = QStringLiteral("built-in-servers");
        n.headline = trText(QStringLiteral("Using the built-in Electrum servers"));
        n.depends = kDependsElectrum;
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
    }
    return n;
}

void MainWindow::refreshStatus()
{
    // Load, import, removal and Settings > Save each end with one of these
    // signals, so this is where notices about the old inputs go.
    if (pruneNotices())
        renderNotices();
    // The controller's line says by itself when a balance is missing or stale;
    // dismissing the notices does not hide that.
    title_->setStatus(controller_->statusText());
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
    const qsizetype dropped = notices_.removeIf([&now](const Notice& n) {
        const Inputs& was = n.inputs;
        if (was.wallet != now.wallet)
            return true; // another wallet, or none
        const bool btcGaps = was.receiveGap != now.receiveGap || was.changeGap != now.changeGap;
        const bool ethGap = was.ethGap != now.ethGap;
        const bool ethEndpoints = was.ethEndpoints != now.ethEndpoints;
        const bool electrum = was.electrumServers != now.electrumServers;
        const bool anySetting = btcGaps || ethGap || ethEndpoints || electrum
                                || was.refreshSeconds != now.refreshSeconds;
        return ((n.depends & kDependsBtcGaps) && btcGaps) || ((n.depends & kDependsEthGap) && ethGap)
               || ((n.depends & kDependsEthEndpoints) && ethEndpoints)
               || ((n.depends & kDependsElectrum) && electrum)
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
    in.receiveGap = s.btcReceiveGap;
    in.changeGap = s.btcChangeGap;
    in.ethGap = s.ethGap;
    in.refreshSeconds = s.ethRefreshSeconds;
    in.electrumServers = s.electrumServers; // implicitly shared: comparing is cheap while unchanged
    in.ethEndpoints = s.ethEndpoints;
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
