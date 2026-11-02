#include "ui/AddressTableModel.h"

#include "app/WalletController.h"
#include "net/PriceFeed.h"
#include "ui/Figures.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/TableUtil.h"
#include "ui/design/Style.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QLocale>
#include <QStringList>
#include <QTimer>

#include <exception>
#include <limits>
#include <utility>

namespace app::ui {

namespace {

// Fraction digits a balance shows in the table: all 8 of BTC, ETH cut to 8 of
// its 18 (truncated, never rounded up). The tooltip shows every digit.
constexpr int kShownFraction = 8;

const wallet::AssetInfo& coinOf(const wallet::AddressRow& row)
{
    return wallet::assetInfo(wallet::networkInfo(row.network).coin);
}

double priceOf(const WalletController* controller, const wallet::AssetInfo& info)
{
    const net::PriceFeed* feed = controller ? controller->prices() : nullptr;
    return feed ? feed->usd(info.asset) : 0.0;
}

QList<double> currentPrices(const WalletController* controller)
{
    QList<double> out;
    for (const wallet::AssetInfo& info : wallet::assets())
        out << priceOf(controller, info);
    return out;
}

// "1234567.5" -> "1,234,567.5", grouped like the USD column.
QString groupThousands(const QString& plain)
{
    const qsizetype dot = plain.indexOf(QLatin1Char('.'));
    QString whole = dot < 0 ? plain : plain.left(dot);
    for (qsizetype i = whole.size() - 3; i > 0; i -= 3)
        whole.insert(i, QLatin1Char(','));
    return dot < 0 ? whole : whole + plain.mid(dot);
}

// "0.5 ETH". Dust that truncates to zero at `maxFraction` digits reads
// "<0.00000001 ETH": a bare "0 ETH" on a Funded row would look like a bug.
QString amountText(const chain::Amount& value, const wallet::AssetInfo& info, int maxFraction)
{
    const QString plain = QString::fromStdString(value.format(info.decimals, maxFraction));
    if (plain == QLatin1String("0") && !value.isZero() && maxFraction > 0)
        return QStringLiteral("<0.%1 %2")
            .arg(QStringLiteral("1").rightJustified(maxFraction, QLatin1Char('0')), info.symbol);
    return groupThousands(plain) + QLatin1Char(' ') + info.symbol;
}

// A signed amount in smallest units (an unconfirmed delta): "+0.0012 BTC", "-0.0012 BTC".
QString signedCoinText(qint64 units, const wallet::AssetInfo& coin)
{
    // Unsigned negation is defined for every value, qAbs(INT64_MIN) is not.
    const quint64 magnitude = units < 0 ? 0 - quint64(units) : quint64(units);
    const QString digits =
        QString::fromStdString(chain::Amount::fromU64(magnitude).format(coin.decimals, coin.decimals));
    return QLatin1Char(units < 0 ? '-' : '+') + groupThousands(digits) + QLatin1Char(' ') + coin.symbol;
}

QString usdText(double value)
{
    if (value > 0.0 && value < 0.01)
        return QStringLiteral("<$0.01");
    static const QLocale english(QLocale::English, QLocale::UnitedStates);
    return QLatin1Char('$') + english.toString(value, 'f', 2);
}

QString dash()
{
    return QStringLiteral("—");
}

// Until every lookup for a row has answered, a zero may only mean "no answer
// yet": nothing has answered (Checking…), or an Ethereum row got its nonce
// while its balance call failed. Such a row reads "—", never a "0" that may
// be wrong. A non-zero amount is an answer, so a funded row always shows it.
bool balancePending(const wallet::AddressRow& row)
{
    return !row.checked && row.state() != wallet::AddressState::Funded;
}

// The line a tooltip adds while a row is not fully checked.
QString pendingTip(const wallet::AddressRow& row)
{
    return row.state() == wallet::AddressState::Unknown
               ? trText(QStringLiteral("Waiting for the first scan result"))
               : trText(QStringLiteral("Some balances are still being checked"));
}

// A row's worth at the latest prices.
struct RowValue
{
    double usd = 0.0;    // assets without a price count as 0
    bool holds = false;  // any balance > 0
    bool priced = false; // every asset it holds (an empty row: its chain's coin) has a price
};

RowValue valueOf(const WalletController* controller, const wallet::AddressRow& row)
{
    RowValue v;
    v.priced = true;
    row.forEachBalance([&](wallet::Asset asset, const chain::Amount& amount) {
        const wallet::AssetInfo& info = wallet::assetInfo(asset);
        v.holds = true;
        const double price = priceOf(controller, info);
        if (price > 0.0)
            v.usd += amount.toDouble(info.decimals) * price;
        else
            v.priced = false;
    });
    if (!v.holds)
        v.priced = priceOf(controller, coinOf(row)) > 0.0;
    return v;
}

// Balance and USD sort by value. Before prices arrive a funded row is still
// worth more than an empty one, so it gets the smallest positive value; a row
// whose balance is not known yet sorts below the empty ones.
double sortValue(const WalletController* controller, const wallet::AddressRow& row)
{
    if (balancePending(row))
        return -1.0;
    const RowValue v = valueOf(controller, row);
    if (v.usd > 0.0 || !v.holds)
        return v.usd;
    return std::numeric_limits<double>::min();
}

QString balanceText(const wallet::AddressRow& row)
{
    QStringList parts;
    row.forEachBalance([&](wallet::Asset asset, const chain::Amount& amount) {
        const wallet::AssetInfo& info = wallet::assetInfo(asset);
        parts << amountText(amount, info, qMin(info.decimals, kShownFraction));
    });
    return parts.isEmpty() ? QStringLiteral("0") : parts.join(QStringLiteral(" · "));
}

// Every asset of the row's network at full precision, zeros included. A row that
// is not fully checked lists only what has answered (a zero there may be a
// failed call) and says the rest is pending.
QString balanceTip(const wallet::AddressRow& row)
{
    if (row.state() == wallet::AddressState::Unknown)
        return pendingTip(row);
    QStringList lines;
    for (const wallet::Asset asset : wallet::networkInfo(row.network).assets) {
        const wallet::AssetInfo& info = wallet::assetInfo(asset);
        const chain::Amount amount = row.amount(asset);
        if (row.checked || !amount.isZero())
            lines << amountText(amount, info, info.decimals);
    }
    if (row.isUtxo() && row.unconfirmed != 0)
        lines << trText(QStringLiteral("Unconfirmed: %1")).arg(signedCoinText(row.unconfirmed, coinOf(row)));
    if (!row.isUtxo() && (row.checked || row.nonce > 0))
        lines << trText(QStringLiteral("Transactions sent: %1")).arg(row.nonce);
    if (!row.checked)
        lines << pendingTip(row);
    return lines.join(QLatin1Char('\n'));
}

// The prices behind the USD cell, and why it is a dash (or only part of the
// row's worth) while the row is not fully checked.
QString usdTip(const WalletController* controller, const wallet::AddressRow& row)
{
    QStringList lines;
    for (const wallet::Asset asset : wallet::networkInfo(row.network).assets) {
        const wallet::AssetInfo& info = wallet::assetInfo(asset);
        const double price = priceOf(controller, info);
        lines << (price > 0.0 ? QStringLiteral("1 %1 = %2").arg(info.symbol, usdText(price))
                              : trText(QStringLiteral("1 %1: no price yet")).arg(info.symbol));
    }
    if (controller)
        lines << controller->pricesLine();
    if (!row.checked)
        lines << pendingTip(row);
    return lines.join(QLatin1Char('\n'));
}

QString typeLabel(chain::ScriptType type)
{
    switch (type) {
    case chain::ScriptType::P2WPKH:
        return trText(QStringLiteral("Native SegWit"));
    case chain::ScriptType::P2SH_P2WPKH:
        return trText(QStringLiteral("Nested SegWit"));
    case chain::ScriptType::P2PKH:
        return trText(QStringLiteral("Legacy"));
    case chain::ScriptType::EthAccount:
        return trText(QStringLiteral("Account"));
    }
    return QString();
}

// The order the types are listed in: the current Bitcoin script first,
// Ethereum last.
int typeRank(chain::ScriptType type)
{
    switch (type) {
    case chain::ScriptType::P2WPKH:
        return 0;
    case chain::ScriptType::P2SH_P2WPKH:
        return 1;
    case chain::ScriptType::P2PKH:
        return 2;
    case chain::ScriptType::EthAccount:
        return 3;
    }
    return 4;
}

// Paths sort numerically: wallet, network, type, account, change, index — so ".../0/10"
// follows ".../0/9", and each wallet's chains stay together, as an Evm address is on
// several networks by the same path.
qint64 pathKey(const wallet::AddressRow& row)
{
    const qint64 wallet = qMin<qint64>(row.wallet, 0xFFFFF);        // 20 bits
    const qint64 network = qMin<qint64>(qint64(row.network), 0x3F); // 6 bits
    const qint64 account = qMin<qint64>(row.account, 0xFF);         // 8 bits
    const qint64 index = qMin<qint64>(row.index, 0x1FFFFFF);        // 25 bits
    return (wallet << 43) | (network << 37) | (qint64(typeRank(row.type)) << 34) | (account << 26)
           | (qint64(row.change & 1u) << 25) | index;
}

// Coming in, in no block yet: it can still be replaced.
bool notInABlock(const wallet::AddressRow& row)
{
    return row.isUtxo() && row.unconfirmed > 0;
}

QString stateLabel(const wallet::AddressRow& row, bool givenOut)
{
    if (givenOut && row.state() == wallet::AddressState::Unused)
        return trText(QStringLiteral("Given out"));
    if (notInABlock(row))
        return trText(QStringLiteral("Not in a block yet"));
    switch (row.state()) {
    case wallet::AddressState::Funded:
        return trText(QStringLiteral("Funded"));
    case wallet::AddressState::Used:
        return trText(QStringLiteral("Used"));
    case wallet::AddressState::Unused:
        return trText(QStringLiteral("Unused"));
    case wallet::AddressState::Unknown:
        break;
    }
    return trText(QStringLiteral("Checking…"));
}

QString stateTip(const wallet::AddressRow& row, bool givenOut)
{
    if (givenOut && row.state() == wallet::AddressState::Unused)
        return trText(QStringLiteral("Handed to someone, and nothing has come in yet"));
    if (notInABlock(row))
        return trText(QStringLiteral("It can still be replaced: do not count it as paid yet"));
    switch (row.state()) {
    case wallet::AddressState::Funded:
        return trText(QStringLiteral("Holds a balance now"));
    case wallet::AddressState::Used:
        return balancePending(row) ? trText(QStringLiteral("Has transactions; the balance is still being checked"))
                                   : trText(QStringLiteral("Has transactions; the balance is zero"));
    case wallet::AddressState::Unused:
        return trText(QStringLiteral("No transactions yet"));
    case wallet::AddressState::Unknown:
        break;
    }
    return trText(QStringLiteral("Waiting for the first scan result"));
}

// Funded rows first when sorted descending; rows still being checked last.
int stateRank(wallet::AddressState state, bool givenOut)
{
    switch (state) {
    case wallet::AddressState::Funded:
        return 4;
    case wallet::AddressState::Used:
        return 3;
    case wallet::AddressState::Unused:
        return givenOut ? 2 : 1;
    case wallet::AddressState::Unknown:
        break;
    }
    return 0;
}

constexpr int kManyFunded = 300;

QVariant rightAligned()
{
    return QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter));
}

} // namespace

