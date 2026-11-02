#include "ui/PlaceBar.h"

#include "app/WalletController.h"
#include "net/PriceFeed.h"
#include "ui/CoinIcon.h"
#include "ui/Feedback.h"
#include "ui/Figures.h"
#include "ui/Holdings.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/TableUtil.h"
#include "ui/design/Style.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <exception>
#include <functional>

namespace app::ui {

namespace {

constexpr int kMarkSize = 40;
// A scan signals per address; the line needs the state a few times a second.
constexpr int kRefreshMs = 300;

QString rootName(Place::Root root)
{
    switch (root) {
    case Place::Root::Portfolio: return trText(QStringLiteral("Portfolio"));
    case Place::Root::Wallets: return trText(QStringLiteral("Wallets"));
    case Place::Root::Addresses: return trText(QStringLiteral("Addresses"));
    }
    return QString();
}

// An address with its middle left out: both ends are what a person compares.
QString shortAddress(const QString& address)
{
    return address.size() <= 22 ? address : address.left(12) + QStringLiteral("…") + address.right(8);
}

// A line too long for the room it has loses its middle, and its tooltip has it whole: the ends of an
// address are what a person compares.
class MiddleLabel : public QLabel
{
public:
    using QLabel::QLabel;

    void setFull(const QString& text)
    {
        full_ = text;
        setToolTip(text);
        fit();
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        fit();
    }

private:
    void fit()
    {
        ensurePolished();
        const QString shown = fontMetrics().elidedText(full_, Qt::ElideMiddle, width());
        if (shown != text())
            setText(shown);
    }

    QString full_;
};

} // namespace

std::optional<wallet::Network> Place::network() const
{
    if (address)
        return address->network;
    if (asset)
        return wallet::assetInfo(*asset).network;
    return std::nullopt;
}

PlaceBar::PlaceBar(WalletController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    setObjectName(QStringLiteral("placeBar"));
    auto* margins = new QVBoxLayout(this);
    margins->setContentsMargins(page::kPageMarginX, page::kPageMarginTop, page::kPageMarginX, 0);
    margins->setSpacing(page::kToolbarSpacing);

    path_ = new QHBoxLayout;
    path_->setContentsMargins(0, 0, 0, 0);
    path_->setSpacing(4);
    margins->addLayout(path_);

    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("cardPanel"));
    auto* row = new QHBoxLayout(card);
    row->setContentsMargins(page::kSectionPadX, page::kFormVSpacing + 2, page::kSectionPadX, page::kFormVSpacing + 2);
    row->setSpacing(page::kFormHSpacing);
    mark_ = new QLabel(card);
    mark_->setFixedSize(kMarkSize, kMarkSize);
    row->addWidget(mark_, 0, Qt::AlignVCenter);
    auto* column = new QVBoxLayout;
    column->setSpacing(2);
    title_ = new MiddleLabel(card);
    title_->setObjectName(QStringLiteral("placeTitle"));
    title_->setTextFormat(Qt::PlainText);
    title_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // On a narrow window the text is cut, never a button.
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    detail_ = new QLabel(card);
    detail_->setObjectName(QStringLiteral("placeDetail"));
    detail_->setTextFormat(Qt::PlainText);
    detail_->setWordWrap(true);
    detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setTone(detail_, Tone::Muted);
    column->addWidget(title_);
    column->addWidget(detail_);
    row->addLayout(column, 1);

    copy_ = new QPushButton(QStringLiteral("Copy address"), card);
    explorer_ = new QPushButton(QStringLiteral("Open in explorer"), card);
    explorer_->setToolTip(trText(QStringLiteral("Opens the address on a public block explorer, in your browser")));
    label_ = new QPushButton(QStringLiteral("Label"), card);
    label_->setToolTip(trText(QStringLiteral("A name for this address that only this PC knows")));
    qr_ = new QPushButton(QStringLiteral("Show QR"), card);
    receive_ = new QPushButton(QStringLiteral("Receive"), card);
    receive_->setObjectName(QStringLiteral("primaryButton"));
    for (QPushButton* button : {copy_, explorer_, label_, qr_, receive_})
        row->addWidget(button, 0, Qt::AlignVCenter);
    margins->addWidget(card);

    tabs_ = new QTabBar(this);
    tabs_->setObjectName(QStringLiteral("placeTabs"));
    tabs_->setExpanding(false);
    tabs_->setDrawBase(false);
    margins->addWidget(tabs_);

