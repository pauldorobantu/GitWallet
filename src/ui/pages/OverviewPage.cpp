#include "ui/pages/OverviewPage.h"

#include "app/WalletController.h"
#include "chain/Address.h"
#include "chain/Amount.h"
#include "net/PriceFeed.h"
#include "ui/Feedback.h"
#include "ui/Figures.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/ThemeManager.h"
#include "ui/design/Style.h"
#include "wallet/Types.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QDate>
#include <QDateTime>
#include <QFormLayout>
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
#include <exception>

namespace app::ui {

namespace {

// Manual refreshes go to public Ethereum endpoints that rate-limit; a pause
// between clicks keeps a burst of them from getting the app throttled.
constexpr int kRefreshCooldownMs = 3000;
// How often the page looks at how old what it shows has become.
constexpr int kAgeCheckMs = 60 * 1000;
// A scan signals per address: the page follows that many times a second at most.
constexpr int kRowsThrottleMs = 200;

using figures::atLeast;
using figures::dash;
using figures::formatAmount;
using figures::formatCount;
using figures::formatUsd;

// The card's second line: what the symbol stands for, and which tokens are counted:
// those of this network's standard only.
QString assetName(const wallet::AssetInfo& info)
{
    return info.standard.isEmpty() ? info.name : QStringLiteral("%1 · %2").arg(info.name, info.standard);
}

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

constexpr qint64 kNoncesOldSeconds = 6 * 3600;

// A server row: its name, or a muted "Not connected" while there is none, and how far
// behind a long round can leave what is not followed live.
void setServer(QLabel* label, const wallet::NetworkStatus& status)
{
    QString text = status.server.isEmpty() ? trText(QStringLiteral("Not connected")) : status.server;
    if (status.tiers.rounds > 0) {
        const int minutes = qMax(1, status.tiers.roundSeconds / 60);
        text += QLatin1String(" · ")
                + trText(QStringLiteral("%1 live, %2 read every %3 min"))
                      .arg(formatCount(status.tiers.live), formatCount(status.tiers.rounds), formatCount(minutes));
    }
    // Nonces are read now and then, and all they can add is a rare address. Long overdue, the
    // figures are as right as ever, but what lies past those windows is not looked at.
    if (status.noncesAt.isValid() && status.noncesAt.secsTo(QDateTime::currentDateTime()) > kNoncesOldSeconds)
        text += QLatin1String(" · ") + trText(QStringLiteral("nonces from %1")).arg(shownTime(status.noncesAt, QDate::currentDate()));
    label->setText(text);
    setTone(label, status.server.isEmpty() ? Tone::Muted : Tone::Neutral);
}

} // namespace

OverviewPage::OverviewPage(WalletController* controller, QWidget* parent)
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
    connect(rowsTimer_, &QTimer::timeout, this, &OverviewPage::scheduleRefresh);
    if (wallet::WalletModel* model = controller_ ? controller_->model() : nullptr) {
        // How many balances still lack an answer: the model keeps the count.
        const auto rowsMoved = [this] {
            if (!rowsTimer_->isActive())
                rowsTimer_->start();
        };
        connect(model, &wallet::WalletModel::rowsAppended, this, rowsMoved);
        connect(model, &wallet::WalletModel::rowChanged, this, rowsMoved);
        connect(model, &wallet::WalletModel::cleared, this, &OverviewPage::scheduleRefresh);
    }

    if (controller_) {
        // A wallet appearing or going is rare and changes the whole page: fill
        // it at once, before it is shown, so its first layout already fits the
        // final texts (no stale labels, no row growing a frame late).
        connect(controller_, &WalletController::walletChanged, this, [this] {
            refreshView();
            showState();
        });
        connect(controller_, &WalletController::totalsChanged, this,
                &OverviewPage::scheduleRefresh);
        connect(controller_, &WalletController::statusChanged, this,
                &OverviewPage::scheduleRefresh);
    }

    // A price says how old it is, and the time it shows gains its date on another day,
    // so the page also re-renders as time passes under it: while every source keeps
    // failing and no balance moves, no signal would. Once a minute, rather than aiming a
    // timer at a moment, also copes with sleep and clock changes.
    auto* ageCheck = new QTimer(this);
    ageCheck->setTimerType(Qt::VeryCoarseTimer);
    ageCheck->setInterval(kAgeCheckMs);
    connect(ageCheck, &QTimer::timeout, this, &OverviewPage::scheduleRefresh);
    ageCheck->start();

    refreshView();
    showState();
}

QWidget* OverviewPage::buildEmptyState()
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
    connect(manyButton, &QPushButton::clicked, this, &OverviewPage::importManyRequested);
    auto* importButton = new QPushButton(QStringLiteral("Add wallet"), actions);
    importButton->setObjectName(QStringLiteral("primaryButton"));
    connect(importButton, &QPushButton::clicked, this, &OverviewPage::importRequested);
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

