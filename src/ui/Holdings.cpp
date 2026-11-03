#include "ui/Holdings.h"

#include "ui/TableUtil.h"

#include "app/WalletController.h"
#include "net/PriceFeed.h"
#include "ui/CoinIcon.h"
#include "ui/Figures.h"
#include "ui/HoverRowTable.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/design/Style.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QEvent>
#include <QHeaderView>
#include <QHelpEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace app::ui {

namespace {

// A scan signals per address; the figures need the state a few times a second.
constexpr int kRefreshMs = 300;

enum Column { ColName, ColPrice, ColAmount, ColValue, ColShare, ColAddresses, ColGo, ColumnCount };
enum Role {
    kSortRole = Qt::UserRole + 1,
    kSubRole,   // the line under the name
    kAssetRole, // a row of a coin
    kWalletRole, // a row of a wallet
    kHoldsRole,  // a key looks at it, and it holds something or is not known to hold nothing
    kWatchedRole, // a key looks at it
    kDustRole,    // all answered, and all of it together is less than counts for anything
};

enum class Kind { Coins, WalletsOfCoin, AssetsOfAddress };

using figures::atLeast;
using figures::dash;
using figures::formatCount;
using figures::formatUsd;

// One row of the table: the figures of a moment, so that sorting and painting read no model.
struct Line {
    QIcon icon;
    QString name;
    QString sub;
    wallet::Asset asset{};
    quint32 wallet = 0;
    Holding holding;
    QString note;
    double share = -1.0; // of what the rows come to; below 0: not known
    bool chosen = true;  // opens to something
};

class HoldingsModel : public QAbstractTableModel
{
public:
    using QAbstractTableModel::QAbstractTableModel;

    Kind kind = Kind::Coins;

    void setLines(QList<Line> lines)
    {
        if (lines.size() != lines_.size()) {
            beginResetModel();
            lines_ = std::move(lines);
            endResetModel();
            return;
        }
        lines_ = std::move(lines);
        if (!lines_.isEmpty())
            emit dataChanged(index(0, 0), index(int(lines_.size()) - 1, ColumnCount - 1));
    }
    void repaintHeaders() { emit headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1); }
    QString pricesLine; // where the prices come from and how old they are, on the columns they make
    const QList<Line>& lines() const { return lines_; }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(lines_.size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : ColumnCount; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal)
            return QAbstractTableModel::headerData(section, orientation, role);
        if (role == Qt::TextAlignmentRole && section != ColName)
            return QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter));
        if (role == Qt::ToolTipRole && section == ColAddresses)
            return trText(QStringLiteral("Addresses that hold some of it"));
        if (role == Qt::ToolTipRole && (section == ColPrice || section == ColValue))
            return pricesLine;
        if (role != Qt::DisplayRole)
            return {};
        switch (section) {
        case ColName: return kind == Kind::WalletsOfCoin ? trText(QStringLiteral("Wallet")) : trText(QStringLiteral("Coin"));
        case ColPrice: return trText(QStringLiteral("Price"));
        case ColAmount: return trText(QStringLiteral("Holdings"));
        case ColValue: return trText(QStringLiteral("Value"));
        case ColShare: return trText(QStringLiteral("Share"));
        case ColAddresses: return trText(QStringLiteral("Addresses"));
        default: return {};
        }
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= lines_.size())
            return {};
        const Line& line = lines_.at(index.row());
        const Holding& h = line.holding;
        const int column = index.column();
        switch (role) {
        case Qt::DisplayRole:
            switch (column) {
            case ColName: return line.name;
            case ColPrice: return h.price > 0.0 ? formatUsd(h.price) : dash();
            case ColAmount: return h.amountText();
            case ColValue: return h.usdText();
            case ColShare:
                if (line.share < 0.0)
                    return dash();
                // What holds something is no 0.0%.
                return line.share > 0.0 && line.share < 0.0005 ? QStringLiteral("<0.1%") : QStringLiteral("%1%").arg(line.share * 100.0, 0, 'f', 1);
            case ColAddresses: return h.holdersText();
            case ColGo: return line.chosen ? QStringLiteral("›") : QString();
            default: return {};
            }
        case Qt::DecorationRole:
            return column == ColName ? QVariant(line.icon) : QVariant();
        case kSubRole:
            return line.sub;
        case kAssetRole:
            return int(line.asset);
        case kWalletRole:
            return line.wallet;
        case kHoldsRole:
            // Only a figure that answered says "nothing": "—" is no zero. With no key there is nothing
            // to hold or to wait for.
            return h.watched && (h.holds() || !h.complete);
        case kWatchedRole:
            return h.watched;
        case kDustRole:
            return kind != Kind::AssetsOfAddress && h.watched && h.complete && h.holds() && h.amount.compare(wallet::assetInfo(h.asset).dust) < 0;
        case kSortRole:
            switch (column) {
            case ColName: return line.name;
            case ColPrice: return h.price;
            case ColShare:
            case ColValue:
                // Before prices come a holding is still worth more than none.
                return h.usd > 0.0 ? h.usd : (h.holds() ? std::numeric_limits<double>::min() : 0.0);
            case ColAmount:
                try {
                    return h.amount.toDouble(wallet::assetInfo(h.asset).decimals);
                } catch (const std::exception&) {
                    return 0.0;
                }
            case ColAddresses: return h.holders;
            default: return {};
            }
        case Qt::TextAlignmentRole:
            return column == ColName ? QVariant() : QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter));
        case Qt::ForegroundRole:
            if (column == ColName)
                return {};
            if (column == ColGo || !h.holds())
                return page::muted();
            return page::text();
        case Qt::ToolTipRole:
            if (column == ColName)
                return plainTip(line.name);
            if (column == ColAmount || column == ColValue)
                return line.note.isEmpty() ? QVariant() : QVariant(line.note);
            return {};
        default:
            return {};
        }
    }

