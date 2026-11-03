#include "ui/pages/PortfolioPage.h"

#include "ui/HoverRowTable.h"
#include "ui/PaymentsModel.h"
#include "wallet/Payments.h"

#include "app/WalletController.h"
#include "chain/Address.h"
#include "chain/Amount.h"
#include "core/SettingsRepo.h"
#include "net/PriceFeed.h"
#include "ui/Feedback.h"
#include "ui/Figures.h"
#include "ui/Holdings.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/ThemeManager.h"
#include "ui/design/Style.h"
#include "wallet/Types.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QAction>
#include <QCheckBox>
#include <QDate>
#include <QDateTime>
#include <QFormLayout>
#include <QHeaderView>
#include <QTableView>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSet>
#include <QStackedLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QtNumeric>

#include <algorithm>

namespace app::ui {

namespace {

// Manual refreshes go to public Ethereum endpoints that rate-limit; a pause
// between clicks keeps a burst of them from getting the app throttled.
// How often the page looks at how old what it shows has become.
constexpr int kAgeCheckMs = 60 * 1000;
// A scan signals per address: the page follows that many times a second at most.
constexpr int kRowsThrottleMs = 200;

using figures::dash;
using figures::formatCount;

// Why a figure is not final yet: how many addresses have no answer, or, while
// an account has no rows at all (not derived yet, or its key unreadable), just
// that. `alone`: the whole line, rather than a part after " · ".
QString uncheckedNote(int pending, bool alone)
{
    if (pending > 0)
        return trText(pending == 1 ? QStringLiteral("%1 address not checked yet")
                                   : QStringLiteral("%1 addresses not checked yet"))
            .arg(formatCount(pending));
    return alone ? trText(QStringLiteral("Not checked yet"))
                 : trText(QStringLiteral("not all checked yet"));
}

// When the prices were fetched, or the Ethereum balances last answered in full,
// in local time: the time alone while that is `today`, with the date once it
// is not. The feed keeps its last prices through failed refreshes (HTTP 429 is
// routine on the keyless API), and Ethereum figures outlive failing endpoints,
// so either can be from yesterday, and "16:39" alone would read as today.
QString shownTime(const QDateTime& when, const QDate& today)
{
    const QDateTime local = when.toLocalTime();
    if (local.date() == today)
        return local.toString(QStringLiteral("HH:mm"));
    return QLocale(QLocale::English).toString(local, QStringLiteral("d MMM HH:mm"));
}

} // namespace

PortfolioPage::PortfolioPage(WalletController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller)
{
    states_ = new QStackedLayout(this);
    states_->setContentsMargins(0, 0, 0, 0);
    emptyState_ = buildEmptyState();
    walletState_ = buildWalletState();
    states_->addWidget(emptyState_);
    states_->addWidget(walletState_);

    rowsTimer_ = new QTimer(this);
    rowsTimer_->setSingleShot(true);
    rowsTimer_->setInterval(kRowsThrottleMs);
    connect(rowsTimer_, &QTimer::timeout, this, &PortfolioPage::scheduleRefresh);
    if (wallet::WalletModel* model = controller_ ? controller_->model() : nullptr) {
        // How many balances still lack an answer: the model keeps the count.
        const auto rowsMoved = [this] {
            if (!rowsTimer_->isActive())
                rowsTimer_->start();
        };
        connect(model, &wallet::WalletModel::rowsAppended, this, rowsMoved);
        connect(model, &wallet::WalletModel::rowChanged, this, rowsMoved);
        connect(model, &wallet::WalletModel::cleared, this, &PortfolioPage::scheduleRefresh);
    }

    if (controller_) {
        // A wallet appearing or going is rare and changes the whole page: fill
        // it at once, before it is shown, so its first layout already fits the
        // final texts (no stale labels, no row growing a frame late).
        connect(controller_, &WalletController::walletChanged, this, [this] {
            table_->refresh();
            refreshView();
            showState();
        });
        connect(controller_, &WalletController::totalsChanged, this,
                &PortfolioPage::scheduleRefresh);
        connect(controller_, &WalletController::statusChanged, this,
                &PortfolioPage::scheduleRefresh);
    }

    // A price says how old it is, and the time it shows gains its date on another day,
    // so the page also re-renders as time passes under it: while every source keeps
    // failing and no balance moves, no signal would. Once a minute, rather than aiming a
    // timer at a moment, also copes with sleep and clock changes.
    auto* ageCheck = new QTimer(this);
    ageCheck->setTimerType(Qt::VeryCoarseTimer);
    ageCheck->setInterval(kAgeCheckMs);
    connect(ageCheck, &QTimer::timeout, this, &PortfolioPage::scheduleRefresh);
    ageCheck->start();

    table_->refresh();
    refreshView();
    showState();
}

QWidget* PortfolioPage::buildEmptyState()
{
    QWidget* empty = page::makeEmptyState(
        this, static_cast<int>(icon::Kind::Dashboard), QStringLiteral("No wallet yet"),
        QStringLiteral("Add your seeds, one by one or a whole list, or just their public keys, to see "
                       "every address they control."));
    auto* actions = new QWidget(empty);
    auto* row = new QHBoxLayout(actions);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(page::kToolbarSpacing);
    auto* manyButton = new QPushButton(QStringLiteral("Add many"), actions);
    manyButton->setToolTip(QStringLiteral("A list of seeds or public keys, pasted or from a file"));
    connect(manyButton, &QPushButton::clicked, this, &PortfolioPage::importManyRequested);
    auto* importButton = new QPushButton(QStringLiteral("Add wallet"), actions);
    importButton->setObjectName(QStringLiteral("primaryButton"));
    connect(importButton, &QPushButton::clicked, this, &PortfolioPage::importRequested);
    row->addWidget(manyButton);
    row->addWidget(importButton);

    // The buttons join the kit's own centred column, under the subtitle. An
    // outer layout around the empty state squeezed its icon badge instead: it
    // sized the column for a one-line subtitle at the page's full width, while
    // the column wraps the subtitle at its own, narrower width.
    if (QLayout* column = empty->layout()) {
        if (auto* box = qobject_cast<QBoxLayout*>(column))
            box->addSpacing(page::kToolbarSpacing); // a little air above the actions
        column->addWidget(actions);
        column->setAlignment(actions, Qt::AlignHCenter);
    }
    return empty;
}

QWidget* PortfolioPage::buildWalletState()
{
    auto* host = new QWidget(this);
    auto* root = new QVBoxLayout(host);
    page::applyPageLayout(root);

    // The grand total, where the prices behind it come from, and what it is made of.
    auto* hero = new QFrame(host);
    hero->setObjectName(QStringLiteral("cardPanel"));
    auto* heroColumn = new QVBoxLayout(hero);
    heroColumn->setContentsMargins(page::kSectionPadX, page::kSectionPadY, page::kSectionPadX, page::kSectionPadY);
    heroColumn->setSpacing(page::kToolbarSpacing);
    auto* heroRow = new QHBoxLayout;
    heroRow->setSpacing(page::kFormHSpacing);
    auto* figures = new QVBoxLayout;
    figures->setSpacing(2);
    auto* title = new QLabel(QStringLiteral("Total balance"), hero);
    setTone(title, Tone::Muted);
    totalLabel_ = new QLabel(dash(), hero);
    totalLabel_->setObjectName(QStringLiteral("heroValue"));
    totalLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pricesLabel_ = new QLabel(dash(), hero);
    pricesLabel_->setObjectName(QStringLiteral("portfolioCaption"));
    pricesLabel_->setWordWrap(true);
    setTone(pricesLabel_, Tone::Muted);
    figures->addWidget(title);
    figures->addWidget(totalLabel_);
    figures->addWidget(pricesLabel_);
    heroRow->addLayout(figures, 1);
    auto* receive = new QPushButton(QStringLiteral("Receive"), hero);
    receive->setObjectName(QStringLiteral("primaryButton"));
    connect(receive, &QPushButton::clicked, this, &PortfolioPage::receiveRequested);
    heroRow->addWidget(receive, 0, Qt::AlignTop);
    heroColumn->addLayout(heroRow);
    allocation_ = new AllocationBar(hero);
    heroColumn->addWidget(allocation_);
    root->addWidget(hero);

    table_ = new HoldingsTable(controller_, host);
    table_->setObjectName(QStringLiteral("portfolioCoins"));
    table_->showCoins();
    // Many coins later they scroll inside it, and do not push who just paid off the page.
    constexpr int kMostCoinRows = 6;
    table_->view()->setMaximumHeight(table_->view()->horizontalHeader()->sizeHint().height() + kMostCoinRows * 52 + 2);
    root->addWidget(table_);
    connect(table_, &HoldingsTable::coinChosen, this, &PortfolioPage::coinChosen);

    auto* latestRow = new QWidget(host);
    latestTitle_ = latestRow;
    latestRow->setObjectName(QStringLiteral("latestPaymentsTitle"));
    auto* latestLine = new QHBoxLayout(latestRow);
    latestLine->setContentsMargins(0, page::kFormVSpacing, 0, 0);
    latestLine->setSpacing(page::kToolbarSpacing);
    auto* latestTitle = new QLabel(QStringLiteral("Latest payments"), latestRow);
    latestTitle->setObjectName(QStringLiteral("sectionTitle"));
    latestLine->addWidget(latestTitle);
    latestLine->addStretch(1);
    auto* allPayments = new QPushButton(QStringLiteral("All payments"), latestRow);
    allPayments->setObjectName(QStringLiteral("pathLink"));
    connect(allPayments, &QPushButton::clicked, this, &PortfolioPage::paymentsRequested);
    latestLine->addWidget(allPayments);
    root->addWidget(latestRow);

    latestModel_ = new PaymentsModel(controller_, 50, this);
    auto* latest = new HoverRowTableView(host);
    latest_ = latest;
    latest->setObjectName(QStringLiteral("latestPayments"));
    latest->setModel(latestModel_);
    page::configureTable(latest);
    latest->setMouseTracking(true);
    latest->setWordWrap(false);
    latest->setSelectionMode(QAbstractItemView::NoSelection);
    latest->setFocusPolicy(Qt::StrongFocus);
    latest->viewport()->setCursor(Qt::PointingHandCursor);
    QHeaderView* latestHeader = latest->horizontalHeader();
    latestHeader->setStretchLastSection(false);
    latestHeader->setResizeContentsPrecision(0);
    latestHeader->setSectionResizeMode(QHeaderView::ResizeToContents);
    latestHeader->setSectionResizeMode(PaymentsModel::ColLabel, QHeaderView::Stretch);
    latest->setColumnHidden(PaymentsModel::ColAddress, true);
    root->addWidget(latest, 1000);
    root->addStretch(1);
    connect(latest, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) {
        if (const wallet::Payment* payment = latestModel_->at(index.row()))
            emit paymentChosen(*payment);
    });
    connect(controller_->payments(), &wallet::PaymentBook::changed, this, &PortfolioPage::refreshLatest);
    connect(controller_, &WalletController::historyProgress, latestModel_, &PaymentsModel::refreshStates);
    connect(controller_, &WalletController::labelsChanged, latestModel_, &PaymentsModel::refreshLabels);
    refreshLatest();
    connect(table_, &HoldingsTable::refreshed, this, [this] {
        allocation_->setParts(table_->parts());
        refreshCount();
    });

