#pragma once

#include <QWidget>

namespace app {
class WalletController;
}

namespace app::ui {

// The Settings page, as kit sections: Scanning (BTC receive / change gap, ETH gap,
// ETH refresh interval), Network (Electrum servers, Ethereum RPC endpoints, one
// per line; empty = defaults; shows the server in use), Wallets (how many, and a
// "Remove all wallets from this PC" action with confirmation). Save applies
// through WalletController::applySettings.
class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPage(WalletController* controller, QWidget* parent = nullptr);

private:
    WalletController* controller_ = nullptr;

    // --- implementation (SettingsPage.cpp) ---
    struct Ui; // the page's widgets

    void load();          // the form from the stored settings, plus the wallet count
    void rebuildWallet(); // the wallet count, and whether Remove is on offer
    void updateInUse();   // the Electrum server / Ethereum endpoint in use
    void resetToDefaults();
    void save();
    void confirmRemove();
    void removeWallet();

    Ui* ui_ = nullptr;
};

} // namespace app::ui