private:
    QList<Line> lines_;
};

// The rows known to hold nothing go, and those no key looks at, unless that is all there is: a page
// with nothing on it says nothing, and a coin not yet answered for is no zero.
class HoldingsFilter : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setHide(bool hide)
    {
        if (hide == hide_)
            return;
        beginFilterChange();
        hide_ = hide;
        endFilterChange(Direction::Rows);
    }
    bool hide() const { return hide_; }
    void setHideDust(bool hide)
    {
        if (hide == hideDust_)
            return;
        hideDust_ = hide;
        linesChanged();
    }
    void linesChanged()
    {
        beginFilterChange();
        anyHolds_ = false;
        anyWatched_ = false;
        anyDust_ = false;
        const QAbstractItemModel* source = sourceModel();
        for (int row = 0; source && row < source->rowCount(); ++row) {
            anyHolds_ = anyHolds_ || holdsEnough(source->index(row, 0));
            anyDust_ = anyDust_ || source->index(row, 0).data(kDustRole).toBool();
            anyWatched_ = anyWatched_ || source->index(row, 0).data(kWatchedRole).toBool();
        }
        endFilterChange(Direction::Rows);
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override
    {
        const QAbstractItemModel* source = sourceModel();
        if (!source)
            return true;
        const QModelIndex first = source->index(row, 0, parent);
        if (holdsEnough(first))
            return true;
        const bool dust = hideDust_ && first.data(kDustRole).toBool();
        if (!hide_ && !dust)
            return true;
        if (anyHolds_)
            return false;
        // All there is being dust, a page of it says more than a page of nothing.
        if (hideDust_ && anyDust_)
            return dust;
        return first.data(kWatchedRole).toBool() || !anyWatched_;
    }

private:
    bool holdsEnough(const QModelIndex& first) const
    {
        return first.data(kHoldsRole).toBool() && !(hideDust_ && first.data(kDustRole).toBool());
    }

    bool hide_ = false;
    bool hideDust_ = false;
    bool anyHolds_ = false;
    bool anyWatched_ = false;
    bool anyDust_ = false;
};