QWidget* OverviewPage::buildWalletState()
{
    auto* host = new QWidget(this);
    auto* root = new QVBoxLayout(host);
    page::applyPageLayout(root);

    // The grand total in the kit's dashboard figure (#statValue), and where
    // the prices behind it come from.
    const page::Section total = page::makeSection(host, QStringLiteral("Total balance"));
    totalLabel_ = new QLabel(dash(), total.card);
    totalLabel_->setObjectName(QStringLiteral("statValue"));
    totalLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pricesLabel_ = new QLabel(dash(), total.card);
    setTone(pricesLabel_, Tone::Muted);
    total.body->addWidget(totalLabel_);
    total.body->addWidget(pricesLabel_);
    root->addWidget(total.card);

    // One card per asset, in the display order of wallet::assets().
    auto* cards = new QHBoxLayout;
    cards->setContentsMargins(0, 0, 0, 0);
    cards->setSpacing(page::kPageSpacing);
    for (const wallet::AssetInfo& info : wallet::assets()) {
        const page::Section card = page::makeSection(host, info.symbol, assetName(info));
        auto* amount = new QLabel(dash(), card.card);
        amount->setObjectName(QStringLiteral("statValue"));
        amount->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* usd = new QLabel(dash(), card.card);
        setTone(usd, Tone::Muted);
        // "≥ $1,234.56 · 297 addresses not checked yet" wraps in a narrow
        // card instead of widening the row of cards past the window.
        usd->setWordWrap(true);
        card.body->addWidget(amount);
        card.body->addWidget(usd);
        card.body->addStretch(1); // top-aligned when a wrapped name makes a sibling taller
        cards->addWidget(card.card, 1);
        amountLabels_.append(amount);
        usdLabels_.append(usd);
    }
    root->addLayout(cards);

    const page::Section status = page::makeSection(host, QStringLiteral("Status"));
    QFormLayout* form = page::addSectionForm(status);
    // Every value label starts with text, and the chip as a chip: rows laid
    // out for an empty, unstyled label grow once the first values arrive, one
    // event-loop pass per enclosing layout, and the chip showed clipped meanwhile.
    scanLabel_ = new QLabel(status.card);
    setChip(scanLabel_, trText(QStringLiteral("Scanning…")), Tone::Accent);
    // A chip hugs its text; grown to the field's width it would paint a bar.
    scanLabel_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    form->addRow(QStringLiteral("Scan"), scanLabel_);
    const auto addValueRow = [&](const QString& label) {
        auto* value = new QLabel(dash(), status.card);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(label, value);
        return value;
    };
    walletsLabel_ = addValueRow(QStringLiteral("Wallets"));
    scannedLabel_ = addValueRow(QStringLiteral("Addresses scanned"));
    usedLabel_ = addValueRow(QStringLiteral("Used addresses"));
    fundedLabel_ = addValueRow(QStringLiteral("Funded addresses"));
    for (const wallet::NetworkInfo& network : wallet::networks())
        serverLabels_.append(addValueRow(QStringLiteral("%1 server").arg(network.name)));
    page::finishForms({form});
    root->addWidget(status.card);

    root->addStretch(1); // keeps the action bar on the bottom edge

    const page::ActionBar bar = page::makeActionBar(host);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), bar.widget);
    refreshButton_->setToolTip(
        QStringLiteral("Fetch prices and read the balances that are not followed live, now."));
    const auto tintRefresh = [button = refreshButton_] {
        page::setLabeledIcon(button, makeIcon(icon::Kind::Refresh, page::text(), 15));
    };
    tintRefresh();
    // The glyph is a pixmap in the text colour of the theme it was drawn for.
    connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed, refreshButton_,
            tintRefresh);
    connect(refreshButton_, &QPushButton::clicked, this, [this] {
        if (!controller_)
            return;
        controller_->refresh();
        Feedback::show(refreshButton_, Feedback::Type::Info,
                       trText(QStringLiteral("Refreshing prices and balances")));
        refreshButton_->setEnabled(false);
        QTimer::singleShot(kRefreshCooldownMs, refreshButton_,
                           [button = refreshButton_] { button->setEnabled(true); });
    });
    bar.right->addWidget(refreshButton_);
    root->addWidget(bar.widget);
    return host;
}

void OverviewPage::showState()
{
    const bool hasWallet = controller_ && controller_->hasWallet();
    states_->setCurrentWidget(hasWallet ? walletState_ : emptyState_);
}

void OverviewPage::scheduleRefresh()
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