AddressTableModel::AddressTableModel(WalletController* controller, QObject* parent)
    : QAbstractTableModel(parent)
    , controller_(controller)
{
    if (!controller_)
        return;
    prices_ = currentPrices(controller_);
    refreshLabels();
    connect(controller_, &WalletController::walletChanged, this, &AddressTableModel::refreshLabels);
    connect(controller_, &WalletController::walletChanged, this, [this] { givenStale_ = true; });
    connect(controller_, &WalletController::rowGivenOut, this, [this](int row) {
        givenStale_ = true;
        if (row >= 0 && row < rows_)
            emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
    });
    connect(controller_, &WalletController::labelsChanged, this, [this] {
        if (rows_ > 0)
            emit dataChanged(index(0, ColLabel), index(rows_ - 1, ColLabel));
    });
    if (wallet::WalletModel* model = controller_->model()) {
        rows_ = model->size();
        readDust();
        connect(model, &wallet::WalletModel::rowsAppended, this, &AddressTableModel::appendRows);
        connect(model, &wallet::WalletModel::rowChanged, this, &AddressTableModel::changeRow);
        connect(model, &wallet::WalletModel::cleared, this, &AddressTableModel::resetRows);
        connect(model, &wallet::WalletModel::rowsDropped, this, &AddressTableModel::resetRows);
    }
    if (net::PriceFeed* feed = controller_->prices())
        connect(feed, &net::PriceFeed::updated, this, &AddressTableModel::refreshPrices);
    // The controller also signals totals after a price update: that covers a
    // feed it creates later, and refreshPrices() ignores repeats.
    connect(controller_, &WalletController::totalsChanged, this, &AddressTableModel::refreshPrices);
    // Headers and labels go through trText(); the language walk does not reach
    // model data, so a language switch repaints them from here.
    connect(LanguageManager::instance(), &LanguageManager::changed, this, [this] {
        emit headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);
        if (rows_ > 0)
            emit dataChanged(index(0, 0), index(rows_ - 1, ColumnCount - 1));
    });
}

int AddressTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : rows_;
}

int AddressTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant AddressTableModel::data(const QModelIndex& index, int role) const
{
    const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
    if (!model || !index.isValid() || index.parent().isValid() || index.row() >= rows_
        || index.row() >= model->size() || index.column() >= ColumnCount)
        return {};
    const wallet::AddressRow& row = model->at(index.row());
    const int column = index.column();

    // Amounts format through chain::Amount (Botan): nothing may throw out of
    // a model call into Qt's painting or sorting.
    try {
        switch (role) {
        case Qt::DisplayRole:
            switch (column) {
            case ColWallet:
                return labels_.value(row.wallet);
            case ColChain:
                return wallet::networkInfo(row.network).name;
            case ColType:
                return typeLabel(row.type);
            case ColPath:
                return row.path();
            case ColAddress:
                return row.address;
            case ColLabel:
                return controller_ ? controller_->addressLabel(row.address) : QString();
            case ColBalance:
                // A figure not every lookup has answered for is a lower bound, here as on its own page.
                return balancePending(row) ? dash() : row.checked ? balanceText(row) : figures::atLeast(balanceText(row));
            case ColUsd: {
                if (balancePending(row))
                    return dash();
                const RowValue v = valueOf(controller_, row);
                return !v.priced ? dash() : row.checked ? usdText(v.usd) : figures::atLeast(usdText(v.usd));
            }
            case ColState:
                return stateLabel(row, waitingForPayment(row));
            }
            break;
        case SortRole:
            switch (column) {
            case ColWallet:
                return labels_.value(row.wallet);
            case ColChain:
                return int(row.network);
            case ColType:
                return typeRank(row.type);
            case ColPath:
                return pathKey(row);
            case ColAddress:
                return row.address;
            case ColLabel:
                return controller_ ? controller_->addressLabel(row.address) : QString();
            case ColBalance:
            case ColUsd:
                return sortValue(controller_, row);
            case ColState:
                return stateRank(row.state(), waitingForPayment(row));
            }
            break;
        case NetworkRole:
            return int(row.network);
        case StateRole:
            return int(row.state());
        case AddressRole:
            return row.address;
        case WalletRole:
            return row.wallet;
        case GivenOutRole:
            return waitingForPayment(row);
        case kCopyRole:
            return column == ColAddress ? QVariant(row.address) : QVariant();
        case Qt::ToolTipRole:
            switch (column) {
            case ColAddress:
                return row.address;
            case ColBalance:
                return balanceTip(row);
            case ColUsd:
                return usdTip(controller_, row);
            case ColState:
                return stateTip(row, waitingForPayment(row));
            default:
                break;
            }
            break;
        case Qt::TextAlignmentRole:
            if (column == ColBalance || column == ColUsd)
                return rightAligned();
            break;
        case Qt::ForegroundRole: {
            // Values only where there is money; the state in the colour of
            // its meaning. All theme roles, so dark / light follow by itself.
            const wallet::AddressState state = row.state();
            if (column == ColBalance || column == ColUsd)
                return state == wallet::AddressState::Funded ? page::text() : page::muted();
            if (column == ColState) {
                if (notInABlock(row))
                    return toneColor(Tone::Warning);
                if (state == wallet::AddressState::Funded)
                    return toneColor(Tone::Success);
                if (waitingForPayment(row))
                    return toneColor(Tone::Accent);
                if (state != wallet::AddressState::Used)
                    return toneColor(Tone::Muted);
            }
            break;
        }
        default:
            break;
        }
    } catch (const std::exception&) {
        return {};
    }
    return {};
}