// A mark and two lines of text in one cell.
class NameDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        // The row's band is the view's.
        constexpr int kMark = 26;
        constexpr int kMarkGap = 12;
        painter->save();
        const QRect cell = option.rect.adjusted(page::kCellPadX, 0, -page::kCellPadX, 0);
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        int textLeft = cell.left();
        if (!icon.isNull()) {
            icon.paint(painter, QRect(cell.left(), cell.center().y() - kMark / 2, kMark, kMark));
            textLeft += kMark + kMarkGap;
        }
        const QRect text(textLeft, cell.top(), cell.right() - textLeft, cell.height());
        const QString name = index.data(Qt::DisplayRole).toString();
        const QString sub = index.data(kSubRole).toString();
        QFont nameFont = option.font;
        nameFont.setWeight(QFont::DemiBold);
        const QFontMetrics nameMetrics(nameFont);
        painter->setPen(page::text());
        painter->setFont(nameFont);
        if (sub.isEmpty()) {
            painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter, nameMetrics.elidedText(name, Qt::ElideRight, text.width()));
        } else {
            QFont subFont = option.font;
            subFont.setPixelSize(qMax(10, QFontInfo(option.font).pixelSize() - 2));
            const QFontMetrics subMetrics(subFont);
            const int block = nameMetrics.height() + subMetrics.height();
            const int top = text.center().y() - block / 2;
            painter->drawText(QRect(text.left(), top, text.width(), nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                              nameMetrics.elidedText(name, Qt::ElideRight, text.width()));
            painter->setFont(subFont);
            painter->setPen(page::muted());
            painter->drawText(QRect(text.left(), top + nameMetrics.height(), text.width(), subMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                              subMetrics.elidedText(sub, Qt::ElideRight, text.width()));
        }
        painter->restore();
    }
};

// Whether the key behind each network is there, and every row of it answered.
struct Coverage {
    bool watched = false;
    bool missing = false; // an account has no rows at all: not derived yet, or its key unreadable
};

