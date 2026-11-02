#pragma once

#include "store/LedgerDb.h"
#include "wallet/Scheduler.h"
#include "wallet/Types.h"
#include "wallet/WalletStore.h"

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>

#include <memory>
#include <QPointer>
#include <QThread>
#include <string>
#include <vector>

class QLockFile;
class QTimer;

namespace net {
class PriceFeed;
} // namespace net

namespace store {
class Ledger;
} // namespace store

namespace wallet {
class HistoryBook;
class PaymentBook;
class UtxoHistory;
class WalletModel;
} // namespace wallet

namespace app {

// Owns the wallet's data and services: the store (xpubs + settings), the database of
// what they have shown so far, the address model, the USD price feed and the scheduler
// that runs a scan per network. Pages read from it; the import dialog and the Settings
// page write through it. One process at a time writes a data folder (its wallet.lock): a
// second GitWallet on the same folder shows the wallet but saves nothing.
// GUI thread only.
struct LabelImport;

class WalletController : public QObject
{
    Q_OBJECT
public:
    explicit WalletController(QObject* parent = nullptr);
    ~WalletController() override;

    bool hasWallet() const;

    // At startup: takes the data folder's lock if it is free, loads wallet.dat
    // if present, shows what the database kept and starts scanning. Returns false with an
    // empty `error` when
    // there is no wallet yet, false with a message when the file could not be
    // read.
    bool loadExisting(QString* error);

    // Validates the mnemonic, derives the account xpubs and adds them as a wallet
    // named `label` (empty: "Wallet N"), saved with the others (DPAPI), then
    // scans it too. Never stores the words; the caller scrubs its own copies.
    // Refused while another GitWallet process holds the data folder, and for a
    // seed whose keys another wallet already has.
    bool addMnemonicWallet(const std::vector<std::string>& words, const std::string& passphrase,
                           const QString& label, QString* error);
    // The same for public keys from WalletStore::accountsFromPublicKeys().
    bool addKeysWallet(const QList<wallet::AccountKey>& keys, const QString& label, QString* error);
    // Adds every request that can go in, with one save and one restart of the
    // scanners, which is what a list of hundreds needs. A request that cannot says
    // why in its `refused`. Returns how many went in; 0 with `error` when the file
    // could not take any of them.
    int addWallets(QList<wallet::WalletRequest>* requests, QString* error);

    // Forgets one wallet: its keys leave wallet.dat, its rows the model; the
    // others keep their data and go on being watched.
    bool removeWallet(quint32 id, QString* error);
    bool renameWallet(quint32 id, const QString& label, QString* error);

    // Deletes wallet.dat, settings included, with any unread file an add kept
    // aside, stops scanning and clears the model. Refused while another GitWallet
    // process holds the data folder, or when the file changed after this window read it.
    bool removeAllWallets(QString* error);

    // The older files removeAllWallets() would delete with wallet.dat.
    int walletFilesKeptAside() const;
    // And the older databases, which may hold the only copy of labels and notes.
    int databasesKeptAside() const;
    // The file this session last kept aside, as errorOccurred() named it.
    QString fileSetAside() const { return fileSetAside_; }
    // What happens to a wallet.dat that could not be loaded, for the user to read:
    // kept aside at the next add, or moved by hand if it cannot even be opened.
    QString unreadFileNote() const;

    // Addresses with history that `settings` would leave out. When a gap falls the rows
    // go and a fresh scan walks each chain from index 0, stopping once a gap of unused
    // addresses follows the last used one; any scan also stops at the limit over all
    // accounts. What this session's rows say: only the addresses answered so far.
    struct GapImpact {
        int used = 0;         // used addresses past where the scan would stop
        int funded = 0;       // those holding a balance
        bool atMost = false;  // the limit cuts some of them: which ones depends on the scan
        qint64 overLimit = 0; // addresses wanted past the limit, beyond those of the settings in use
    };
    GapImpact gapImpact(const wallet::ScanSettings& settings) const;
    // A gap that falls takes the rows with it (applySettings).
    static bool gapsFall(const wallet::ScanSettings& from, const wallet::ScanSettings& to);