    const page::ActionBar bar = page::makeActionBar(host);
    const page::ViewMenu viewMenu = page::makeViewMenu(bar.widget);
    hideZero_ = page::addViewToggle(this, viewMenu.menu, QStringLiteral("Hide zero balances"), "hideZero", SettingsRepo().get(QStringLiteral("ui.hideZero")) != QLatin1String("0"));
    hideZero_->setToolTip(QStringLiteral("Hides coins with no balance. A coin no server has answered for yet stays."));
    table_->setHideZero(hideZero_->isChecked());
    connect(hideZero_, &QAction::toggled, this, [this](bool on) {
        SettingsRepo().set(QStringLiteral("ui.hideZero"), on ? QStringLiteral("1") : QStringLiteral("0"));
        table_->setHideZero(on);
        refreshView();
    });
    hideDust_ = page::addViewToggle(this, viewMenu.menu, QStringLiteral("Hide tiny amounts"), "hideDust", figures::hideDust());
    table_->setHideDust(hideDust_->isChecked());
    const auto dustTip = [this] {
        hideDust_->setToolTip(trText(QStringLiteral("Hides a coin when you hold less than %1 of it. It still counts in the total.")).arg(figures::dustLimits()));
    };
    dustTip();
    connect(LanguageManager::instance(), &LanguageManager::changed, this, dustTip);
    connect(hideDust_, &QAction::toggled, this, [this](bool on) {
        figures::setHideDust(on);
        table_->setHideDust(on);
        refreshView();
    });
    bar.left->addWidget(viewMenu.button);
    countLabel_ = page::makeBarCount(bar.widget);
    bar.left->addWidget(countLabel_);
    bar.left->addStretch(1);

