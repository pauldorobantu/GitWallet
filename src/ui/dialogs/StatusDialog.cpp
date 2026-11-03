#include "ui/dialogs/StatusDialog.h"

#include "app/WalletController.h"
#include "ui/DialogChrome.h"
#include "ui/Figures.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/design/Style.h"
#include "wallet/Registry.h"
#include "wallet/Scheduler.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QDate>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace app::ui {

namespace {

constexpr qint64 kNoncesOldSeconds = 6 * 3600;

// The time alone while that is today, with the date once it is not.
QString shownTime(const QDateTime& when)
{
    const QDateTime local = when.toLocalTime();
    if (local.date() == QDate::currentDate())
        return local.toString(QStringLiteral("HH:mm"));
    return QLocale(QLocale::English).toString(local, QStringLiteral("d MMM HH:mm"));
}

// A server row: its name, or a muted "Not connected" while there is none, and how far
// behind a long round can leave what is not followed live.
void setServer(QLabel* label, const wallet::NetworkStatus& status)
{
    QString text = status.server.isEmpty() ? trText(QStringLiteral("Not connected")) : status.server;
    if (status.tiers.rounds > 0) {
        const int minutes = qMax(1, status.tiers.roundSeconds / 60);
        text += QStringLiteral(" · ")
                + trText(QStringLiteral("%1 live, %2 read every %3 min"))
                      .arg(figures::formatCount(status.tiers.live), figures::formatCount(status.tiers.rounds), figures::formatCount(minutes));
    }
    // Nonces are read now and then, and all they can add is a rare address. Long overdue, the
    // figures are as right as ever, but what lies past those windows is not looked at.
    if (status.noncesAt.isValid() && status.noncesAt.secsTo(QDateTime::currentDateTime()) > kNoncesOldSeconds)
        text += QStringLiteral(" · ") + trText(QStringLiteral("new addresses last looked for %1")).arg(shownTime(status.noncesAt));
    label->setText(text);
    setTone(label, status.server.isEmpty() ? Tone::Muted : Tone::Neutral);
}

} // namespace

StatusDialog::StatusDialog(WalletController* controller, QWidget* parent)
    : QDialog(parent), controller_(controller)
{
    setObjectName(QStringLiteral("statusDialog"));
    setWindowTitle(trText(QStringLiteral("Scan status and servers")));
    applyDialogChrome(this);
    auto* root = new QVBoxLayout(this);
    page::applyDialogLayout(root);

    whole_ = new QLabel(this);
    whole_->setObjectName(QStringLiteral("statusWhole"));
    whole_->setTextFormat(Qt::PlainText);
    whole_->setWordWrap(true);
    root->addWidget(whole_);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    scan_ = new QLabel(this);
    scan_->setObjectName(QStringLiteral("statusScan"));
    scan_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    form->addRow(trText(QStringLiteral("Scan")), scan_);
    const auto valueRow = [&](const QString& title) {
        auto* value = new QLabel(figures::dash(), this);
        value->setTextFormat(Qt::PlainText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(title, value);
        return value;
    };
    wallets_ = valueRow(trText(QStringLiteral("Wallets")));
    scanned_ = valueRow(trText(QStringLiteral("Addresses scanned")));
    used_ = valueRow(trText(QStringLiteral("Used addresses")));
    funded_ = valueRow(trText(QStringLiteral("Funded addresses")));
    for (const wallet::NetworkInfo& network : wallet::networks())
        servers_.append(valueRow(trText(QStringLiteral("%1 server")).arg(network.name)));
    root->addLayout(form);
    root->addStretch(1);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* close = new QPushButton(trText(QStringLiteral("Close")), this);
    close->setObjectName(QStringLiteral("primaryButton"));
    close->setDefault(true);
    buttons->addWidget(close);
    root->addLayout(buttons);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    // Many at once are one look.
    soon_ = new QTimer(this);
    soon_->setSingleShot(true);
    soon_->setInterval(250);
    connect(soon_, &QTimer::timeout, this, &StatusDialog::refresh);
    connect(controller_, &WalletController::statusChanged, soon_, qOverload<>(&QTimer::start));
    connect(controller_, &WalletController::totalsChanged, soon_, qOverload<>(&QTimer::start));

    page::fitDialog(this, page::DialogSize::Compact);
    refresh();
}

void StatusDialog::refresh()
{
    const Tone tones[] = {Tone::Success, Tone::Accent, Tone::Warning, Tone::Error, Tone::Muted};
    setChip(scan_, controller_->statusLead(), tones[int(controller_->statusTone())]);
    whole_->setText(controller_->statusText());
    const wallet::WalletModel* model = controller_->model();
    const int derived = model ? model->size() : 0;
    const int checked = model ? model->checkedCount() : 0;
    wallets_->setText(figures::formatCount(int(controller_->store().wallets.size())));
    // Until the servers have answered, what a row says it knows is from an earlier run.
    scanned_->setText(controller_->showingSaved() ? trText(QStringLiteral("%1 saved")).arg(figures::formatCount(derived))
                      : checked >= derived ? figures::formatCount(derived)
                                           : trText(QStringLiteral("%1 of %2")).arg(figures::formatCount(checked), figures::formatCount(derived)));
    used_->setText(figures::formatCount(model ? model->usedCount() : 0));
    funded_->setText(figures::countSoFar(model ? model->fundedCount() : 0, derived > 0 && checked >= derived));
    const QList<wallet::NetworkInfo>& networks = wallet::networks();
    for (qsizetype i = 0; i < networks.size() && i < servers_.size(); ++i)
        setServer(servers_.at(i), controller_->networkStatus(networks.at(i).network));
}

} // namespace app::ui
