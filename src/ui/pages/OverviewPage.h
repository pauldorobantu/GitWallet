#pragma once

#include "wallet/Registry.h"

#include <QList>
#include <QWidget>

class QLabel;
class QPushButton;
class QStackedLayout;
class QTimer;

namespace app {
class WalletController;
}

namespace app::ui {

// The Overview page. Without a wallet: an empty state with an "Add wallet" primary
// button and "Add many" beside it (importRequested, importManyRequested). With
// wallets: the grand total in USD,
// one card per asset (BTC, ETH, USDT, USDC: amount in its own unit + USD), the
// scan progress and when the prices were last updated, and a Refresh action in
// the bottom action bar. A figure reads plain only once every balance behind it
// has answered; until then it reads "—" (nothing known yet) or "≥ X" (what is
// known so far), next to how many addresses are still unchecked (or that the
// address limit leaves some unchecked). Figures from an earlier round (a server on
// another chain, a refresh failing) say since when, and so do those the database kept,
// until the servers have answered.
class OverviewPage : public QWidget
{
    Q_OBJECT
public:
    explicit OverviewPage(WalletController* controller, QWidget* parent = nullptr);

signals:
    void importRequested();
    void importManyRequested();

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
    QLabel* pricesLabel_ = nullptr; // under the total: why it is not final yet, then the prices
    QList<QLabel*> amountLabels_; // one per wallet::assets() entry, in that order
    QList<QLabel*> usdLabels_;
    // Chip: "Connecting…" / "Address limit reached" / "Ethereum endpoint answers for
    // another network" / "ETH balances from 14:02" / "Scanning…" / "Incomplete" / "Up to date"
    QLabel* scanLabel_ = nullptr;
    QLabel* walletsLabel_ = nullptr;
    QLabel* scannedLabel_ = nullptr;
    QLabel* usedLabel_ = nullptr;
    QLabel* fundedLabel_ = nullptr;
    QList<QLabel*> serverLabels_; // one per wallet::networks() entry, in that order
    QPushButton* refreshButton_ = nullptr;
    QTimer* rowsTimer_ = nullptr; // a scan signals per address; the figures follow a few times a second
    bool refreshPending_ = false;
};

} // namespace app::ui