QVariant AddressTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return QAbstractTableModel::headerData(section, orientation, role);
    if (role == Qt::TextAlignmentRole)
        return (section == ColBalance || section == ColUsd) ? rightAligned() : QVariant();
    if (role == Qt::ToolTipRole && section == ColUsd && controller_)
        return controller_->pricesLine();
    if (role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColWallet:
        return trText(QStringLiteral("Wallet"));
    case ColChain:
        return trText(QStringLiteral("Chain"));
    case ColType:
        return trText(QStringLiteral("Type"));
    case ColPath:
        return trText(QStringLiteral("Path"));
    case ColAddress:
        return trText(QStringLiteral("Address"));
    case ColLabel:
        return trText(QStringLiteral("Label"));
    case ColBalance:
        return trText(QStringLiteral("Balance"));
    case ColUsd:
        return QStringLiteral("USD");
    case ColState:
        return trText(QStringLiteral("State"));
    default:
        break;
    }
    return {};
}

void AddressTableModel::appendRows()
{
    // The scanners append one row per signal, hundreds per event-loop turn.
    // The sorted proxy pays O(rows) for every insert (it shifts its mapping
    // from the insert point), so announcing each row on its own grows
    // quadratically: minutes of GUI time when a hostile server pushes a scan
    // to its row limit. The rows of one turn go out as one insert instead,
    // which the proxy places in a few runs. Until then the views keep the old
    // count: data() never reads past rows_, and changeRow() leaves a row that
    // is not announced yet to its insert.
    if (announcePosted_)
        return;
    announcePosted_ = true;
    QTimer::singleShot(0, this, [this] {
        announcePosted_ = false;
        announceRows();
    });
}