    // Every account of the wallet has rows, on both chains of a Bitcoin account.
    bool walletHasAllRows(const wallet::WalletEntry& entry) const;

    // Saves new scan settings and restarts the scanners / clients with them.
    // When wallet.dat is not this session's to write (another process holds
    // the folder, or the file changed or went away after this session read
    // it), they apply for this session only: false, with the reason.
    bool applySettings(const wallet::ScanSettings& settings, QString* error);

    const wallet::WalletStore& store() const { return store_; }
    wallet::WalletModel* model() const { return model_; }
    net::PriceFeed* prices() const { return prices_; }
    wallet::PaymentBook* payments() const { return payments_; }

    void refresh(); // prices + a round of every balance that is polled, now (what is followed live is live)

    bool isScanning() const;  // any network
    bool addressLimitReached() const; // a window is cut at the row limit
    // How one network's scan stands: is it through, on which server, as of when.
    wallet::NetworkStatus networkStatus(wallet::Network network) const;
    // Every balance shown is whole and current: statusText() says "Up to date".
    bool balancesComplete() const;
    // What is shown is as current as it gets, a key this version does not read aside.
    bool figuresCurrent() const;
    bool hasUnreadKeys() const; // a wallet has a key of a kind or network this version does not know
    // One short line for the title bar chip. "Up to date" only when
    // balancesComplete(); otherwise it leads with what keeps it from that.
    QString statusText() const;

    // What the figures on screen are from until the servers have answered: true from the
    // start, when the database holds rows, until every network has answered for all it
    // follows. The time is the oldest network's, and invalid if one never had a whole answer.
    bool showingSaved() const { return showingSaved_; }
    QDateTime savedAt() const { return savedAt_; }
    // The database is still handing over what it kept: whatever is there is part of it.
    bool loadingSaved() const { return waitingForLedger_; }
    bool keepingSaved() const; // the database is open, so what happens now is there at the next start
    bool labelsOwed() const;   // those of a file kept aside have yet to come over, and none can change until they have

    // "Prices: DefiLlama · 14:02", beside any USD figure: where the prices of what the wallets
    // hold come from and how old the oldest is, with the date once it is not today, and
    // that it stopped moving, which of them has no price, and that a refresh is failing.
    QString pricesLine() const;

    // The history of every network that has one.
    QList<wallet::UtxoHistory*> histories() const { return scheduler_->histories(); }
    wallet::UtxoHistory* historyOf(wallet::Network network) const;
    // Empty when labels and notes can be read and written now; if not, why not.
    QString labelsBlocker() const;
    // A label and a note on a transaction, and a label on an address. False + why when they
    // cannot be kept (a name that looks like seed words, or labelsBlocker()).
    bool setTxNote(wallet::Network network, const chain::Hash256& txid, const wallet::TxNote& note, QString* error);
    bool setAddressLabel(const QString& address, const QString& label, QString* error);
    QString addressLabel(const QString& address) const { return addressLabels_.value(address); }
    const QHash<QString, QString>& addressLabels() const { return addressLabels_; }
    bool hasLabels() const;
    int labelCount() const;              // every label and note there is
    int labelCount(quint32 wallet) const; // those that would go with this wallet
    // Every label and note, for a copy kept elsewhere: only this PC has them.
    QByteArray labelsFile() const;
    // The same into a file, off this thread. `done` runs on this one, if `guard` is still there.
    void writeLabelsFile(const QString& path, QObject* guard, const std::function<void(bool ok, const QString& error)>& done);
    // What such a file says goes in, a part per event-loop turn, over what is there: only what a
    // line says, so that a file from a wallet that has only labels leaves a note alone. `done`: how
    // many went in, how many lines were left out, how many name a transaction not read yet, so
    // that the same file will take them later, or -1 with `error` when none can be kept.
    void importLabelsFile(const QString& path, QObject* guard, const std::function<void(int added, int skipped, int later, const QString& error)>& done);

