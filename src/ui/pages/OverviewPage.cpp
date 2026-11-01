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
// Bits of a row's pending mask: bit i stands for wallet::assets()[i].
constexpr qsizetype kMaskBits = 32;

using figures::atLeast;
using figures::dash;
using figures::formatAmount;
using figures::formatCount;
using figures::formatUsd;

// BTC lives on Bitcoin rows; ETH and the ERC-20 tokens on Ethereum rows.
bool onChain(const wallet::AddressRow& row, const wallet::AssetInfo& info)
{
    return (info.asset == wallet::Asset::BTC) == row.isBitcoin();
}

// The zero this row holds in `info` may only mean "no answer yet": the row is
// not fully checked (nothing answered, or an Ethereum balance call failed) and
// holds none of it. A non-zero amount is an answer, from this pass or an
// earlier one. The per-asset form of the Addresses table's balancePending():
// an Ethereum row whose ETH balance arrived while its USDT call failed is
// still unknown for USDT.
bool balancePending(const wallet::AddressRow& row, const wallet::AssetInfo& info)
{
    if (row.checked || !onChain(row, info))
        return false;
    // A Bitcoin row holds BTC only, and state() reads it without building an
    // Amount (a BigInt): this runs inside the model's signals.
    if (row.isBitcoin())
        return row.state() != wallet::AddressState::Funded;
    return row.amount(info.asset).isZero();
}

