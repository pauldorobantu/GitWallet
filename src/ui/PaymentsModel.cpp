#include "ui/PaymentsModel.h"

#include "app/WalletController.h"
#include "ui/Figures.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/design/Style.h"
#include "wallet/History.h"
#include "wallet/Payments.h"
#include "wallet/UtxoHistory.h"
#include "wallet/WalletStore.h"

#include <QDateTime>
#include <QFont>

#include <exception>

namespace app::ui {

namespace {

QString amountText(const wallet::Payment& payment)
{
    const wallet::AssetInfo* info = wallet::findAsset(payment.asset);
    if (!info)
        return payment.asset;
    try {
        return QLatin1Char('+') + figures::formatAmount(payment.amount, *info) + QLatin1Char(' ') + info->symbol;
    } catch (const std::exception&) {
        return figures::dash();
    }
}

} // namespace

PaymentsModel::PaymentsModel(WalletController* controller, int limit, QObject* parent)
    : QAbstractTableModel(parent), controller_(controller), limit_(limit)
{
    refresh();
}

void PaymentsModel::refresh()
{
    beginResetModel();
    const int all = int(controller_->payments()->all().size());
    shown_ = limit_ > 0 ? qMin(limit_, all) : all;
    endResetModel();
}

void PaymentsModel::refreshStates()
{
    if (rowCount() > 0)
        emit dataChanged(index(0, ColState), index(rowCount() - 1, ColState));
}

void PaymentsModel::refreshLabels()
{
    if (rowCount() > 0)
        emit dataChanged(index(0, ColLabel), index(rowCount() - 1, ColLabel));
}

const wallet::Payment* PaymentsModel::at(int row) const
{
    const QList<wallet::Payment>& all = controller_->payments()->all();
    const int position = int(all.size()) - 1 - row;
    return row >= 0 && row < shown_ && position >= 0 ? &all.at(position) : nullptr;
}

int PaymentsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : shown_;
}

int PaymentsModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant PaymentsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return QAbstractTableModel::headerData(section, orientation, role);
    if (role == Qt::TextAlignmentRole && (section == ColAmount || section == ColUsd))
        return QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter));
    if (role == Qt::ToolTipRole && section == ColUsd)
        return trText(QStringLiteral("At the price when it came"));
    if (role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColWhen: return trText(QStringLiteral("When"));
    case ColAmount: return trText(QStringLiteral("Amount"));
    case ColUsd: return QStringLiteral("USD");
    case ColLabel: return trText(QStringLiteral("Label"));
    case ColWallet: return trText(QStringLiteral("Wallet"));
    case ColAddress: return trText(QStringLiteral("Address"));
    case ColState: return trText(QStringLiteral("State"));
    default: return {};
    }
}

QVariant PaymentsModel::data(const QModelIndex& index, int role) const
{
    const wallet::Payment* payment = at(index.row());
    if (!payment || index.parent().isValid())
        return {};
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case ColWhen: return QDateTime::fromMSecsSinceEpoch(payment->at).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        case ColAmount: return amountText(*payment);
        case ColUsd: return payment->usd > 0.0 ? figures::formatUsd(payment->usd) : figures::dash();
        case ColLabel: return controller_->addressLabel(payment->address);
        case ColWallet: {
            const wallet::WalletEntry* entry = controller_->store().find(payment->wallet);
            return entry ? entry->label : figures::dash();
        }
        case ColAddress:
            return payment->addresses > 1 ? trText(QStringLiteral("%1 and %2 more")).arg(payment->address).arg(payment->addresses - 1) : payment->address;
        case ColState: return stateText(*payment);
        default: return {};
        }
    case Qt::FontRole:
        if (!payment->read) {
            QFont bold;
            bold.setWeight(QFont::DemiBold);
            return bold;
        }
        return {};
    case Qt::ForegroundRole:
        if (index.column() == ColState) {
            const QString state = stateKey(*payment);
            if (state == QLatin1String("block"))
                return toneColor(Tone::Success);
            if (state == QLatin1String("pending"))
                return toneColor(Tone::Warning);
            if (state == QLatin1String("gone"))
                return toneColor(Tone::Error);
            return toneColor(Tone::Muted);
        }
        return {};
    case Qt::TextAlignmentRole:
        if (index.column() == ColAmount || index.column() == ColUsd)
            return QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter));
        return {};
    case Qt::ToolTipRole:
        if (index.column() == ColWhen && payment->whileClosed)
            return trText(QStringLiteral("It came while GitWallet was closed: this is when it was noticed"));
        if (index.column() == ColState && !payment->hasTx)
            return trText(QStringLiteral("This network has no history here yet: the balance went up by this much"));
        if (index.column() == ColState && stateKey(*payment) == QLatin1String("pending"))
            return trText(QStringLiteral("It can still be replaced: do not count it as paid yet"));
        if (index.column() == ColLabel || index.column() == ColAddress)
            return payment->address;
        return {};
    default:
        return {};
    }
}

const wallet::TxRecord* PaymentsModel::recordOf(const wallet::Payment& payment) const
{
    const wallet::AssetInfo* info = payment.hasTx ? wallet::findAsset(payment.asset) : nullptr;
    const wallet::UtxoHistory* history = info ? controller_->historyOf(info->network) : nullptr;
    const int position = history ? history->book()->find(payment.txid) : -1;
    return position >= 0 ? &history->book()->at(position) : nullptr;
}

bool PaymentsModel::proofFailed(const wallet::Payment& payment) const
{
    const wallet::AssetInfo* info = wallet::findAsset(payment.asset);
    const wallet::UtxoHistory* history = info ? controller_->historyOf(info->network) : nullptr;
    return history && history->proofFailed(payment.txid);
}

QString PaymentsModel::stateKey(const wallet::Payment& payment) const
{
    if (!payment.hasTx)
        return QStringLiteral("balance");
    const wallet::TxRecord* tx = recordOf(payment);
    if (!tx)
        return QStringLiteral("gone");
    if (tx->seen != wallet::TxSeen::Alive || (tx->height > 0 && !tx->proven && proofFailed(payment)))
        return QStringLiteral("gone");
    return tx->height > 0 && tx->proven ? QStringLiteral("block") : QStringLiteral("pending");
}

QString PaymentsModel::stateText(const wallet::Payment& payment) const
{
    if (!payment.hasTx)
        return trText(QStringLiteral("Balance went up"));
    const wallet::TxRecord* tx = recordOf(payment);
    if (!tx)
        return figures::dash();
    if (tx->seen == wallet::TxSeen::Replaced)
        return trText(QStringLiteral("Replaced"));
    if (tx->seen == wallet::TxSeen::Dropped)
        return trText(QStringLiteral("Dropped"));
    if (tx->height <= 0)
        return trText(QStringLiteral("Pending"));
    if (tx->proven) {
        const wallet::AssetInfo* info = wallet::findAsset(payment.asset);
        const wallet::UtxoHistory* history = info ? controller_->historyOf(info->network) : nullptr;
        return figures::confirmations(history ? qMax(1, history->tip() - tx->height + 1) : 1);
    }
    return proofFailed(payment) ? trText(QStringLiteral("Not verified")) : trText(QStringLiteral("Verifying"));
}

} // namespace app::ui
