#include "ui/dialogs/PaymentsDialog.h"

#include "app/WalletController.h"
#include "ui/DialogChrome.h"
#include "ui/Feedback.h"
#include "ui/Figures.h"
#include "ui/HoverRowTable.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/PaymentsModel.h"
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

PaymentsDialog::PaymentsDialog(WalletController* controller, QWidget* parent)
    : QDialog(parent), controller_(controller)
{
    setObjectName(QStringLiteral("paymentsDialog"));
    setWindowTitle(trText(QStringLiteral("Incoming payments")));
    applyDialogChrome(this);
    auto* root = new QVBoxLayout(this);
    page::applyDialogLayout(root);

    auto* model = new PaymentsModel(controller_, 0, this);
    lastAtOpen_ = controller_->payments()->lastId();
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
    // What is on screen sets the widths: measuring a thousand rows at each change holds up the scanners too.
    header->setResizeContentsPrecision(0);
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(PaymentsModel::ColAddress, QHeaderView::Stretch);
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
    // Many coming one after the other are one redraw.
    auto* redraw = new QTimer(this);
    redraw->setSingleShot(true);
    redraw->setInterval(200);
    connect(controller_->payments(), &wallet::PaymentBook::changed, redraw, qOverload<>(&QTimer::start));
    connect(redraw, &QTimer::timeout, this, [this, model] {
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
    connect(controller_, &WalletController::labelsChanged, model, &PaymentsModel::refreshLabels);

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
    Feedback::show(this, Feedback::Type::Info, trText(QStringLiteral("That address is not among those followed now")));
}

void PaymentsDialog::done(int result)
{
    if (forLock_)
        controller_->payments()->markReadUpTo(lastAtOpen_);
    else
        controller_->payments()->markAllRead();
    QDialog::done(result);
}

} // namespace app::ui
