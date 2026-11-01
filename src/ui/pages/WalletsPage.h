#pragma once

#include <QWidget>

namespace app {
class WalletController;
}

namespace app::ui {

// The Wallets page: one row per wallet with its source, addresses, funded
// addresses, balances and USD value, so a business with many seeds sees at a
// glance what each one holds. A figure reads plain only once every balance behind
// it has answered, as on the Overview page. From here a wallet is added, renamed,
// removed, opened on the Addresses page (also by double-clicking it) and its public
// keys shown.
class WalletsPage : public QWidget
{
    Q_OBJECT
public:
    explicit WalletsPage(WalletController* controller, QWidget* parent = nullptr);

signals:
    void addRequested();
    void showAddressesRequested(quint32 wallet);

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