    connect(copy_, &QPushButton::clicked, this, [this] {
        if (!place_.address)
            return;
        // What may go to someone who is to pay: what the key gives, not what is kept.
        const QString address = controller_ ? controller_->checkedAddress(*place_.address) : QString();
        if (address.isEmpty())
            Feedback::show(copy_, Feedback::Type::Error, trText(QStringLiteral("What is kept for this address is not what the key of this wallet gives, so it is not offered.")));
        else
            copyTextToClipboard(copy_, address, trText(QStringLiteral("Address copied")));
    });
    connect(explorer_, &QPushButton::clicked, this, [this] {
        if (!place_.address)
            return;
        QString url = wallet::networkInfo(place_.address->network).addressUrl;
        QDesktopServices::openUrl(QUrl(url.replace(QLatin1String("{id}"), place_.address->address)));
    });
    connect(label_, &QPushButton::clicked, this, [this] {
        if (place_.address)
            emit labelRequested(place_.address->address);
    });
    connect(qr_, &QPushButton::clicked, this, [this] { emit showAddressQr(place_); });
    connect(receive_, &QPushButton::clicked, this, [this] { emit receiveRequested(place_); });
    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < tabOrder_.size())
            emit tabChosen(tabOrder_.at(index));
    });

    refresh_ = new QTimer(this);
    refresh_->setSingleShot(true);
    refresh_->setInterval(kRefreshMs);
    connect(refresh_, &QTimer::timeout, this, [this] {
        if (isVisible())
            refresh();
    });
    const auto queue = [this] {
        if (!refresh_->isActive())
            refresh_->start();
    };
    if (controller_) {
        connect(controller_, &WalletController::totalsChanged, this, queue);
        connect(controller_, &WalletController::statusChanged, this, queue);
        connect(controller_, &WalletController::labelsChanged, this, queue);
        connect(controller_, &WalletController::walletChanged, this, queue);
    }
    connect(LanguageManager::instance(), &LanguageManager::changed, this, [this] {
        if (!isVisible())
            return;
        const Place::Tab now = tab();
        rebuildPath();
        rebuildTabs(now);
        refresh();
    });
    hide();
}

void PlaceBar::showPlace(const Place& place, Place::Tab tab)
{
    place_ = place;
    rebuildPath();
    rebuildTabs(tab);
    refresh();
    QWidget::show();
}

Place::Tab PlaceBar::tab() const
{
    const int index = tabs_->currentIndex();
    return index >= 0 && index < tabOrder_.size() ? tabOrder_.at(index) : Place::Tab::Holdings;
}

QStringList PlaceBar::pathTexts() const
{
    QStringList out;
    for (int i = 0; i < path_->count(); ++i) {
        const QWidget* widget = path_->itemAt(i)->widget();
        if (const auto* button = qobject_cast<const QPushButton*>(widget))
            out << button->text();
        else if (const auto* label = qobject_cast<const QLabel*>(widget); label && label->objectName() == QLatin1String("pathHere"))
            out << label->text();
    }
    return out;
}

void PlaceBar::addStep(const QString& text, const std::function<void()>& open)
{
    if (path_->count() > 0) {
        auto* gap = new QLabel(QStringLiteral("›"), this);
        setTone(gap, Tone::Muted);
        path_->addWidget(gap);
    }
    if (!open) {
        auto* here = new QLabel(text, this);
        here->setObjectName(QStringLiteral("pathHere"));
        here->setTextFormat(Qt::PlainText);
        path_->addWidget(here);
        return;
    }
    auto* link = new QPushButton(text, this);
    link->setObjectName(QStringLiteral("pathLink"));
    link->setCursor(Qt::PointingHandCursor);
    link->setFlat(true);
    connect(link, &QPushButton::clicked, this, open);
    path_->addWidget(link);
}