    // The Receive page: the keys a wallet could be paid on, one per kind of address, its first account,
    // in the order the list of coins captures them.
    QList<wallet::AccountKey> receiveKeys(quint32 wallet, wallet::Network network) const;
    // That row's address, as its key gives it now; empty when it gives another, or none. What is
    // handed to someone to pay comes from here, not from what is kept.
    QString checkedAddress(const wallet::AddressRow& row) const;
    // The next address of one of them. Always derived here and now from the key: what is kept beside
    // it only says which index is next, and if it says another address for that index, none is offered.
    wallet::ReceiveOffer nextReceive(quint32 wallet, wallet::Network network, chain::ScriptType type) const;
    // That address left this PC's screen for someone who is to pay: it is not offered again, and it is
    // followed ahead of others until it is paid. Only a receive address with no history counts, of any
    // account of the wallet's: a change address is no one's to be given. It holds for this session in
    // any case; false + why when it cannot be written down, and then that is tried again until it is.
    bool markGivenOut(const wallet::AddressRow& row, QString* error);
    bool isGivenOut(const wallet::AddressRow& row) const;
    // Positions in the model of what was given out on that kind of key, the latest first, at most `limit`.
    QList<int> givenOutRows(quint32 wallet, wallet::Network network, chain::ScriptType type, int limit) const;
    int givenOutCount(quint32 wallet, wallet::Network network, chain::ScriptType type) const; // all of those
    // Addresses given out with nothing come in yet, of one wallet or all: removed, they would be
    // offered again if the seed comes back.
    int givenOutWaiting(quint32 wallet = 0) const;
    bool givenOutOwed() const { return givenOwed_; } // what was given out has yet to reach the file
    // The screens are tried with no server: what a network's status then says, as a test wants it.
    void setStatusForTest(const std::function<void(wallet::NetworkStatus&)>& adjust) { statusForTest_ = adjust; }
    void setShowingSavedForTest(bool saved) { showingSaved_ = saved; }
    // What is not plain about one asset's price; empty: nothing.
    QString priceNote(wallet::Asset asset) const;
    // Addresses whose transactions do not add up to their balance, when that is what stands out now.
    int balancesInDoubt() const;

    double usdValue(wallet::Asset asset) const; // total of `asset` at the latest price
    double totalUsd() const;
    double totalUsd(quint32 wallet) const;      // one wallet's

signals:
    void walletChanged();  // loaded / imported / removed
    void statusChanged();  // statusText() or any of the scan states above changed
    void givenOutChanged(); // an address was given out
    void rowGivenOut(int row); // which, in the model
    void historyProgress(); // how far a history has got moved
    void labelsChanged();  // an address's label
    void totalsChanged();  // balances or prices changed
    void scanFinished();   // both chains finished their initial scan
    // The scanners' problem() reports and the controller's own. `message` is
    // GitWallet's own sentence; `serverDetail` is what a server wrote, if
    // anything (one line, cleaned up), and is never part of `message`.
    void errorOccurred(wallet::Problem kind, const QString& message, const QString& serverDetail);

private:
    // --- implementation (WalletController.cpp may add private members) ---
    const wallet::AccountKey* receiveKey(quint32 wallet, wallet::Network network, chain::ScriptType type) const; // the one Receive goes by
    const wallet::AccountKey* keyOf(const wallet::AddressRow& row) const; // the one that row comes from
    bool saveGivenOut(QString* why);
    // What statusText() leads with, the most pressing first.
    enum class Status {
        NoWallet,
        Loading,    // the database is still handing over what it kept
        Connecting,
        Limit,      // a window cut at the row limit
        WrongChain, // a server answers for another network
        Stale,      // the last round on a network left addresses without an answer
        Scanning,
        Unchecked,  // nothing scans, yet a row has no answer
        Unscanned,  // an account, or a Bitcoin chain, has no rows at all
        DoNotAddUp, // what a server says an address holds is not what its transactions come to
        UpToDate,
    };

