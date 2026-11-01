#pragma once

#include "wallet/Types.h"
#include "wallet/WalletStore.h"

#include <QObject>
#include <QString>

#include <string>
#include <vector>

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
// through it. GUI thread only.
class WalletController : public QObject
{
    Q_OBJECT
public:
    explicit WalletController(QObject* parent = nullptr);
    ~WalletController() override;

    bool hasWallet() const;

    // At startup: loads wallet.dat if present and starts scanning. Returns false
    // with an empty `error` when there is no wallet yet, false with a message
    // when the file could not be read.
    bool loadExisting(QString* error);

    // Validates the mnemonic, derives the account xpubs, saves them (DPAPI) and
    // starts scanning. Never stores the words; the caller scrubs its own copies.
    bool importMnemonic(const std::vector<std::string>& words, const std::string& passphrase,
                        QString* error);

    // Deletes wallet.dat, stops scanning and clears the model.
    bool removeWallet(QString* error);

    // Saves new scan settings and restarts the scanners / clients with them.
    bool applySettings(const wallet::ScanSettings& settings, QString* error);

    const wallet::WalletStore& store() const { return store_; }
    wallet::WalletModel* model() const { return model_; }
    net::PriceFeed* prices() const { return prices_; }

    void refresh(); // prices + Ethereum balances now (Bitcoin is live)

    bool isScanning() const;
    QString statusText() const;   // one short line for the title bar chip
    QString electrumServer() const;
    QString ethEndpoint() const;

    double usdValue(wallet::Asset asset) const; // total of `asset` at the latest price
    double totalUsd() const;

signals:
    void walletChanged();  // loaded / imported / removed
    void statusChanged();  // statusText() or isScanning() changed
    void totalsChanged();  // balances or prices changed
    void scanFinished();   // both chains finished their initial scan
    void errorOccurred(const QString& message);

private:
    // --- implementation (WalletController.cpp may add private members) ---
    void startServices();
    void stopServices();
    void queueStatus();                   // throttled statusChanged() for progress and balances
    void onScanFinished(bool* chainDone); // a scanner finished its initial scan
    void onScannerError(const QString& message);

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
    // wallet.dat is the file this session last loaded or wrote; only then may
    // settings alone be written over an existing one.
    bool fileLoaded_ = false;
};

} // namespace app
