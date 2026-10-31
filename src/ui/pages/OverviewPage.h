#pragma once

#include <QWidget>

namespace app {
class WalletController;
}

namespace app::ui {

// The Overview page. Without a wallet: an empty state with an "Import seed"
// primary button (emits importRequested). With a wallet: the grand total in USD,
// one card per asset (BTC, ETH, USDT, USDC: amount in its own unit + USD), the
// scan progress and when prices / balances were last updated, and a Refresh
// action in the bottom action bar.
class OverviewPage : public QWidget
{
    Q_OBJECT
public:
    explicit OverviewPage(WalletController* controller, QWidget* parent = nullptr);

signals:
    void importRequested();

private:
    WalletController* controller_ = nullptr;
};

} // namespace app::ui