void PlaceBar::rebuildPath()
{
    // Later: the click that got here may still be on its way through one of them.
    while (QLayoutItem* item = path_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    const Place place = place_;
    addStep(rootName(place.root), [this, root = place.root] { emit rootChosen(root); });

    // The steps there are, in the order this way down went through them; the last is where the reader is.
    struct Step {
        QString text;
        Place to;
    };
    QList<Step> steps;
    const auto walletName = [this](quint32 id) {
        const wallet::WalletEntry* entry = controller_ ? controller_->store().find(id) : nullptr;
        return entry ? entry->label : QString();
    };
    Place walk;
    walk.root = place.root;
    walk.onlyWallet = place.onlyWallet;
    const auto addCoin = [&] {
        if (!place.asset)
            return;
        walk.asset = place.asset;
        steps.append({wallet::assetInfo(*place.asset).name, walk});
    };
    const auto addWallet = [&] {
        if (place.wallet == 0)
            return;
        walk.wallet = place.wallet;
        // Still part of what the place is, with or without a step of its own.
        if (!place.onlyWallet)
            steps.append({walletName(place.wallet), walk});
        else if (!steps.isEmpty())
            steps.last().to = walk;
    };
    if (place.root == Place::Root::Wallets) {
        addWallet();
        addCoin();
    } else {
        addCoin();
        addWallet();
    }
    if (place.address) {
        walk.address = place.address;
        const QString label = controller_ ? controller_->addressLabel(place.address->address) : QString();
        steps.append({label.isEmpty() ? shortAddress(place.address->address) : label, walk});
    }
    for (qsizetype i = 0; i < steps.size(); ++i) {
        if (i + 1 == steps.size())
            addStep(steps.at(i).text, nullptr);
        else
            addStep(steps.at(i).text, [this, to = steps.at(i).to] { emit placeChosen(to); });
    }
    path_->addStretch(1);
}

void PlaceBar::rebuildTabs(Place::Tab tab)
{
    QStringList names;
    QList<Place::Tab> order;
    if (place_.hasHoldings()) {
        QString name = trText(QStringLiteral("Coins"));
        if (place_.address)
            name = trText(QStringLiteral("Assets"));
        else if (place_.asset)
            name = trText(QStringLiteral("Wallets"));
        names << name;
        order.append(Place::Tab::Holdings);
    }
    if (place_.hasAddressesTab()) {
        names << trText(QStringLiteral("Addresses"));
        order.append(Place::Tab::Addresses);
    }
    names << trText(QStringLiteral("History"));
    order.append(Place::Tab::History);

    const QSignalBlocker quiet(tabs_);
    // The same tabs stay, as this may be a click on one of them on its way through.
    QStringList now;
    for (int i = 0; i < tabs_->count(); ++i)
        now << tabs_->tabText(i);
    if (now != names) {
        while (tabs_->count() > 0)
            tabs_->removeTab(0);
        for (const QString& name : std::as_const(names))
            tabs_->addTab(name);
    }
    tabOrder_ = order;
    const qsizetype at = tabOrder_.indexOf(tab);
    tabs_->setCurrentIndex(at >= 0 ? int(at) : 0);
}

void PlaceBar::refresh()
{
    refresh_->stop();
    if (!controller_)
        return;
    const bool atAddress = place_.address.has_value();
    copy_->setVisible(atAddress);
    explorer_->setVisible(atAddress);
    label_->setVisible(atAddress);
    qr_->setVisible(atAddress);
    receive_->setVisible(!atAddress);

    const wallet::WalletEntry* entry = place_.wallet != 0 ? controller_->store().find(place_.wallet) : nullptr;
    QIcon icon;
    QString title;
    QStringList detail;
    if (atAddress) {
        const wallet::AddressRow& opened = *place_.address;
        const wallet::WalletModel* model = controller_->model();
        const int position = model->find(opened.wallet, opened.network, opened.type, opened.account, opened.change, opened.index);
        const wallet::AddressRow row = position >= 0 ? model->at(position) : opened;
        const QString label = controller_->addressLabel(row.address);
        icon = coinIcon(wallet::assetInfo(wallet::networkInfo(row.network).coin));
        title = label.isEmpty() ? row.address : QStringLiteral("%1 · %2").arg(label, row.address);
        detail << row.path();
        if (const wallet::WalletEntry* own = controller_->store().find(row.wallet))
            detail << own->label;
        try {
            double usd = 0.0;
            bool priced = true;
            for (const wallet::Asset asset : wallet::networkInfo(row.network).assets) {
                const wallet::AssetInfo& info = wallet::assetInfo(asset);
                const chain::Amount amount = row.amount(asset);
                if (amount.isZero() && asset != wallet::networkInfo(row.network).coin)
                    continue;
                detail << figures::formatAmount(amount, info) + QLatin1Char(' ') + info.symbol;
                const double price = controller_->prices()->usd(asset);
                priced = priced && (price > 0.0 || amount.isZero());
                usd += amount.toDouble(info.decimals) * price;
            }
            if (priced)
                detail << figures::formatUsd(usd);
        } catch (const std::exception&) {
        }
    } else if (place_.asset) {
        const wallet::AssetInfo& info = wallet::assetInfo(*place_.asset);
        const Holding holding = holdingOf(controller_, *place_.asset, place_.wallet);
        icon = coinIcon(info);
        title = holding.amountText();
        detail << holding.usdText();
        if (holding.price > 0.0)
            detail << QStringLiteral("1 %1 = %2").arg(info.symbol, figures::formatUsd(holding.price));
        if (entry && !place_.onlyWallet)
            detail << entry->label;
        detail << trText(holding.holders == 1 ? QStringLiteral("%1 address holds it") : QStringLiteral("%1 addresses hold it")).arg(figures::formatCount(holding.holders));
    } else if (entry) {
        icon = nameIcon(entry->label);
        title = totalOf(controller_, entry->id).text();
        detail << entry->label;
        const wallet::WalletCounts counts = controller_->model()->counts(entry->id);
        detail << trText(QStringLiteral("%1 funded of %2 addresses")).arg(figures::formatCount(counts.funded), figures::formatCount(counts.rows));
    }
    mark_->setPixmap(icon.pixmap(QSize(kMarkSize, kMarkSize)));
    static_cast<MiddleLabel*>(title_)->setFull(title);
    detail_->setText(detail.join(QStringLiteral(" · ")));
}

} // namespace app::ui