void AddressTableModel::announceRows()
{
    // The wallet model only appends, or says it dropped rows (a reset). Rows below rows_
    // never move otherwise, so every index a view holds stays valid; the views learn about
    // the new rows here, inside begin/endInsertRows.
    const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
    const int size = model ? model->size() : 0;
    if (size <= rows_)
        return;
    beginInsertRows(QModelIndex(), rows_, size - 1);
    for (int row = rows_; row < size; ++row)
        noteDust(row);
    rows_ = size;
    endInsertRows();
}

void AddressTableModel::changeRow(int row)
{
    if (row < 0)
        return;
    if (row >= rows_) {
        appendRows(); // not announced yet: the insert carries the new data
        return;
    }
    noteDust(row);
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

void AddressTableModel::resetRows()
{
    // The reset announces every row; an announceRows() still posted finds
    // nothing left to add.
    beginResetModel();
    const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
    rows_ = model ? model->size() : 0;
    readDust();
    endResetModel();
}

void AddressTableModel::refreshPrices()
{
    QList<double> now = currentPrices(controller_);
    if (now == prices_)
        return;
    prices_ = std::move(now);
    if (rows_ == 0)
        return;
    // Only a row that holds something has a figure to move. Telling the rows apart saves
    // the sorted filter from looking at all of them again; a list sorted by value pays about
    // a millisecond for each that moves, so past some hundred once costs less.
    const wallet::WalletModel* model = controller_->model();
    if (!model || model->fundedCount() > kManyFunded) {
        emit dataChanged(index(0, ColBalance), index(rows_ - 1, ColUsd));
        return;
    }
    const int rows = qMin(rows_, model->size());
    for (int row = 0; row < rows; ++row) {
        if (model->at(row).state() == wallet::AddressState::Funded)
            emit dataChanged(index(row, ColBalance), index(row, ColUsd));
    }
}

void AddressTableModel::noteDust(int row)
{
    const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
    if (model && row < model->size() && model->at(row).onlyDust())
        dust_.insert(row);
    else
        dust_.remove(row);
}

void AddressTableModel::readDust()
{
    dust_.clear();
    const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
    if (!model)
        return;
    for (int row = 0; row < rows_ && row < model->size(); ++row) {
        if (model->at(row).state() == wallet::AddressState::Funded && model->at(row).onlyDust())
            dust_.insert(row);
    }
}

bool AddressTableModel::isDust(int row) const
{
    const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
    if (!model || row >= model->size() || !dust_.contains(row))
        return false;
    const wallet::AddressRow& it = model->at(row);
    return !wasGivenOut(it) && controller_->addressLabel(it.address).isEmpty();
}

bool AddressTableModel::waitingForPayment(const wallet::AddressRow& row) const
{
    if (row.used || row.state() == wallet::AddressState::Funded)
        return false;
    return wasGivenOut(row);
}

bool AddressTableModel::wasGivenOut(const wallet::AddressRow& row) const
{
    if (row.change != 0 || !controller_)
        return false;
    if (givenStale_) {
        givenStale_ = false;
        given_.clear();
        for (const wallet::WalletEntry& entry : controller_->store().wallets) {
            for (const wallet::AccountKey& key : entry.accounts) {
                if (!key.given.isEmpty())
                    given_.insert(KeyId{key.wallet, key.account, quint8(key.type), chain::isUtxo(key.type) ? quint16(key.network) : quint16(0)}, key.given);
            }
        }
    }
    if (given_.isEmpty())
        return false;
    const auto own = given_.constFind(KeyId{row.wallet, row.account, quint8(row.type), row.isUtxo() ? quint16(row.network) : quint16(0)});
    return own != given_.cend() && own->contains(row.index);
}

void AddressTableModel::refreshLabels()
{
    labels_.clear();
    for (const wallet::WalletEntry& w : controller_->store().wallets)
        labels_.insert(w.id, w.label);
    if (rows_ > 0)
        emit dataChanged(index(0, ColWallet), index(rows_ - 1, ColWallet));
}

} // namespace app::ui