Coverage coverageOf(const WalletController* controller, wallet::Network network, quint32 wallet)
{
    Coverage out;
    const wallet::WalletModel* model = controller->model();
    for (const wallet::WalletEntry& entry : controller->store().wallets) {
        if (wallet != 0 && entry.id != wallet)
            continue;
        for (const wallet::AccountKey& account : entry.accounts) {
            if (!account.serves(network))
                continue;
            out.watched = true;
            if (model->find(account.wallet, network, account.type, account.account, 0, 0) < 0)
                out.missing = true;
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------- the figures ----

namespace {

// How current what is known of a network is. Empty: it is.
QString networkNote(const WalletController* controller, wallet::Network network, bool complete)
{
    const wallet::NetworkStatus status = controller->networkStatus(network);
    if ((status.wrongChain || status.failing || status.connecting) && status.balancesAt.isValid())
        return trText(QStringLiteral("as of %1")).arg(status.balancesAt.toLocalTime().toString(QStringLiteral("d MMM HH:mm")));
    if (status.wrongChain)
        return trText(QStringLiteral("server on another chain"));
    if (status.failing)
        return trText(QStringLiteral("lookups failing"));
    if (complete && status.scanning)
        return controller->showingSaved() ? trText(QStringLiteral("saved, being checked")) : trText(QStringLiteral("still scanning"));
    return QString();
}

QString incomingNote(const wallet::AssetInfo& coin, qint64 units)
{
    return trText(QStringLiteral("%1 not in a block yet")).arg(figures::formatAmount(chain::Amount::fromU64(quint64(units)), coin) + QLatin1Char(' ') + coin.symbol);
}

} // namespace

QStringList walletNotes(const WalletController* controller, quint32 wallet)
{
    QStringList parts;
    QStringList unpriced;
    const auto add = [&parts](const QString& part) {
        if (!part.isEmpty() && !parts.contains(part))
            parts << part;
    };
    if (controller->loadingSaved())
        add(trText(QStringLiteral("saved figures still being read")));
    for (const wallet::AssetInfo& info : wallet::assets()) {
        const Holding holding = holdingOf(controller, info.asset, wallet);
        if (holding.unread)
            add(trText(QStringLiteral("a key this version cannot read")));
        if (!holding.watched)
            continue;
        if (holding.incoming > 0)
            add(incomingNote(info, holding.incoming));
        if (holding.missing && holding.pending == 0)
            add(trText(QStringLiteral("not all checked yet")));
        if (controller->addressLimitReached())
            add(trText(QStringLiteral("address limit reached")));
        add(networkNote(controller, info.network, holding.complete));
        if (holding.holds()) {
            if (holding.price <= 0.0)
                unpriced << info.symbol;
            else
                add(controller->priceNote(info.asset));
        }
    }
    if (!unpriced.isEmpty())
        parts << trText(QStringLiteral("no price for %1")).arg(unpriced.join(QStringLiteral(", ")));
    return parts;
}

QStringList addressNotes(const WalletController* controller, const wallet::AddressRow& row)
{
    QStringList parts;
    const wallet::AssetInfo& coin = wallet::assetInfo(wallet::networkInfo(row.network).coin);
    if (row.isUtxo() && row.unconfirmed > 0)
        parts << incomingNote(coin, row.unconfirmed);
    const QString current = networkNote(controller, row.network, row.checked);
    if (!current.isEmpty())
        parts << current;
    row.forEachBalance([&](wallet::Asset asset, const chain::Amount&) {
        const QString price = controller->priceNote(asset);
        if (!price.isEmpty() && !parts.contains(price))
            parts << price;
    });
    return parts;
}

QString holdingNote(const WalletController* controller, const Holding& holding)
{
    const wallet::AssetInfo& info = wallet::assetInfo(holding.asset);
    const wallet::NetworkInfo& network = wallet::networkInfo(info.network);
    if (!holding.watched) {
        if (holding.unread)
            return trText(QStringLiteral("a key this version cannot read"));
        return (holding.oneWallet ? trText(QStringLiteral("This wallet has no key on %1")) : trText(QStringLiteral("No wallet has a key on %1"))).arg(network.name);
    }
    QStringList parts;
    if (holding.incoming > 0)
        parts << incomingNote(info, holding.incoming);
    if (controller->loadingSaved())
        parts << trText(QStringLiteral("saved figures still being read"));
    if (holding.pending > 0)
        parts << trText(holding.pending == 1 ? QStringLiteral("%1 address not checked yet") : QStringLiteral("%1 addresses not checked yet"))
                     .arg(formatCount(holding.pending));
    else if (holding.missing)
        parts << trText(QStringLiteral("not all checked yet"));
    if (holding.unread)
        parts << trText(QStringLiteral("a key this version cannot read"));
    if (controller->addressLimitReached())
        parts << trText(QStringLiteral("address limit reached"));
    const QString current = networkNote(controller, info.network, holding.complete);
    if (!current.isEmpty())
        parts << current;
    if (holding.holds() && holding.price > 0.0) {
        const QString price = controller->priceNote(holding.asset);
        if (!price.isEmpty())
            parts << price;
    }
    return parts.join(QStringLiteral(" · "));
}

Holding holdingAt(const WalletController* controller, const wallet::AddressRow& row, wallet::Asset asset)
{
    Holding h;
    const wallet::AssetInfo& info = wallet::assetInfo(asset);
    h.asset = asset;
    h.amount = row.amount(asset);
    h.watched = true;
    h.oneWallet = true;
    h.complete = row.checked;
    h.pending = row.checked ? 0 : 1;
    h.holders = h.holds() ? 1 : 0;
    h.incoming = row.isUtxo() && row.unconfirmed > 0 && asset == wallet::networkInfo(row.network).coin ? row.unconfirmed : 0;
    h.price = controller && controller->prices() ? controller->prices()->usd(asset) : 0.0;
    try {
        h.usd = h.amount.toDouble(info.decimals) * h.price;
    } catch (const std::exception&) {
        h.readable = false;
    }
    return h;
}

Holding holdingOf(const WalletController* controller, wallet::Asset asset, quint32 wallet)
{
    Holding h;
    h.asset = asset;
    h.oneWallet = wallet != 0;
    const wallet::WalletModel* model = controller ? controller->model() : nullptr;
    if (!model)
        return h;
    const wallet::AssetInfo& info = wallet::assetInfo(asset);
    const Coverage coverage = coverageOf(controller, info.network, wallet);
    h.watched = coverage.watched;
    h.pending = wallet != 0 ? model->uncheckedOn(info.network, wallet) : model->uncheckedOn(info.network);
    h.missing = coverage.missing;
    // A key of one wallet that this version cannot read says nothing of another wallet's figures.
    const wallet::WalletEntry* entry = wallet != 0 ? controller->store().find(wallet) : nullptr;
    h.unread = wallet != 0 ? (entry && !entry->keptAccounts.isEmpty()) : controller->hasUnreadKeys();
    h.complete = h.watched && h.pending == 0 && !coverage.missing && !controller->addressLimitReached() && !controller->loadingSaved() && !h.unread;
    h.holders = wallet != 0 ? model->holders(asset, wallet) : model->holders(asset);
    if (asset == wallet::networkInfo(info.network).coin)
        h.incoming = wallet != 0 ? model->incoming(info.network, wallet) : model->incoming(info.network);
    h.price = controller->prices() ? controller->prices()->usd(asset) : 0.0;
    h.amount = wallet != 0 ? model->total(asset, wallet) : model->total(asset);
    try {
        h.usd = h.amount.toDouble(info.decimals) * h.price;
    } catch (const std::exception&) {
        // chain:: reports a value it cannot convert by throwing: no figure pretends to include it.
        h.readable = false;
        h.usd = 0.0;
    }
    return h;
}

QString Holding::amountText() const
{
    if (!watched || !readable)
        return figures::dash();
    const wallet::AssetInfo& info = wallet::assetInfo(asset);
    try {
        if (!complete && !holds())
            return figures::dash();
        const QString shown = figures::formatAmount(amount, info) + QLatin1Char(' ') + info.symbol;
        return complete ? shown : figures::atLeast(shown);
    } catch (const std::exception&) {
        return figures::dash();
    }
}

QString Holding::usdText() const
{
    if (!watched || !readable || price <= 0.0 || (!complete && !holds()))
        return figures::dash();
    const QString shown = figures::formatUsd(usd);
    return complete ? shown : figures::atLeast(shown);
}

QString Holding::holdersText() const
{
    if (!watched || (!complete && holders == 0))
        return figures::dash();
    const QString shown = figures::formatCount(holders);
    return complete ? shown : figures::atLeast(shown);
}

ScopeTotal totalOf(const WalletController* controller, quint32 wallet)
{
    ScopeTotal total;
    total.known = true;
    total.complete = true;
    bool anyWatched = false;
    bool anyPrice = false;
    for (const wallet::AssetInfo& info : wallet::assets()) {
        const Holding h = holdingOf(controller, info.asset, wallet);
        if (!h.watched) {
            // A key nobody here can read holds something nobody here knows of.
            total.complete = total.complete && !h.unread;
            continue;
        }
        anyWatched = true;
        if (!h.readable || (h.holds() && h.price <= 0.0))
            total.known = false;
        anyPrice = anyPrice || h.price > 0.0;
        total.complete = total.complete && h.complete;
        total.usd += h.usd;
    }
    if (!anyWatched || !anyPrice)
        total.known = false;
    return total;
}

QString ScopeTotal::text() const
{
    // While nothing is known the total stays "—", never "$0.00".
    if (!known || (!complete && usd <= 0.0))
        return figures::dash();
    const QString shown = figures::formatUsd(usd);
    return complete ? shown : figures::atLeast(shown);
}

// ---------------------------------------------------------------- the bar ----

AllocationBar::AllocationBar(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(8);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void AllocationBar::setParts(const QList<Part>& parts)
{
    parts_.clear();
    total_ = 0.0;
    for (const Part& part : parts) {
        if (part.usd > 0.0) {
            parts_.append(part);
            total_ += part.usd;
        }
    }
    std::stable_sort(parts_.begin(), parts_.end(), [](const Part& a, const Part& b) { return a.usd > b.usd; });
    setVisible(total_ > 0.0);
    update();
}

void AllocationBar::paintEvent(QPaintEvent*)
{
    if (total_ <= 0.0)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    const qreal radius = page::corner(height() / 2.0);
    clip.addRoundedRect(QRectF(rect()), radius, radius);
    p.setClipPath(clip);
    p.setPen(Qt::NoPen);
    constexpr qreal kGap = 2.0;
    qreal x = 0.0;
    for (const Part& part : std::as_const(parts_)) {
        const qreal share = width() * part.usd / total_;
        p.setBrush(part.color);
        // No sliver of nothing: a part too thin for a gap gets none.
        p.drawRect(QRectF(x, 0, std::max(share - (share > 2 * kGap ? kGap : 0.0), 1.0), height()));
        x += share;
    }
}

bool AllocationBar::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip && total_ > 0.0) {
        const auto* help = static_cast<QHelpEvent*>(event);
        qreal x = 0.0;
        for (const Part& part : std::as_const(parts_)) {
            x += width() * part.usd / total_;
            if (help->pos().x() <= x) {
                QToolTip::showText(help->globalPos(), QStringLiteral("%1 · %2 · %3%").arg(part.name, figures::formatUsd(part.usd)).arg(part.usd / total_ * 100.0, 0, 'f', 1), this);
                return true;
            }
        }
    }
    return QWidget::event(event);
}

// ---------------------------------------------------------------- the table ----

struct HoldingsTable::Ui : QObject
{
    using QObject::QObject;

    HoldingsModel* model = nullptr;
    HoldingsFilter* filter = nullptr;
    HoverRowTableView* view = nullptr;
    QTimer* refresh = nullptr;

    Kind kind = Kind::Coins;
    quint32 wallet = 0;
    wallet::Asset asset{};
    wallet::AddressRow row;
    bool sharesKnown = false; // every figure is final and priced: what a share is of is known
};

HoldingsTable::HoldingsTable(WalletController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller)
{
    ui_ = new Ui(this);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    ui_->model = new HoldingsModel(this);
    ui_->filter = new HoldingsFilter(this);
    ui_->filter->setSourceModel(ui_->model);
    ui_->filter->setSortRole(kSortRole);
    ui_->filter->setSortCaseSensitivity(Qt::CaseInsensitive);

    auto* view = new HoverRowTableView(this);
    view->setObjectName(QStringLiteral("holdingsTable"));
    ui_->view = view;
    view->setModel(ui_->filter);
    page::configureTable(view, 52);
    view->setMouseTracking(true);
    view->setWordWrap(false);
    view->setSelectionMode(QAbstractItemView::NoSelection);
    view->setFocusPolicy(Qt::StrongFocus);
    view->setItemDelegateForColumn(ColName, new NameDelegate(view));
    QHeaderView* header = view->horizontalHeader();
    header->setSectionsClickable(true);
    header->setSortIndicatorClearable(false);
    header->setHighlightSections(false);
    header->setStretchLastSection(false);
    view->setSortingEnabled(true);
    header->setSortIndicator(ColValue, Qt::DescendingOrder);
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ColName, QHeaderView::Stretch);
    root->addWidget(view);

    ui_->refresh = new QTimer(this);
    ui_->refresh->setSingleShot(true);
    ui_->refresh->setInterval(kRefreshMs);
    connect(ui_->refresh, &QTimer::timeout, this, [this] {
        if (isVisible())
            refresh();
    });

    const auto open = [this](const QModelIndex& index) {
        const QModelIndex source = ui_->filter->mapToSource(index);
        if (!source.isValid() || source.row() >= ui_->model->lines().size())
            return;
        const Line& line = ui_->model->lines().at(source.row());
        if (!line.chosen)
            return;
        if (ui_->kind == Kind::WalletsOfCoin)
            emit walletChosen(line.wallet);
        else
            emit coinChosen(line.asset);
    };
    connect(view, &QAbstractItemView::clicked, this, open);
    connect(view, &QAbstractItemView::activated, this, open);

    connect(LanguageManager::instance(), &LanguageManager::changed, this, [this] {
        ui_->model->repaintHeaders();
        refresh();
    });
    if (controller_) {
        connect(controller_, &WalletController::totalsChanged, this, &HoldingsTable::queueRefresh);
        connect(controller_, &WalletController::statusChanged, this, &HoldingsTable::queueRefresh);
        connect(controller_, &WalletController::walletChanged, this, &HoldingsTable::queueRefresh);
    }
    applyKind();
}

void HoldingsTable::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
}

