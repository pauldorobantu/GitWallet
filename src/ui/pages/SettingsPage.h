#pragma once

#include <QWidget>

namespace wallet {
struct ScanSettings;
}

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

    // What a test goes through, past the file dialogs.
    void makeBackupAt(const QString& path);
    void restoreBackupFrom(const QString& path);

signals:
    void lockRequested();

private:
    WalletController* controller_ = nullptr;

    // --- implementation (SettingsPage.cpp) ---
    struct Ui; // the page's widgets

    void load();          // the form from the stored settings, plus the wallet count
    void rebuildWallet(); // the wallet count, and whether Remove is on offer
    void updateInUse();   // the Electrum server / Ethereum endpoint in use
    void resetToDefaults();
    void save();          // checks the form, then askThenApply()
    // Warns when smaller gaps leave out more than the `accepted` used addresses the
    // user has already agreed to (-1: nothing yet), or when that is not known yet.
    void askThenApply(const wallet::ScanSettings& settings, int accepted);
    void apply(const wallet::ScanSettings& settings);
    void confirmRemove();
    void exportLabels();
    void importLabels();
    void removeWallet();
    void updatePassword(); // what the card says, and which buttons it offers
    void updateBackup();
    void askPassword(int mode);
    void chooseBackupFile();
    void chooseRestoreFile();

    Ui* ui_ = nullptr;
};

} // namespace app::ui
