#include "ui/pages/OverviewPage.h"

#include "app/WalletController.h"
#include "chain/Address.h"
#include "chain/Amount.h"
#include "net/PriceFeed.h"
#include "ui/Feedback.h"
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
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
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
// How often the page checks whether the calendar day has changed under it.
constexpr int kDayCheckMs = 60 * 1000;

QString dash()
{
    return QStringLiteral("—");
}

// Fraction digits shown per asset: satoshis for BTC, the wei tail cut at
// micro-ether, cents for the dollar tokens.
int shownDecimals(wallet::Asset asset)
{
    switch (asset) {
    case wallet::Asset::BTC: return 8;
    case wallet::Asset::ETH: return 6;
    case wallet::Asset::USDT:
    case wallet::Asset::USDC: return 2;
    }
    return 2;
}

bool isDollarToken(wallet::Asset asset)
{
    return asset == wallet::Asset::USDT || asset == wallet::Asset::USDC;
}

// The card's second line: what the symbol stands for, and that the dollar
// tokens counted are the Ethereum (ERC-20) ones only.
QString assetName(wallet::Asset asset)
{
    switch (asset) {
    case wallet::Asset::BTC: return QStringLiteral("Bitcoin");
    case wallet::Asset::ETH: return QStringLiteral("Ethereum");
    case wallet::Asset::USDT: return QStringLiteral("Tether USD · ERC-20");
    case wallet::Asset::USDC: return QStringLiteral("USD Coin · ERC-20");
    }
    return QString();
}

// Amount::format() does the exact conversion (truncated, trailing zeros
// trimmed); on top of it the whole part gets thousands separators and the
// dollar tokens keep two decimals: 1234567.5 USDT reads "1,234,567.50".
// Throws whatever Amount::format() throws.
QString formatAmount(const chain::Amount& amount, const wallet::AssetInfo& info)
{
    const int shown = shownDecimals(info.asset);
    const QString plain = QString::fromStdString(amount.format(info.decimals, shown));
    // Dust that truncates to zero reads "<0.000001", as on the Addresses page:
    // a bare "0" next to "3 funded addresses" would look like a bug.
    if (plain == QLatin1String("0") && !amount.isZero() && shown > 0)
        return QStringLiteral("<0.") + QStringLiteral("1").rightJustified(shown, QLatin1Char('0'));
    const qsizetype dot = plain.indexOf(QLatin1Char('.'));
    QString whole = dot < 0 ? plain : plain.left(dot);
    QString fraction = dot < 0 ? QString() : plain.mid(dot + 1);
    if (isDollarToken(info.asset))
        fraction = fraction.leftJustified(2, QLatin1Char('0'));
    for (qsizetype i = whole.size() - 3; i > 0; i -= 3)
        whole.insert(i, QLatin1Char(','));
    return fraction.isEmpty() ? whole : whole + QLatin1Char('.') + fraction;
}

QString formatUsd(double value)
{
    if (!qIsFinite(value))
        return dash();
    if (value > 0.0 && value < 0.01)
        return QStringLiteral("<$0.01");
    return QStringLiteral("$") + QLocale(QLocale::English).toString(qMax(0.0, value), 'f', 2);
}

QString formatCount(int n)
{
    return QLocale(QLocale::English).toString(n);
}

// When the prices were fetched, in local time: the time alone while that is
// `today`, with the date once it is not. The feed keeps its last prices
// through failed refreshes (HTTP 429 is routine on the keyless API), so the
// totals can rest on yesterday's prices, and "16:39" alone would read as today.
QString priceTime(const QDateTime& updated, const QDate& today)
{
    const QDateTime local = updated.toLocalTime();
    if (local.date() == today)
        return local.toString(QStringLiteral("HH:mm"));
    return QLocale(QLocale::English).toString(local, QStringLiteral("d MMM HH:mm"));
}

bool hasBitcoinAccount(const QList<wallet::AccountKey>& accounts)
{
    return std::any_of(accounts.cbegin(), accounts.cend(),
                       [](const wallet::AccountKey& a) { return chain::isBitcoin(a.type); });
}

// A server row: its name, or a muted "Not connected" while there is none.
void setServer(QLabel* label, const QString& name)
{
    label->setText(name.isEmpty() ? trText(QStringLiteral("Not connected")) : name);
    setTone(label, name.isEmpty() ? Tone::Muted : Tone::Neutral);
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

    // A price from an earlier day shows its date, so the page also re-renders
    // when the day changes under it: while CoinGecko keeps failing and no
    // balance moves, no signal would. Comparing calendar days once a minute,
    // rather than aiming a timer at midnight, also copes with sleep and clock
    // changes.
    auto* dayCheck = new QTimer(this);
    dayCheck->setTimerType(Qt::VeryCoarseTimer);
    dayCheck->setInterval(kDayCheckMs);
    connect(dayCheck, &QTimer::timeout, this, [this] {
        if (QDate::currentDate().toJulianDay() != shownDay_)
            scheduleRefresh();
    });
    dayCheck->start();

    refreshView();
    showState();
}

