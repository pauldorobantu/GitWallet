#pragma once

#include <QList>
#include <QWidget>

class QLabel;
class QPushButton;
class QStackedLayout;

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
    QWidget* buildEmptyState();
    QWidget* buildWalletState();
    void showState();       // the wallet view when there is a wallet, else the empty state
    void scheduleRefresh(); // one refreshView() per burst of totals / status signals
    void refreshView();

    WalletController* controller_ = nullptr;
    QStackedLayout* states_ = nullptr;
    QWidget* emptyState_ = nullptr;
    QWidget* walletState_ = nullptr;
    QLabel* totalLabel_ = nullptr;
    QLabel* pricesLabel_ = nullptr;
    QList<QLabel*> amountLabels_; // one per wallet::assets() entry, in that order
    QList<QLabel*> usdLabels_;
    QLabel* scanLabel_ = nullptr; // chip: "Connecting…" / "Scanning…" / "Up to date"
    QLabel* scannedLabel_ = nullptr;
    QLabel* usedLabel_ = nullptr;
    QLabel* fundedLabel_ = nullptr;
    QLabel* electrumLabel_ = nullptr;
    QLabel* ethLabel_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    bool refreshPending_ = false;
    qint64 shownDay_ = 0; // local Julian day the last refreshView() rendered for
};

} // namespace app::ui
