#pragma once

#include <QDialog>

namespace app {
class WalletController;
}

namespace app::ui {

// Import a BIP39 mnemonic (12–24 words) with an optional passphrase. Validates
// as the user types (word count, words not in the list, checksum), then calls
// WalletController::importMnemonic. The text fields are wiped when the dialog
// closes, accepted or not; the seed itself is never written anywhere.
class ImportSeedDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ImportSeedDialog(WalletController* controller, QWidget* parent = nullptr);
    ~ImportSeedDialog() override;

protected:
    void done(int result) override; // wipes the fields, then closes

private:
    WalletController* controller_ = nullptr;
};

} // namespace app::ui
