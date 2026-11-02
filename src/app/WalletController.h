#pragma once

#include "wallet/Types.h"
#include "wallet/WalletStore.h"

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>

#include <memory>
#include <string>
#include <vector>

class QLockFile;
class QTimer;

namespace net {
class ElectrumClient;
class EthRpc;
class PriceFeed;
} // namespace net

namespace wallet {
class WalletModel;
class BtcScanner;
class EthScanner;
} // namespace wallet

namespace app {

// Owns the wallet's data and services: the store (xpubs + settings), the address
// model, the Electrum and Ethereum clients, the USD price feed and the two
// scanners. Pages read from it; the import dialog and the Settings page write
// through it. One process at a time writes a data folder (its wallet.lock): a
// second GitWallet on the same folder shows the wallet but saves nothing.
// GUI thread only.
class WalletController : public QObject
{
    Q_OBJECT
public:
    explicit WalletController(QObject* parent = nullptr);
    ~WalletController() override;

    bool hasWallet() const;

    // At startup: takes the data folder's lock if it is free, loads wallet.dat
    // if present and starts scanning. Returns false with an empty `error` when
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

    void refresh(); // prices + Ethereum balances now (Bitcoin is live)

    bool isScanning() const;  // either chain
    bool btcScanning() const; // the Bitcoin scan is not through (it never is while cut at the limit)
    bool ethScanning() const;
    bool addressLimitReached() const; // a Bitcoin or Ethereum window is cut at the row limit
    bool ethWrongChain() const;       // the Ethereum endpoint in use was found not to serve mainnet
    // When the last Ethereum pass ended that answered every address (local
    // time); invalid before the first one.
    QDateTime ethBalancesAt() const;
    // The last Ethereum pass failed or left addresses without an answer: the
    // ETH, USDT and USDC figures are as of ethBalancesAt() at best.
    bool ethRefreshFailing() const;
    // Every balance shown is whole and current: statusText() says "Up to date".
    bool balancesComplete() const;
    // One short line for the title bar chip. "Up to date" only when
    // balancesComplete(); otherwise it leads with what keeps it from that.
    QString statusText() const;
    QString electrumServer() const;
    QString ethEndpoint() const;

    double usdValue(wallet::Asset asset) const; // total of `asset` at the latest price
    double totalUsd() const;
    double totalUsd(quint32 wallet) const;      // one wallet's

signals:
    void walletChanged();  // loaded / imported / removed
    void statusChanged();  // statusText() or any of the scan states above changed
    void totalsChanged();  // balances or prices changed
    void scanFinished();   // both chains finished their initial scan
    // The scanners' problem() reports and the controller's own. `message` is
    // GitWallet's own sentence; `serverDetail` is what a server wrote, if
    // anything (one line, cleaned up), and is never part of `message`.
    void errorOccurred(wallet::Problem kind, const QString& message, const QString& serverDetail);

private:
    // --- implementation (WalletController.cpp may add private members) ---
    // What statusText() leads with, the most pressing first.
    enum class Status {
        NoWallet,
        Connecting,
        Limit,      // a window cut at the row limit
        WrongChain, // the Ethereum endpoint is off mainnet
        EthStale,   // the last Ethereum pass left addresses without an answer
        Scanning,
        Unchecked,  // nothing scans, yet a row has no answer
        Unscanned,  // an account, or a Bitcoin chain, has no rows at all
        UpToDate,
    };

    void startServices();
    void stopServices();
    void queueStatus();                   // throttled statusChanged() for progress and balances
    void onScanFinished(bool* chainDone); // a scanner finished its initial scan
    void onScannerProblem(wallet::Problem kind, const QString& message, const QString& serverDetail);
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
    net::ElectrumClient* electrum_ = nullptr;
    net::EthRpc* eth_ = nullptr;
    net::PriceFeed* prices_ = nullptr;
    wallet::BtcScanner* btcScanner_ = nullptr;
    wallet::EthScanner* ethScanner_ = nullptr;
    QTimer* priceTimer_ = nullptr;
    QTimer* statusTimer_ = nullptr; // single-shot throttle behind queueStatus()
    bool btcDone_ = false;
    bool ethDone_ = false;
    bool running_ = false;          // between startServices() and stopServices()
    bool scanFinishedSent_ = false; // scanFinished() already emitted for this start
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