void HoldingsTable::queueRefresh()
{
    if (!ui_->refresh->isActive())
        ui_->refresh->start();
}

void HoldingsTable::opened()
{
    applyKind();
    refresh();
    ui_->filter->sort(ColValue, Qt::DescendingOrder);
    ui_->view->horizontalHeader()->setSortIndicator(ColValue, Qt::DescendingOrder);
}

void HoldingsTable::showCoins(quint32 wallet)
{
    ui_->kind = Kind::Coins;
    ui_->wallet = wallet;
    opened();
}

void HoldingsTable::showWallets(wallet::Asset asset)
{
    ui_->kind = Kind::WalletsOfCoin;
    ui_->asset = asset;
    opened();
}

void HoldingsTable::showAddress(const wallet::AddressRow& row)
{
    ui_->kind = Kind::AssetsOfAddress;
    ui_->row = row;
    opened();
}

void HoldingsTable::applyKind()
{
    ui_->model->kind = ui_->kind;
    ui_->model->repaintHeaders();
    QTableView* view = ui_->view;
    view->setColumnHidden(ColPrice, ui_->kind == Kind::WalletsOfCoin);
    view->setColumnHidden(ColAddresses, ui_->kind == Kind::AssetsOfAddress);
    view->setColumnHidden(ColGo, ui_->kind == Kind::AssetsOfAddress);
    view->viewport()->setCursor(ui_->kind == Kind::AssetsOfAddress ? Qt::ArrowCursor : Qt::PointingHandCursor);
}