    root->addWidget(bar.widget);
    return host;
}

void PortfolioPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    hideZero_->setChecked(SettingsRepo().get(QStringLiteral("ui.hideZero")) != QLatin1String("0"));
    hideDust_->setChecked(figures::hideDust());
}

void PortfolioPage::showState()
{
    const bool hasWallet = controller_ && controller_->hasWallet();
    states_->setCurrentWidget(hasWallet ? walletState_ : emptyState_);
}

void PortfolioPage::scheduleRefresh()
{
    // A scan emits these signals for every address it checks; the labels only
    // need the state at the end of each burst, so one repaint per event-loop
    // pass instead of one per row.
    if (refreshPending_)
        return;
    refreshPending_ = true;
    QTimer::singleShot(0, this, [this] {
        refreshPending_ = false;
        refreshView();
    });
}

void PortfolioPage::refreshView()
{
    const QDate today = QDate::currentDate();
    if (!controller_ || !controller_->hasWallet())
        return;

    const wallet::WalletModel* model = controller_->model();

    // An account whose first row does not exist yet has not been derived (or its key
    // could not be read): no balance of that network is known.
    const QList<wallet::AccountKey> accounts = controller_->store().allAccounts();
    QSet<wallet::Network> missing;
    for (const wallet::AccountKey& account : accounts) {
        for (const wallet::NetworkInfo& network : wallet::networks()) {
            if (account.serves(network.network) && model && model->find(account.wallet, network.network, account.type, account.account, 0, 0) < 0)
                missing.insert(network.network);
        }
    }

    // The scan state, per network. Figures of a network from an earlier round (a server
    // on another chain, a round that could not answer every address) say since when, or,
    // before any round answered in full, why not.
    QHash<wallet::Network, wallet::NetworkStatus> states;
    for (const wallet::NetworkInfo& network : wallet::networks())
        states.insert(network.network, controller_->networkStatus(network.network));
    const bool limit = controller_->addressLimitReached();
    const bool saved = controller_->showingSaved();
    const bool unread = controller_->hasUnreadKeys(); // a key's coins are in no figure
    QStringList staleCaptions; // under the total
    bool anyScanning = false;
    for (const wallet::NetworkInfo& network : wallet::networks()) {
        const wallet::NetworkStatus& st = states[network.network];
        anyScanning = anyScanning || st.scanning;
        if (st.wrongChain || st.failing || (st.connecting && !saved && st.balancesAt.isValid())) {
            const QString coin = wallet::assetInfo(network.coin).symbol;
            staleCaptions << (st.balancesAt.isValid()
                                  ? trText(QStringLiteral("%1 balances from %2")).arg(coin, shownTime(st.balancesAt, today))
                              : st.wrongChain ? trText(QStringLiteral("%1 endpoint answers for another network")).arg(network.name)
                                              : trText(QStringLiteral("%1 lookups failing")).arg(coin));
        }
    }

    const ScopeTotal total = totalOf(controller_);
    totalLabel_->setText(total.text());
    allocation_->setParts(table_->parts());

    // Under the total: where the figures come from, why they are not final yet, if they
    // are not, then the prices and how old those are.
    const int uncheckedRows = model ? model->size() - model->checkedCount() : 0;
    QStringList caption;
    if (saved) {
        const QDateTime savedAt = controller_->savedAt();
        caption << (savedAt.isValid() ? trText(QStringLiteral("Saved balances from %1, being checked")).arg(shownTime(savedAt, today))
                                      : trText(QStringLiteral("Saved balances, being checked")));
    }
    if (uncheckedRows > 0)
        caption << uncheckedNote(uncheckedRows, true);
    else if (!missing.isEmpty() || unread)
        caption << trText(QStringLiteral("Not all accounts checked yet"));
    if (limit)
        caption << trText(QStringLiteral("Address limit reached"));
    if (!staleCaptions.isEmpty())
        caption << staleCaptions;
    else if (caption.isEmpty() && anyScanning)
        caption << trText(QStringLiteral("Still scanning"));
    const int inDoubt = controller_->balancesInDoubt();
    if (inDoubt > 0)
        caption << trText(QStringLiteral("%1 addresses do not match their transactions")).arg(formatCount(inDoubt));
    caption << controller_->pricesLine();
    pricesLabel_->setText(caption.join(QStringLiteral(" · ")));

    const int derived = model ? model->size() : 0;
    const int checked = model ? model->checkedCount() : 0;
    const int walletCount = int(controller_->store().wallets.size());
    const bool allAnswered = derived > 0 && checked >= derived;
    countText_ = derived == 0 ? trText(QStringLiteral("%1 wallets")).arg(formatCount(walletCount))
                              : trText(QStringLiteral("%1 wallets · %2 funded of %3 addresses"))
                                    .arg(formatCount(walletCount), figures::countSoFar(model ? model->fundedCount() : 0, allAnswered), formatCount(derived));
    refreshCount();
}

void PortfolioPage::refreshLatest()
{
    latestModel_->refresh();
    const bool any = latestModel_->rowCount() > 0;
    latestTitle_->setVisible(any);
    latest_->setVisible(any);
}

void PortfolioPage::refreshCount()
{
    const int dust = table_->dustHidden();
    countLabel_->setText(dust > 0 ? countText_ + QStringLiteral(" · ") + trText(QStringLiteral("%1 tiny amounts hidden")).arg(formatCount(dust)) : countText_);
}

} // namespace app::ui
