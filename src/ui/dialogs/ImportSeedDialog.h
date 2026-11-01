#pragma once

#include "wallet/BulkImport.h"
#include "wallet/Types.h"

#include <QDialog>
#include <QList>
#include <QMap>

#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QWidget;

namespace app {
class WalletController;
}

namespace app::ui {

// Adds wallets, one or many at once, from seeds or from public keys.
//
// A BIP39 mnemonic (12–24 words) with an optional passphrase is validated as the user
// types (word count, words not in the list, checksum), then goes to
// WalletController::addMnemonicWallet. A list of them, one per line with an optional
// name (see wallet/BulkImport.h), is checked the same way line by line; the good lines
// go in together, and what did not go in stays in the field, each line named by number
// with why and never quoted. The text fields are wiped when the dialog closes, accepted
// or not, and a seed itself is never written anywhere.
//
// Public keys (zpub, ypub, xpub for Bitcoin, xpub for Ethereum) need no secret at all,
// and each shows its first address to compare with the other wallet: see
// WalletController::addKeysWallet. Seed words pasted into the name or a key field are
// wiped at once, so they cannot become a wallet's name by accident.
class ImportSeedDialog : public QDialog
{
    Q_OBJECT
public:
    // `many`: opens on the list of seeds rather than on one seed.
    explicit ImportSeedDialog(WalletController* controller, QWidget* parent = nullptr, bool many = false);
    ~ImportSeedDialog() override;

    // Reads a text file into the list on show. False, with the reason in the status
    // line, when it cannot.
    bool loadListFile(const QString& path);
    // What was added, for the window to say once this has closed; empty if nothing.
    QString resultText() const { return result_; }

protected:
    void done(int result) override; // wipes the fields, then closes

private:
    int source() const;         // which of the four ways the dialog is on
    void sourceChanged();
    void validateWords();       // the status line + wordsValid_, on every edit
    void validateKeys();        // the status line + parsedKeys_, on every edit
    void validateSeedList();    // the status line + listReady_, on every edit
    void validateKeyList();     // the same
    void updateImportEnabled();
    void importSeed();
    void addOneSeed();
    void addOneKeys();
    void startSeedList();       // a seed's keys take milliseconds: one per event-loop turn
    void seedListStep();
    void addKeyList();
    // Adds listRequests_ and says how it went. `toFix` and what was refused stay in
    // `field`, by line number; the dialog closes when nothing is left.
    void finishList(QPlainTextEdit* field, const QMap<int, QString>& toFix);
    void stopList();            // scrubs what a list in progress holds
    void setBusy(bool busy);
    void chooseListFile();
    QString labelFor(const QString& name); // a line's own name, or the field's numbered
    void wipeFields();          // overwrites, then empties, every text field

    WalletController* controller_ = nullptr;
    QLabel* nameLabel_ = nullptr;
    QLineEdit* name_ = nullptr;
    QComboBox* source_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QPlainTextEdit* words_ = nullptr;
    QPlainTextEdit* seedList_ = nullptr;
    QPlainTextEdit* keys_ = nullptr;     // Bitcoin public keys
    QLineEdit* ethKey_ = nullptr;
    QWidget* xpubRow_ = nullptr;         // only while the Bitcoin keys include an xpub
    QComboBox* xpubKind_ = nullptr;      // no default: the wrong type shows other addresses
    QPlainTextEdit* keyList_ = nullptr;
    QWidget* listKindRow_ = nullptr;     // only while the list includes an xpub
    QComboBox* listKind_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* info_ = nullptr;
    QCheckBox* hasPassphrase_ = nullptr;
    QLineEdit* passphrase_ = nullptr;
    QPushButton* cancel_ = nullptr;      // "Close" once something went in
    QPushButton* import_ = nullptr;
    bool wordsValid_ = false; // count, words and checksum all check out
    QList<wallet::AccountKey> parsedKeys_; // what the keys field holds, once all of it is valid

    int listReady_ = 0;       // lines of the list on show that can go in
    bool running_ = false;    // a list is being worked through
    std::vector<wallet::SeedLine> pending_; // the seeds of that list; each line scrubbed as it is done
    size_t pendingNext_ = 0;
    int pendingDone_ = 0;
    int pendingTotal_ = 0;
    std::string listPassphrase_;
    QMap<int, QString> listToFix_;
    QList<wallet::WalletRequest> listRequests_;
    int unnamed_ = 0;         // lines given a number after the field's name
    int addedTotal_ = 0;
    QString result_;
};

} // namespace app::ui
