#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

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
    void validateWords();       // the status line + wordsValid_, on every edit
    void updateImportEnabled();
    void importSeed();
    void wipeFields();          // overwrites, then empties, both editors

    WalletController* controller_ = nullptr;
    QPlainTextEdit* words_ = nullptr;
    QLabel* status_ = nullptr;
    QCheckBox* hasPassphrase_ = nullptr;
    QLineEdit* passphrase_ = nullptr;
    QPushButton* import_ = nullptr;
    bool wordsValid_ = false; // count, words and checksum all check out
};

} // namespace app::ui