void HoldingsTable::setHideZero(bool hide)
{
    ui_->filter->setHide(hide);
}

void HoldingsTable::setHideDust(bool hide)
{
    ui_->filter->setHideDust(hide);
}

bool HoldingsTable::hideZero() const
{
    return ui_->filter->hide();
}

QTableView* HoldingsTable::view() const
{
    return ui_->view;
}

int HoldingsTable::rowsShown() const
{
    return ui_->filter->rowCount();
}

int HoldingsTable::rowsHolding() const
{
    const QList<Line>& lines = ui_->model->lines();
    return int(std::count_if(lines.cbegin(), lines.cend(), [](const Line& line) { return line.holding.watched && line.holding.holds(); }));
}

bool HoldingsTable::allFinal() const
{
    const QList<Line>& lines = ui_->model->lines();
    return std::all_of(lines.cbegin(), lines.cend(), [](const Line& line) { return line.holding.watched ? line.holding.complete : !line.holding.unread; });
}

int HoldingsTable::rowsTotal() const
{
    const QList<Line>& lines = ui_->model->lines();
    return int(std::count_if(lines.cbegin(), lines.cend(), [](const Line& line) { return line.holding.watched; }));
}

QList<AllocationBar::Part> HoldingsTable::parts() const
{
    QList<AllocationBar::Part> out;
    if (ui_->kind == Kind::WalletsOfCoin || !ui_->sharesKnown)
        return out;
    for (const Line& line : ui_->model->lines())
        out.append({coinColor(wallet::assetInfo(line.asset)), line.holding.usd, line.name});
    return out;
}

