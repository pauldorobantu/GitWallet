#include "ui/pages/HoldingsPage.h"

#include "app/WalletController.h"
#include "core/SettingsRepo.h"
#include "ui/Figures.h"
#include "ui/Holdings.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace app::ui {

HoldingsPage::HoldingsPage(WalletController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller)
{
    auto* root = new QVBoxLayout(this);
    page::applyPageLayout(root);
    table_ = new HoldingsTable(controller_, this);
    table_->setObjectName(QStringLiteral("placeHoldings"));
    root->addWidget(table_, 1);

    const page::ActionBar bar = page::makeActionBar(this);
    hideZero_ = new QCheckBox(QStringLiteral("Hide zero balances"), bar.widget);
    hideZero_->setChecked(SettingsRepo().get(QStringLiteral("ui.hideZero")) != QLatin1String("0"));
    table_->setHideZero(hideZero_->isChecked());
    connect(hideZero_, &QCheckBox::toggled, this, [this](bool on) {
        SettingsRepo().set(QStringLiteral("ui.hideZero"), on ? QStringLiteral("1") : QStringLiteral("0"));
        table_->setHideZero(on);
        updateCount();
    });
    bar.left->addWidget(hideZero_);
    count_ = page::makeBarCount(bar.widget);
    bar.left->addWidget(count_);
    bar.left->addStretch(1);
    root->addWidget(bar.widget);
    connect(table_, &HoldingsTable::refreshed, this, &HoldingsPage::updateCount);
}

void HoldingsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    hideZero_->setChecked(SettingsRepo().get(QStringLiteral("ui.hideZero")) != QLatin1String("0"));
}

void HoldingsPage::updateCount()
{
    // No count of nothing while nothing has answered.
    if (!table_->allFinal() && table_->rowsHolding() == 0)
        count_->clear();
    else if (table_->allFinal())
        count_->setText(trText(QStringLiteral("%1 of %2 hold something")).arg(figures::formatCount(table_->rowsHolding()), figures::formatCount(table_->rowsTotal())));
    else
        count_->setText(trText(QStringLiteral("%1 hold something so far")).arg(figures::formatCount(table_->rowsHolding())));
}

} // namespace app::ui
