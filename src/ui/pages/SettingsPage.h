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
};

} // namespace app::ui
