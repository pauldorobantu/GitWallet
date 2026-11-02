#pragma once

#include <QWidget>

namespace app {
class WalletController;
}

namespace app::ui {

// The Wallets page: one row per wallet with its source, addresses, funded
// addresses, balances and USD value, so a business with many seeds sees at a
// glance what each one holds. A figure reads plain only once every balance behind
// it has answered and the scan reached all it should, as on the Overview page; a
// chain the wallet has no key for reads "—", and the State column says how current
// the row is. From here a wallet is added, renamed, removed, opened on what it holds (a
// double click) or on its addresses or history, and its public keys shown.
class WalletsPage : public QWidget
{
    Q_OBJECT
public:
    explicit WalletsPage(WalletController* controller, QWidget* parent = nullptr);

signals:
    void addRequested();
    void addManyRequested();
    void openRequested(quint32 wallet);
    void showAddressesRequested(quint32 wallet);
    void showHistoryRequested(quint32 wallet);

protected:
    void showEvent(QShowEvent* event) override; // the figures repaint only while this page shows

private:
    WalletController* controller_ = nullptr;

    // --- implementation (WalletsPage.cpp) ---
    struct Ui; // the page's widgets and models

    void updateState();     // buttons, count, table / empty / no-results
    quint32 selectedId() const; // 0 when no row is selected
    void renameSelected();
    void confirmRemoveSelected();
    void showKeysOfSelected();

    Ui* ui_ = nullptr;
};

} // namespace app::ui