quint32 pendingMask(const wallet::AddressRow& row)
{
    quint32 mask = 0;
    if (row.checked)
        return mask; // nearly every row once the scan is through
    const QList<wallet::AssetInfo>& list = wallet::assets();
    for (qsizetype i = 0; i < list.size() && i < kMaskBits; ++i) {
        if (balancePending(row, list.at(i)))
            mask |= quint32(1) << i;
    }
    return mask;
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

    pendingPerAsset_.fill(0, qMin(wallet::assets().size(), kMaskBits));
    if (wallet::WalletModel* model = controller_ ? controller_->model() : nullptr) {
        // Which balances still lack an answer, followed row by row: a scan
        // signals per address, and a recount would walk every row each time.
        connect(model, &wallet::WalletModel::rowsAppended, this, [this] {
            if (syncPending())
                scheduleRefresh();
        });
        connect(model, &wallet::WalletModel::rowChanged, this, &OverviewPage::notePendingRow);
        connect(model, &wallet::WalletModel::cleared, this, &OverviewPage::resetPending);
        syncPending(); // rows the model already holds
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
        QStringLiteral("Add your seeds, one by one or a whole list, or just their public keys, to see "
                       "every Bitcoin and Ethereum address they control."));
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
        const page::Section card = page::makeSection(host, info.symbol, assetName(info.asset));
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

void OverviewPage::resetPending()
{
    rowPending_.clear();
    pendingPerAsset_.fill(0);
    pendingRows_ = 0;
    syncPending(); // whatever the model holds (nothing, right after a clear)
    scheduleRefresh();
}

bool OverviewPage::syncPending()
{
    const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
    const qsizetype size = model ? model->size() : 0;
    bool moved = false;
    if (size < rowPending_.size()) {
        // Rows only go away through cleared(); should that ever be missed,
        // the counts start over rather than describe rows that are gone.
        rowPending_.clear();
        pendingPerAsset_.fill(0);
        pendingRows_ = 0;
        moved = true;
    }
    // From the model's size, not a signal's range: a listener that upserts
    // from inside another one's slot makes appends arrive out of order. Reserved
    // for a burst only: reserve() allocates exactly what it is asked for, and
    // this runs once per row during a scan, which would copy the list every time.
    if (size - rowPending_.size() > 1)
        rowPending_.reserve(size);
    for (qsizetype i = rowPending_.size(); i < size; ++i) {
        const quint32 mask = pendingMask(model->at(int(i)));
        rowPending_.append(mask);
        if (mask != 0) {
            countPending(mask, +1);
            moved = true;
        }
    }
    return moved;
}

void OverviewPage::notePendingRow(int row)
{
    const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
    if (!model || row < 0 || row >= model->size())
        return;
    if (row >= rowPending_.size()) {
        // Not counted yet: appending reads its current state.
        if (syncPending())
            scheduleRefresh();
        return;
    }
    const quint32 now = pendingMask(model->at(row));
    const quint32 before = rowPending_.at(row);
    if (now == before)
        return;
    countPending(before, -1);
    countPending(now, +1);
    rowPending_[row] = now;
    scheduleRefresh();
}

void OverviewPage::countPending(quint32 mask, int sign)
{
    if (mask == 0)
        return;
    pendingRows_ += sign;
    for (qsizetype i = 0; i < pendingPerAsset_.size(); ++i) {
        if (mask & (quint32(1) << i))
            pendingPerAsset_[i] += sign;
    }
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
    syncPending(); // a no-op while the model's signals keep the counts current

    // Only xpubs are stored, so every start begins with an empty model. An
    // account whose first row does not exist yet has not been derived (or its
    // key could not be read): no balance of its chain is known.
    const QList<wallet::AccountKey> accounts = controller_->store().allAccounts();
    bool btcMissing = false;
    bool ethMissing = false;
    bool btcWatched = false;
    bool ethWatched = false;
    for (const wallet::AccountKey& account : accounts) {
        (chain::isBitcoin(account.type) ? btcWatched : ethWatched) = true;
        if (model && model->find(account.wallet, account.type, account.account, 0, 0) >= 0)
            continue;
        (chain::isBitcoin(account.type) ? btcMissing : ethMissing) = true;
    }

    // The scan state, per chain where the controller tells it apart. The
    // address limit is reported for the wallet as a whole, so it holds back
    // every figure. The Ethereum figures (ETH, USDT, USDC) are from an earlier
    // pass while the endpoint is off mainnet or the last refresh could not
    // answer every address: they say since when, or, before any pass answered
    // in full, why not.
    const bool btcScanning = controller_->btcScanning();
    const bool ethScanning = controller_->ethScanning();
    const bool limit = controller_->addressLimitReached();
    const bool wrongChain = controller_->ethWrongChain();
    const bool ethFailing = controller_->ethRefreshFailing();
    const bool ethStale = wrongChain || ethFailing;
    const QDateTime ethAt = controller_->ethBalancesAt();
    QString ethAsOf;    // on the Ethereum cards
    QString ethCaption; // under the total, and on the chip for a failed refresh
    if (ethAt.isValid()) {
        ethAsOf = trText(QStringLiteral("as of %1")).arg(shownTime(ethAt, today));
        ethCaption = trText(QStringLiteral("ETH balances from %1")).arg(shownTime(ethAt, today));
    } else if (wrongChain) {
        ethAsOf = trText(QStringLiteral("endpoint not on mainnet"));
        ethCaption = trText(QStringLiteral("Ethereum endpoint not on mainnet"));
    } else {
        ethAsOf = trText(QStringLiteral("lookups failing"));
        ethCaption = trText(QStringLiteral("ETH lookups failing"));
    }

    // A card, and the total, read plain only once every balance behind them
    // has answered: a "0" for an address nobody has answered for (a scan in
    // progress, every Ethereum endpoint failing) would claim the funds are
    // gone. Until then they show what is known as a lower bound ("≥"), or "—"
    // while nothing is, and say how many addresses are still unchecked; so
    // too while the address limit leaves addresses unchecked. Figures that
    // are known but may have moved since (a chain still scanning, Ethereum
    // figures from an earlier pass) stay plain and say so. An asset with a
    // balance but no price would make the total silently too low, so then it
    // is not shown at all.
    bool anyPrice = false;
    bool totalComplete = true;
    bool anyIncomplete = false;
    const QList<wallet::AssetInfo>& list = wallet::assets();
    for (qsizetype i = 0; i < list.size() && i < amountLabels_.size(); ++i) {
        const wallet::AssetInfo& info = list.at(i);
        const bool bitcoin = info.asset == wallet::Asset::BTC;
        // No wallet has a key on this chain: nothing was looked at, so no zero.
        if (!(bitcoin ? btcWatched : ethWatched)) {
            amountLabels_.at(i)->setText(dash());
            usdLabels_.at(i)->setText(bitcoin ? trText(QStringLiteral("No wallet has a Bitcoin key"))
                                              : trText(QStringLiteral("No wallet has an Ethereum key")));
            continue;
        }
        const double price = prices ? prices->usd(info.symbol) : 0.0;
        const int pending = pendingPerAsset_.value(i);
        const bool unchecked = pending > 0 || (bitcoin ? btcMissing : ethMissing);
        const bool incomplete = unchecked || limit;
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
        if (!bitcoin && ethStale)
            parts << ethAsOf;
        else if (!incomplete && (bitcoin ? btcScanning : ethScanning))
            parts << trText(QStringLiteral("still scanning"));
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

    // Under the total: why it is not final yet, if it is not, then the prices.
    // While a scan runs with every row answered (a Bitcoin balance still on
    // its way), the figure is whole so far but may still move.
    QStringList caption;
    if (pendingRows_ > 0)
        caption << uncheckedNote(pendingRows_, true);
    else if (btcMissing || ethMissing)
        caption << trText(QStringLiteral("Not all accounts checked yet"));
    if (limit)
        caption << trText(QStringLiteral("Address limit reached"));
    if (ethStale)
        caption << ethCaption;
    else if (caption.isEmpty() && (btcScanning || ethScanning))
        caption << trText(QStringLiteral("Still scanning"));
    const QDateTime updated = prices ? prices->updatedAt() : QDateTime();
    caption << (updated.isValid() ? trText(QStringLiteral("Prices: CoinGecko · updated %1"))
                                        .arg(shownTime(updated, today))
                                  : trText(QStringLiteral("Prices: CoinGecko · not loaded yet")));
    pricesLabel_->setText(caption.join(QStringLiteral(" · ")));

    // The controller's title-bar status, in this page's words and in its
    // order: what keeps a balance missing or stale for good comes before
    // "Scanning…", which a window cut at the limit would otherwise show for
    // ever. Bitcoin balances are followed live over Electrum, and a
    // connection lost after the scan finished is retried inside the client
    // with nothing pending in the scanner, so without a server the chip must
    // not claim "Up to date" either. Nor may it while a balance has no answer
    // and nothing is scanning for it.
    const QString electrumServer = controller_->electrumServer();
    if (electrumServer.isEmpty() && hasBitcoinAccount(accounts))
        setChip(scanLabel_, trText(QStringLiteral("Connecting…")), Tone::Warning);
    else if (limit)
        setChip(scanLabel_, trText(QStringLiteral("Address limit reached")), Tone::Error);
    else if (wrongChain)
        setChip(scanLabel_, trText(QStringLiteral("Ethereum endpoint not on mainnet")), Tone::Error);
    else if (ethFailing)
        setChip(scanLabel_, ethCaption, Tone::Warning);
    else if (btcScanning || ethScanning)
        setChip(scanLabel_, trText(QStringLiteral("Scanning…")), Tone::Accent);
    else if (anyIncomplete)
        setChip(scanLabel_, trText(QStringLiteral("Incomplete")), Tone::Warning);
    else
        setChip(scanLabel_, trText(QStringLiteral("Up to date")), Tone::Success);
    const int derived = model ? model->size() : 0;
    const int checked = model ? model->checkedCount() : 0;
    walletsLabel_->setText(formatCount(int(controller_->store().wallets.size())));
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
