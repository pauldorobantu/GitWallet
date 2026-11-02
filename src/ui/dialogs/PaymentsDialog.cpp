#include "ui/dialogs/PaymentsDialog.h"

#include "app/WalletController.h"
#include "ui/DialogChrome.h"
#include "ui/Figures.h"
#include "ui/HoverRowTable.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/design/Style.h"
#include "wallet/History.h"
#include "wallet/Payments.h"
#include "wallet/UtxoHistory.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QAbstractTableModel>
#include <QDateTime>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLocale>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <exception>

namespace app::ui {

namespace {

enum Column { ColWhen, ColAmount, ColUsd, ColWallet, ColAddress, ColState, ColumnCount };

class PaymentsModel : public QAbstractTableModel
{
public:
    explicit PaymentsModel(WalletController* controller, QObject* parent) : QAbstractTableModel(parent), controller_(controller) {}

    // The latest first: a row's place moves with each that comes, so it is all told again.
    void refresh()
    {
        beginResetModel();
        endResetModel();
    }
    // How a payment stands now can move with nothing new having come.
    void refreshStates()
    {
        if (rowCount() > 0)
            emit dataChanged(index(0, ColState), index(rowCount() - 1, ColState));
    }
    const wallet::Payment* at(int row) const
    {
        const QList<wallet::Payment>& all = controller_->payments()->all();
        return row >= 0 && row < all.size() ? &all.at(all.size() - 1 - row) : nullptr;
    }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(controller_->payments()->all().size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : ColumnCount; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
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
        case ColWallet: return trText(QStringLiteral("Wallet"));
        case ColAddress: return trText(QStringLiteral("Address"));
        case ColState: return trText(QStringLiteral("State"));
        default: return {};
        }
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        const wallet::Payment* payment = at(index.row());
        if (!payment || index.parent().isValid())
            return {};
        switch (role) {
        case Qt::DisplayRole:
            switch (index.column()) {
            case ColWhen: {
                static const QLocale english(QLocale::English, QLocale::UnitedStates);
                return english.toString(QDateTime::fromMSecsSinceEpoch(payment->at), QStringLiteral("d MMM HH:mm"));
            }
            case ColAmount: return amountText(*payment);
            case ColUsd: return payment->usd > 0.0 ? figures::formatUsd(payment->usd) : figures::dash();
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
            return {};
        default:
            return {};
        }
    }

private:
    static QString amountText(const wallet::Payment& payment)
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

    const wallet::TxRecord* recordOf(const wallet::Payment& payment) const
    {
        const wallet::AssetInfo* info = payment.hasTx ? wallet::findAsset(payment.asset) : nullptr;
        const wallet::UtxoHistory* history = info ? controller_->historyOf(info->network) : nullptr;
        const int position = history ? history->book()->find(payment.txid) : -1;
        return position >= 0 ? &history->book()->at(position) : nullptr;
    }

    QString stateKey(const wallet::Payment& payment) const
    {
        if (!payment.hasTx)
            return QStringLiteral("balance");
        const wallet::TxRecord* tx = recordOf(payment);
        if (!tx)
            return QStringLiteral("gone");
        if (tx->seen != wallet::TxSeen::Alive)
            return QStringLiteral("gone");
        return tx->height > 0 ? QStringLiteral("block") : QStringLiteral("pending");
    }

    QString stateText(const wallet::Payment& payment) const
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
        return tx->height > 0 ? trText(QStringLiteral("In a block")) : trText(QStringLiteral("Not in a block yet"));
    }

    WalletController* controller_ = nullptr;
};

} // namespace

PaymentsDialog::PaymentsDialog(WalletController* controller, QWidget* parent)
    : QDialog(parent), controller_(controller)
{
    setObjectName(QStringLiteral("paymentsDialog"));
    setWindowTitle(trText(QStringLiteral("Payments received")));
    applyDialogChrome(this);
    auto* root = new QVBoxLayout(this);
    page::applyDialogLayout(root);

    auto* model = new PaymentsModel(controller_, this);
    auto* view = new HoverRowTableView(this);
    view_ = view;
    view->setObjectName(QStringLiteral("paymentsTable"));
    view->setModel(model);
    page::configureTable(view);
    view->setMouseTracking(true);
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideMiddle);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    QHeaderView* header = view->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ColAddress, QHeaderView::Stretch);
    root->addWidget(view, 1);

    nothing_ = page::makeEmptyState(this, int(icon::Kind::Bell), QStringLiteral("No payment yet"),
                                    QStringLiteral("What comes in from now on shows here. What came before is in History."));
    root->addWidget(nothing_, 1);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(page::kToolbarSpacing);
    buttons->addStretch(1);
    open_ = new QPushButton(QStringLiteral("Open address"), this);
    open_->setObjectName(QStringLiteral("paymentsOpen"));
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    close->setObjectName(QStringLiteral("primaryButton"));
    close->setDefault(true);
    buttons->addWidget(open_);
    buttons->addWidget(close);
    root->addLayout(buttons);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(open_, &QPushButton::clicked, this, &PaymentsDialog::openChosen);
    connect(view, &QAbstractItemView::activated, this, &PaymentsDialog::openChosen);
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &PaymentsDialog::updateState);
    connect(controller_->payments(), &wallet::PaymentBook::changed, this, [this, model] {
        const QModelIndexList chosen = view_->selectionModel()->selectedRows();
        const qint64 kept = chosen.isEmpty() || !model->at(chosen.first().row()) ? 0 : model->at(chosen.first().row())->id;
        model->refresh();
        for (int row = 0; kept != 0 && row < model->rowCount(); ++row) {
            if (model->at(row)->id == kept) {
                view_->selectRow(row);
                break;
            }
        }
        updateState();
    });
    connect(controller_, &WalletController::historyProgress, model, &PaymentsModel::refreshStates);

    page::fitDialog(this, page::DialogSize::Wide);
    updateState();
}

void PaymentsDialog::updateState()
{
    const bool any = view_->model()->rowCount() > 0;
    view_->setVisible(any);
    nothing_->setVisible(!any);
    open_->setEnabled(view_->selectionModel()->hasSelection());
}

void PaymentsDialog::openChosen()
{
    const QModelIndexList chosen = view_->selectionModel()->selectedRows();
    const auto* model = static_cast<const PaymentsModel*>(view_->model());
    const wallet::Payment* payment = chosen.isEmpty() ? nullptr : model->at(chosen.first().row());
    const wallet::AssetInfo* info = payment ? wallet::findAsset(payment->asset) : nullptr;
    if (!info)
        return;
    const wallet::WalletModel* rows = controller_->model();
    for (int row = 0; row < rows->size(); ++row) {
        const wallet::AddressRow& it = rows->at(row);
        if (it.network == info->network && it.wallet == payment->wallet && it.address == payment->address) {
            const wallet::AddressRow found = it;
            accept();
            emit openAddressRequested(found);
            return;
        }
    }
}

void PaymentsDialog::done(int result)
{
    controller_->payments()->markAllRead();
    QDialog::done(result);
}

} // namespace app::ui
