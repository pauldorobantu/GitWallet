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

// The Overview page. Without a wallet: an empty state with an "Add wallet"
// primary button (emits importRequested). With wallets: the grand total in USD,
// one card per asset (BTC, ETH, USDT, USDC: amount in its own unit + USD), the
// scan progress and when the prices were last updated, and a Refresh action in
// the bottom action bar. A figure reads plain only once every balance behind it
// has answered; until then it reads "—" (nothing known yet) or "≥ X" (what is
// known so far), next to how many addresses are still unchecked (or that the
// address limit leaves some unchecked). Ethereum figures from an earlier pass
// (endpoint off mainnet, refresh failing) say since when.
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

    // Balances without an answer yet, counted from the wallet model's signals
    // so that a refresh never walks thousands of rows.
    void resetPending();          // the model was cleared
    bool syncPending();           // counts the rows appended since the last call; true if a count moved
    void notePendingRow(int row); // a row was replaced: moves its bits between the counts
    void countPending(quint32 mask, int sign);

    WalletController* controller_ = nullptr;
    QStackedLayout* states_ = nullptr;
    QWidget* emptyState_ = nullptr;
    QWidget* walletState_ = nullptr;
    QLabel* totalLabel_ = nullptr;
    QLabel* pricesLabel_ = nullptr; // under the total: why it is not final yet, then the prices
    QList<QLabel*> amountLabels_; // one per wallet::assets() entry, in that order
    QList<QLabel*> usdLabels_;
    // Chip: "Connecting…" / "Address limit reached" / "Ethereum endpoint not on
    // mainnet" / "ETH balances from 14:02" / "Scanning…" / "Incomplete" / "Up to date"
    QLabel* scanLabel_ = nullptr;
    QLabel* walletsLabel_ = nullptr;
    QLabel* scannedLabel_ = nullptr;
    QLabel* usedLabel_ = nullptr;
    QLabel* fundedLabel_ = nullptr;
    QLabel* electrumLabel_ = nullptr;
    QLabel* ethLabel_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    bool refreshPending_ = false;
    qint64 shownDay_ = 0; // local Julian day the last refreshView() rendered for
    QList<quint32> rowPending_;  // per model row: bit i while its wallet::assets()[i] balance has no answer
    QList<int> pendingPerAsset_; // rows with that bit set, per wallet::assets() entry
    int pendingRows_ = 0;        // rows with any bit set
};

} // namespace app::ui
