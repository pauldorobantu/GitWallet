#pragma once

#include "wallet/Types.h"

#include <QList>
#include <QObject>

class QTimer;

namespace net {
class EthRpc;
}

namespace wallet {

class WalletModel;

// Scans m/44'/60'/0'/0/i in windows: one JSON-RPC batch reads the nonces and one
// Multicall3 eth_call reads the ETH, USDT and USDC balances of the window. An
// address counts as used if its nonce > 0 or any balance > 0; the window grows
// until `gap` consecutive unused addresses follow the last used one. After the
// initial scan every known address is refreshed every `refreshSeconds`, and the
// window extends again if a new address got used.
class EthScanner : public QObject
{
    Q_OBJECT
public:
    EthScanner(net::EthRpc* rpc, WalletModel* model, QObject* parent = nullptr);

    void setAccounts(const QList<AccountKey>& accounts); // non-Ethereum accounts are ignored
    void setGap(int gap);
    void setRefreshSeconds(int seconds);
    void start();
    void stop();
    void refreshNow();

    bool isScanning() const;
    int checkedCount() const;
    int derivedCount() const;

signals:
    void progress(int checked, int derived);
    void scanFinished();
    void error(const QString& message);

private:
    // --- implementation (EthScanner.cpp may add private members) ---
    net::EthRpc* rpc_ = nullptr;
    WalletModel* model_ = nullptr;
    QList<AccountKey> accounts_;
    QTimer* refreshTimer_ = nullptr;
    int gap_ = 100;
    int refreshSeconds_ = 60;
    bool running_ = false;
};

} // namespace wallet