    void startServices();
    void stopServices();
    void queueStatus();                   // throttled statusChanged() for progress and balances
    void onScanFinished();                // every network has its first answers in
    void onScanProgress();
    void tryLedgerAgain(); // the data folder was held at the start
    void onScannerProblem(wallet::Problem kind, const QString& message, const QString& serverDetail);
    // Opens the database, if this window may write it, and starts scanning once what it
    // kept is in the model.
    void openLedger();
    void onLedgerLoaded(const QList<store::SavedPrice>& prices, const QList<store::SavedTime>& times);
    void importNextPart(const std::shared_ptr<LabelImport>& import);
    // A key among what looks like one: the id of a transaction these wallets know, or that a file of
    // labels names, is no secret.
    bool historiesRead() const; // every history has read all there is, as far as anyone can tell
    bool holdsSecret(const QString& text, const QSet<chain::Hash256>& alsoKnown = {}) const;
    int differingAddresses() const;
    void saveTimes();  // when each network last had a whole answer
    void savePrices();
    Status status() const;
    bool accountUnscanned() const; // an account (a Bitcoin one: either chain) without a single row
    // Takes, or keeps, the data folder's wallet.lock. False while another
    // GitWallet process holds it: that one alone writes or deletes wallet.dat.
    bool lockDataDir();
    // Empty when settings alone may be written to wallet.dat now, else why not.
    QString settingsSaveBlocker();
    // Adds `accounts` as a wallet, saves and restarts the scanners. What a
    // refusal says starts with `what` ("The seed was not added").
    bool addWallet(QList<wallet::AccountKey> accounts, const QString& label, bool fromKeys,
                   const QString& what, QString* error);
    int addWalletsAs(QList<wallet::WalletRequest>* requests, const QString& what, QString* error);
    // Empty when the wallet list may be written to wallet.dat now, else why not.
    // A wallet.dat that opens after all is left alone (the answer says to restart).
    // One that still cannot be read is moved aside, its new name in `movedTo`: it
    // may hold other wallets and open later for the right Windows user or a newer
    // GitWallet, so it is never overwritten.
    QString walletSaveBlocker(QString* movedTo);
    bool saveStore(QString* why); // writes wallet.dat and remembers what it wrote
    bool accountHasRows(const wallet::AccountKey& account) const;
    void restartServices(); // stop + start, keeping the model's rows

    wallet::WalletStore store_;
    wallet::WalletModel* model_ = nullptr;
    net::PriceFeed* prices_ = nullptr;
    wallet::Scheduler* scheduler_ = nullptr;
    wallet::PaymentBook* payments_ = nullptr;
    store::Ledger* ledger_ = nullptr;
    QTimer* priceTimer_ = nullptr;
    QTimer* statusTimer_ = nullptr; // single-shot throttle behind queueStatus()
    QTimer* timesTimer_ = nullptr;
    QTimer* ledgerRetry_ = nullptr; // the data folder is held: asks again until it is free
    int ledgerRetries_ = 0;
    std::function<void(wallet::NetworkStatus&)> statusForTest_;
    QTimer* givenRetry_ = nullptr;  // what was given out did not reach the file: asks again until it does
    bool givenOwed_ = false;
    int rowsBeforeLedger_ = 0;      // rows with an answer in the model when the database was opened
    bool settingsOnlyHere_ = false; // the settings in use are not what wallet.dat holds
    bool olderCopied_ = false;      // this session has made the way back from an older wallet.dat
    QHash<QString, QString> addressLabels_;
    QString ledgerClosedWhy_;       // why nothing can be kept, when the database is not open
    bool importingLabels_ = false;
    QList<QPointer<QThread>> copiesBeingWritten_; // a window closing waits for those
    bool running_ = false;          // between startServices() and stopServices()
    bool waitingForLedger_ = false; // the scans start when the database has handed over
    bool showingSaved_ = false;
    QDateTime savedAt_;
    // Taken at the first load or write and held until this controller is
    // destroyed, however many days the window stays open.
    std::unique_ptr<QLockFile> lock_;
    // SHA-256 of wallet.dat as this session last loaded or wrote it; empty when
    // it has no such file (none, deleted, unreadable). Settings alone are
    // written over an existing wallet.dat only while it still has this digest,
    // so nothing written there since by someone else is replaced.
    QByteArray fileDigest_;
    QString fileSetAside_;
};

} // namespace app
