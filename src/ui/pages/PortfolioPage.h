#pragma once

#include "wallet/Registry.h"

#include <QList>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QStackedLayout;
class QTimer;

namespace app {
class WalletController;
}

namespace app::ui {

class AllocationBar;
class HoldingsTable;

// The page the app opens on. Without a wallet: an empty state with an "Add wallet" primary button
// and "Add many" beside it (importRequested, importManyRequested). With wallets: the grand total in
// USD and what it is made of, then one row per coin, which opens to that coin. A figure reads plain
// only once every balance behind it has answered; until then it reads "—" (nothing known yet) or
// "≥ X" (what is known so far), and the line under the total says why. How the scan stands and on
// which servers is under Details, as is usual: detail stays hidden until asked for.
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

protected:
    void showEvent(QShowEvent* event) override; // the box follows the one on the pages below

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
    AllocationBar* allocation_ = nullptr;
    HoldingsTable* table_ = nullptr;
    QCheckBox* hideZero_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QWidget* details_ = nullptr;
    QPushButton* detailsButton_ = nullptr;
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
