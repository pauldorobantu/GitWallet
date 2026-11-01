#pragma once

#include <QWidget>

namespace app {
class WalletController;
}

namespace app::ui {

// The Settings page, as kit sections: Scanning (BTC receive / change gap, ETH gap,
// ETH refresh interval), Network (Electrum servers, Ethereum RPC endpoints, one
// per line; empty = defaults; shows the server in use), Wallet (the account
// xpubs, copyable, and a "Remove wallet from this PC" action with confirmation).
// Save applies through WalletController::applySettings.
class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPage(WalletController* controller, QWidget* parent = nullptr);

private:
    WalletController* controller_ = nullptr;

    // --- implementation (SettingsPage.cpp) ---
    struct Ui; // the page's widgets

    void load();          // the form from the stored settings, plus the xpubs
    void rebuildWallet(); // one row per stored account
    void updateInUse();   // the Electrum server / Ethereum endpoint in use
    void resetToDefaults();
    void save();
    void confirmRemove();
    void removeWallet();

    Ui* ui_ = nullptr;
};

} // namespace app::ui
