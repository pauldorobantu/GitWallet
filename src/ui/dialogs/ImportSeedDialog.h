#pragma once

#include "wallet/Types.h"

#include <QDialog>
#include <QList>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;

namespace app {
class WalletController;
}

namespace app::ui {

// Adds a wallet under a name, from one of two sources. A BIP39 mnemonic (12–24
// words) with an optional passphrase is validated as the user types (word count,
// words not in the list, checksum), then goes to
// WalletController::addMnemonicWallet; the text fields are wiped when the dialog
// closes, accepted or not, and the seed itself is never written anywhere. Public
// keys (zpub, ypub, xpub, one per line) need no secret at all: see
// WalletController::addKeysWallet.
class ImportSeedDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ImportSeedDialog(WalletController* controller, QWidget* parent = nullptr);
    ~ImportSeedDialog() override;

protected:
    void done(int result) override; // wipes the fields, then closes

private:
    bool wordsMode() const;     // the recovery words page is the one showing
    void sourceChanged();
    void validateWords();       // the status line + wordsValid_, on every edit
    void validateKeys();        // the status line + parsedKeys_, on every edit
    void updateImportEnabled();
    void importSeed();
    void wipeFields();          // overwrites, then empties, both secret editors

    WalletController* controller_ = nullptr;
    QLineEdit* name_ = nullptr;
    QComboBox* source_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QPlainTextEdit* words_ = nullptr;
    QPlainTextEdit* keys_ = nullptr;
    QWidget* xpubRow_ = nullptr;   // only while the keys include an xpub
    QComboBox* xpubKind_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* info_ = nullptr;
    QCheckBox* hasPassphrase_ = nullptr;
    QLineEdit* passphrase_ = nullptr;
    QPushButton* import_ = nullptr;
    bool wordsValid_ = false; // count, words and checksum all check out
    QList<wallet::AccountKey> parsedKeys_; // what the keys field holds, once all of it is valid
};

} // namespace app::ui