void HoldingsTable::refresh()
{
    ui_->refresh->stop();
    QList<Line> lines;
    if (!controller_) {
        ui_->model->setLines(lines);
        return;
    }
    if (ui_->kind == Kind::WalletsOfCoin) {
        const wallet::AssetInfo& info = wallet::assetInfo(ui_->asset);
        for (const wallet::WalletEntry& entry : controller_->store().wallets) {
            Line line;
            line.wallet = entry.id;
            line.asset = ui_->asset;
            line.name = entry.label;
            line.sub = entry.fromKeys ? trText(QStringLiteral("Public keys")) : trText(QStringLiteral("Seed"));
            line.icon = nameIcon(entry.label);
            line.holding = holdingOf(controller_, info.asset, entry.id);
            line.chosen = line.holding.watched;
            lines.append(line);
        }
    } else if (ui_->kind == Kind::Coins) {
        for (const wallet::AssetInfo& info : wallet::assets()) {
            Line line;
            line.asset = info.asset;
            line.name = info.name;
            line.sub = info.standard.isEmpty() ? info.symbol : QStringLiteral("%1 · %2").arg(info.symbol, info.standard);
            line.icon = coinIcon(info);
            line.holding = holdingOf(controller_, info.asset, ui_->wallet);
            line.chosen = line.holding.watched;
            lines.append(line);
        }
    } else {
        // One address: as it stands now, not as it stood when it was opened.
        const wallet::WalletModel* model = controller_->model();
        const wallet::AddressRow& opened = ui_->row;
        const int position = model->find(opened.wallet, opened.network, opened.type, opened.account, opened.change, opened.index);
        if (position >= 0) {
            const wallet::AddressRow& row = model->at(position);
            for (const wallet::Asset asset : wallet::networkInfo(row.network).assets) {
                const wallet::AssetInfo& info = wallet::assetInfo(asset);
                Line line;
                line.asset = asset;
                line.name = info.name;
                line.sub = info.standard.isEmpty() ? info.symbol : QStringLiteral("%1 · %2").arg(info.symbol, info.standard);
                line.icon = coinIcon(info);
                line.chosen = false;
                line.holding = holdingAt(controller_, row, asset);
                lines.append(line);
            }
        }
    }
    // A share is of a whole: it takes every figure final, and a price for all that is held.
    double all = 0.0;
    bool every = true;
    for (Line& line : lines) {
        all += line.holding.usd;
        every = every && (line.holding.watched ? line.holding.complete && line.holding.readable && (!line.holding.holds() || line.holding.price > 0.0) : !line.holding.unread);
        // What keeps it from final can be read without hovering, under its name.
        line.note = holdingNote(controller_, line.holding);
        if (!line.note.isEmpty() && line.holding.watched)
            line.sub += QStringLiteral(" · ") + line.note;
    }
    ui_->model->pricesLine = controller_->pricesLine();
    ui_->sharesKnown = all > 0.0 && every;
    for (Line& line : lines) {
        if (ui_->sharesKnown && line.holding.watched)
            line.share = line.holding.usd / all;
    }
    ui_->model->setLines(std::move(lines));
    ui_->filter->linesChanged();
    emit refreshed();
}

} // namespace app::ui