void OverviewPage::refreshView()
{
    const QDate today = QDate::currentDate();
    if (!controller_ || !controller_->hasWallet())
        return;

    const wallet::WalletModel* model = controller_->model();
    const net::PriceFeed* prices = controller_->prices();

    // An account whose first row does not exist yet has not been derived (or its key
    // could not be read): no balance of that network is known.
    const QList<wallet::AccountKey> accounts = controller_->store().allAccounts();
    QSet<wallet::Network> watched;
    QSet<wallet::Network> missing;
    for (const wallet::AccountKey& account : accounts) {
        for (const wallet::NetworkInfo& network : wallet::networks()) {
            if (!account.serves(network.network))
                continue;
            watched.insert(network.network);
            if (!model || model->find(account.wallet, network.network, account.type, account.account, 0, 0) < 0)
                missing.insert(network.network);
        }
    }

    // The scan state, per network. The address limit is reported for the wallet as a
    // whole, so it holds back every figure. Figures of a network from an earlier round
    // (a server on another chain, a round that could not answer every address) say
    // since when, or, before any round answered in full, why not.
    QHash<wallet::Network, wallet::NetworkStatus> states;
    for (const wallet::NetworkInfo& network : wallet::networks())
        states.insert(network.network, controller_->networkStatus(network.network));
    const bool limit = controller_->addressLimitReached();
    const bool saved = controller_->showingSaved();
    const bool loading = controller_->loadingSaved(); // part of what the database kept
    const bool unread = controller_->hasUnreadKeys(); // a key's coins are in no figure
    const auto asOfText = [&](const wallet::NetworkStatus& st) {
        if (st.balancesAt.isValid())
            return trText(QStringLiteral("as of %1")).arg(shownTime(st.balancesAt, today));
        return st.wrongChain ? trText(QStringLiteral("server on another chain"))
                             : trText(QStringLiteral("lookups failing"));
    };
    QString staleCaption; // under the total, and on the chip for a failed refresh
    bool anyWrongChain = false;
    bool anyFailing = false;
    bool anyScanning = false;
    QString wrongChainText;
    for (const wallet::NetworkInfo& network : wallet::networks()) {
        const wallet::NetworkStatus& st = states[network.network];
        anyScanning = anyScanning || st.scanning;
        if (st.wrongChain && !anyWrongChain) {
            anyWrongChain = true;
            wrongChainText = trText(QStringLiteral("%1 endpoint answers for another network")).arg(network.name);
        }
        anyFailing = anyFailing || st.failing;
        if ((st.wrongChain || st.failing || (st.connecting && !saved && st.balancesAt.isValid())) && staleCaption.isEmpty()) {
            const QString coin = wallet::assetInfo(network.coin).symbol;
            staleCaption = st.balancesAt.isValid()
                               ? trText(QStringLiteral("%1 balances from %2")).arg(coin, shownTime(st.balancesAt, today))
                           : st.wrongChain ? wrongChainText
                                           : trText(QStringLiteral("%1 lookups failing")).arg(coin);
        }
    }

    // A card, and the total, read plain only once every balance behind them
    // has answered: a "0" for an address nobody has answered for (a scan in
    // progress, every endpoint failing) would claim the funds are
    // gone. Until then they show what is known as a lower bound ("≥"), or "—"
    // while nothing is, and say how many addresses are still unchecked; so
    // too while the address limit leaves addresses unchecked. Figures that
    // are known but may have moved since (a chain still scanning, figures from an
    // earlier round) stay plain and say so. An asset with a
    // balance but no price would make the total silently too low, so then it
    // is not shown at all.
    bool anyPrice = false;
    bool totalComplete = true;
    bool anyIncomplete = false;
    const QList<wallet::AssetInfo>& list = wallet::assets();
    for (qsizetype i = 0; i < list.size() && i < amountLabels_.size(); ++i) {
        const wallet::AssetInfo& info = list.at(i);
        const wallet::NetworkInfo& network = wallet::networkInfo(info.network);
        const wallet::NetworkStatus& st = states[info.network];
        // No wallet has a key on this chain: nothing was looked at, so no zero.
        if (!watched.contains(info.network)) {
            amountLabels_.at(i)->setText(dash());
            usdLabels_.at(i)->setText(trText(QStringLiteral("No wallet has a key on %1")).arg(network.name));
            continue;
        }
        const double price = prices ? prices->usd(info.asset) : 0.0;
        const int pending = model ? model->uncheckedOn(info.network) : 0;
        const bool unchecked = pending > 0 || missing.contains(info.network);
        const bool incomplete = unchecked || limit || loading || unread;
        QString amountText = dash();
        QString usdText = dash();
        bool hasBalance = false;
        try {
            const chain::Amount amount = model ? model->total(info.asset) : chain::Amount();
            hasBalance = !amount.isZero();
            if (!incomplete || hasBalance) {
                const QString shown = formatAmount(amount, info);
                amountText = incomplete ? atLeast(shown) : shown;
                if (price > 0) {
                    const QString usd = formatUsd(controller_->usdValue(info.asset));
                    usdText = incomplete ? atLeast(usd) : usd;
                }
            }
        } catch (const std::exception&) {
            // chain:: reports a value it cannot convert by throwing: the card
            // shows "—" and the total does not pretend to include it.
            totalComplete = false;
        }
        // Under the figure: its dollar value, then why the figure is not final
        // (what is still unchecked), then how current the known part is.
        QStringList parts;
        if (usdText != dash())
            parts << usdText;
        if (unchecked)
            parts << uncheckedNote(pending, parts.isEmpty());
        if (limit)
            parts << trText(QStringLiteral("address limit reached"));
        if (st.wrongChain || st.failing)
            parts << asOfText(st);
        else if (st.connecting && st.balancesAt.isValid())
            parts << asOfText(st); // no server to say what has moved since
        else if (!incomplete && st.scanning)
            parts << (saved ? trText(QStringLiteral("saved, being checked")) : trText(QStringLiteral("still scanning")));
        QString usdLine = parts.isEmpty() ? dash() : parts.join(QStringLiteral(" · "));
        if (!usdLine.isEmpty() && usdLine.at(0).isLower())
            usdLine[0] = usdLine.at(0).toUpper(); // a note that opens the line
        anyIncomplete = anyIncomplete || incomplete;
        amountLabels_.at(i)->setText(amountText);
        usdLabels_.at(i)->setText(usdLine);
        if (price > 0)
            anyPrice = true;
        else if (hasBalance)
            totalComplete = false;
    }

    QString totalText = dash();
    if (anyPrice && totalComplete) {
        try {
            const double total = controller_->totalUsd();
            // While nothing is known yet the total stays "—", never "$0.00".
            if (!anyIncomplete)
                totalText = formatUsd(total);
            else if (total > 0.0)
                totalText = atLeast(formatUsd(total));
        } catch (const std::exception&) {
            totalText = dash();
        }
    }
    totalLabel_->setText(totalText);

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
    if (!staleCaption.isEmpty())
        caption << staleCaption;
    else if (caption.isEmpty() && anyScanning)
        caption << trText(QStringLiteral("Still scanning"));
    caption << controller_->pricesLine();
    pricesLabel_->setText(caption.join(QStringLiteral(" · ")));

    // The controller's title-bar status, in this page's words and in its
    // order: what keeps a balance missing or stale for good comes before
    // "Scanning…", which a window cut at the limit would otherwise show for
    // ever. A network followed live over Electrum with a connection lost after the
    // scan finished is retried inside the client with nothing pending in the scanner,
    // so without a server the chip must not claim "Up to date" either. Nor may it
    // while a balance has no answer and nothing is scanning for it.
    const bool anyConnecting = std::any_of(states.cbegin(), states.cend(),
                                           [](const wallet::NetworkStatus& st) { return st.connecting; });
    if (loading)
        setChip(scanLabel_, trText(QStringLiteral("Loading saved balances…")), Tone::Accent);
    else if (anyConnecting)
        setChip(scanLabel_, trText(QStringLiteral("Connecting…")), Tone::Warning);
    else if (limit)
        setChip(scanLabel_, trText(QStringLiteral("Address limit reached")), Tone::Error);
    else if (anyWrongChain)
        setChip(scanLabel_, wrongChainText, Tone::Error);
    else if (anyFailing)
        setChip(scanLabel_, staleCaption, Tone::Warning);
    else if (anyScanning)
        setChip(scanLabel_, saved ? trText(QStringLiteral("Checking saved balances…")) : trText(QStringLiteral("Scanning…")),
                Tone::Accent);
    else if (anyIncomplete)
        setChip(scanLabel_, trText(QStringLiteral("Incomplete")), Tone::Warning);
    else
        setChip(scanLabel_, trText(QStringLiteral("Up to date")), Tone::Success);
    const int derived = model ? model->size() : 0;
    const int checked = model ? model->checkedCount() : 0;
    walletsLabel_->setText(formatCount(int(controller_->store().wallets.size())));
    // Until the servers have answered, what a row says it knows is from an earlier run.
    scannedLabel_->setText(saved ? trText(QStringLiteral("%1 saved")).arg(formatCount(derived))
                           : checked >= derived
                               ? formatCount(derived)
                               : trText(QStringLiteral("%1 of %2"))
                                     .arg(formatCount(checked), formatCount(derived)));
    usedLabel_->setText(formatCount(model ? model->usedCount() : 0));
    fundedLabel_->setText(formatCount(model ? model->fundedCount() : 0));
    const QList<wallet::NetworkInfo>& networkList = wallet::networks();
    for (qsizetype i = 0; i < networkList.size() && i < serverLabels_.size(); ++i)
        setServer(serverLabels_.at(i), states[networkList.at(i).network]);
}

} // namespace app::ui
