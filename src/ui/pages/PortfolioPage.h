#pragma once

#include "wallet/Registry.h"

#include <QList>
#include <QWidget>

class QAction;
class QCheckBox;
class QTableView;
class QLabel;
class QPushButton;
class QStackedLayout;
class QTimer;

namespace wallet {
struct Payment;
}

namespace app {
class WalletController;
}

namespace app::ui {

class AllocationBar;
class HoldingsTable;
class PaymentsModel;

// The page the app opens on. Without a wallet: an empty state with an "Add wallet" primary button
// and "Add many" beside it (importRequested, importManyRequested). With wallets: the grand total in
// USD and what it is made of, then one row per coin, which opens to that coin. A figure reads plain
// only once every balance behind it has answered; until then it reads "—" (nothing known yet) or
// "≥ X" (what is known so far), and the line under the total says why.
class PortfolioPage : public QWidget
{
    Q_OBJECT
public:
    explicit PortfolioPage(WalletController* controller, QWidget* parent = nullptr);

    HoldingsTable* table() const { return table_; }

signals:
    void importRequested();
    void importManyRequested();
    void coinChosen(wallet::Asset asset);
    void receiveRequested();
    void paymentsRequested();
    void paymentChosen(const wallet::Payment& payment);

protected:
    void showEvent(QShowEvent* event) override; // the box follows the one on the pages below

private:
    QWidget* buildEmptyState();
    QWidget* buildWalletState();
    void showState();       // the wallet view when there is a wallet, else the empty state
    void scheduleRefresh(); // one refreshView() per burst of totals / status signals
    void refreshView();
    void refreshCount();
    void refreshLatest();

    WalletController* controller_ = nullptr;
    QStackedLayout* states_ = nullptr;
    QWidget* emptyState_ = nullptr;
    QWidget* walletState_ = nullptr;
    QLabel* totalLabel_ = nullptr;
    QLabel* pricesLabel_ = nullptr; // under the total: why it is not final yet, then the prices
    AllocationBar* allocation_ = nullptr;
    HoldingsTable* table_ = nullptr;
    QAction* hideZero_ = nullptr;
    QAction* hideDust_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QWidget* latestTitle_ = nullptr;
    QTableView* latest_ = nullptr;
    PaymentsModel* latestModel_ = nullptr;
    QString countText_;
    // Chip: "Connecting…" / "Address limit reached" / "Ethereum endpoint answers for
    // another network" / "ETH balances from 14:02" / "Scanning…" / "Incomplete" / "Up to date"
    QTimer* rowsTimer_ = nullptr; // a scan signals per address; the figures follow a few times a second
    bool refreshPending_ = false;
};

} // namespace app::ui