QWidget* OverviewPage::buildEmptyState()
{
    QWidget* empty = page::makeEmptyState(
        this, static_cast<int>(icon::Kind::Dashboard), QStringLiteral("No wallet yet"),
        QStringLiteral("Import a seed to see every Bitcoin and Ethereum address it controls."));
    auto* importButton = new QPushButton(QStringLiteral("Import seed"), empty);
    importButton->setObjectName(QStringLiteral("primaryButton"));
    connect(importButton, &QPushButton::clicked, this, &OverviewPage::importRequested);

    // The button joins the kit's own centred column, under the subtitle. An
    // outer layout around the empty state squeezed its icon badge instead: it
    // sized the column for a one-line subtitle at the page's full width, while
    // the column wraps the subtitle at its own, narrower width.
    if (QLayout* column = empty->layout()) {
        if (auto* box = qobject_cast<QBoxLayout*>(column))
            box->addSpacing(page::kToolbarSpacing); // a little air above the action
        column->addWidget(importButton);
        column->setAlignment(importButton, Qt::AlignHCenter);
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
        const page::Section card = page::makeSection(host, info.symbol, assetName(info.asset));
        auto* amount = new QLabel(dash(), card.card);
        amount->setObjectName(QStringLiteral("statValue"));
        amount->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* usd = new QLabel(dash(), card.card);
        setTone(usd, Tone::Muted);
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
    scannedLabel_ = addValueRow(QStringLiteral("Addresses scanned"));
    usedLabel_ = addValueRow(QStringLiteral("Used addresses"));
    fundedLabel_ = addValueRow(QStringLiteral("Funded addresses"));
    electrumLabel_ = addValueRow(QStringLiteral("Electrum server"));
    ethLabel_ = addValueRow(QStringLiteral("Ethereum endpoint"));
    page::finishForms({form});
    root->addWidget(status.card);

    root->addStretch(1); // keeps the action bar on the bottom edge

    const page::ActionBar bar = page::makeActionBar(host);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), bar.widget);
    refreshButton_->setToolTip(
        QStringLiteral("Fetch prices and Ethereum balances now. Bitcoin balances update live."));
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
                       trText(QStringLiteral("Refreshing prices and Ethereum balances")));
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
    // Recorded even without a wallet: nothing on the empty state depends on
    // the day, and the day check stays quiet instead of firing every minute.
    const QDate today = QDate::currentDate();
    shownDay_ = today.toJulianDay();
    if (!controller_ || !controller_->hasWallet())
        return;

    const wallet::WalletModel* model = controller_->model();
    const net::PriceFeed* prices = controller_->prices();

    // The total is only shown when it is whole: an asset with a balance but no
    // price would make it silently too low.
    bool anyPrice = false;
    bool totalComplete = true;
    const QList<wallet::AssetInfo>& list = wallet::assets();
    for (qsizetype i = 0; i < list.size() && i < amountLabels_.size(); ++i) {
        const wallet::AssetInfo& info = list.at(i);
        const double price = prices ? prices->usd(info.symbol) : 0.0;
        QString amountText = dash();
        QString usdText = dash();
        bool hasBalance = false;
        try {
            const chain::Amount amount = model ? model->total(info.asset) : chain::Amount();
            hasBalance = !amount.isZero();
            amountText = formatAmount(amount, info);
            if (price > 0)
                usdText = formatUsd(controller_->usdValue(info.asset));
        } catch (const std::exception&) {
            // chain:: reports a value it cannot convert by throwing: the card
            // shows "—" and the total does not pretend to include it.
            totalComplete = false;
        }
        amountLabels_.at(i)->setText(amountText);
        usdLabels_.at(i)->setText(usdText);
        if (price > 0)
            anyPrice = true;
        else if (hasBalance)
            totalComplete = false;
    }

    QString totalText = dash();
    if (anyPrice && totalComplete) {
        try {
            totalText = formatUsd(controller_->totalUsd());
        } catch (const std::exception&) {
            totalText = dash();
        }
    }
    totalLabel_->setText(totalText);

    const QDateTime updated = prices ? prices->updatedAt() : QDateTime();
    pricesLabel_->setText(updated.isValid()
                              ? trText(QStringLiteral("Prices: CoinGecko · updated %1"))
                                    .arg(priceTime(updated, today))
                              : trText(QStringLiteral("Prices: CoinGecko · not loaded yet")));

    // Bitcoin balances are followed live over Electrum. A connection lost
    // after the scan finished is retried inside the client with nothing
    // pending in the scanner, so isScanning() stays false all through the
    // outage: without a server the chip must not claim "Up to date". The
    // wording matches the controller's title-bar status for that state.
    const QString electrumServer = controller_->electrumServer();
    if (electrumServer.isEmpty() && hasBitcoinAccount(controller_->store().accounts))
        setChip(scanLabel_, trText(QStringLiteral("Connecting…")), Tone::Warning);
    else if (controller_->isScanning())
        setChip(scanLabel_, trText(QStringLiteral("Scanning…")), Tone::Accent);
    else
        setChip(scanLabel_, trText(QStringLiteral("Up to date")), Tone::Success);
    const int derived = model ? model->size() : 0;
    const int checked = model ? model->checkedCount() : 0;
    scannedLabel_->setText(checked >= derived
                               ? formatCount(derived)
                               : trText(QStringLiteral("%1 of %2"))
                                     .arg(formatCount(checked), formatCount(derived)));
    usedLabel_->setText(formatCount(model ? model->usedCount() : 0));
    fundedLabel_->setText(formatCount(model ? model->fundedCount() : 0));
    setServer(electrumLabel_, electrumServer);
    setServer(ethLabel_, controller_->ethEndpoint());
}

} // namespace app::ui
