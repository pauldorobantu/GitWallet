#pragma once

#include "wallet/Types.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>

namespace net {
class ElectrumClient;
}

namespace wallet {

class WalletModel;

// Walks every Bitcoin account's receive (0) and change (1) chain from index 0,
// deriving addresses publicly from the stored xpub, subscribing each scripthash
// on the Electrum server and fetching the balance of every used address. A
// chain is scanned until `gap` consecutive unused addresses follow the last used
// one. When a later address becomes used (a subscription notification), the
// window grows so the gap holds again — addresses another app creates from the
// same seed therefore show up by themselves.
class BtcScanner : public QObject
{
    Q_OBJECT
public:
    BtcScanner(net::ElectrumClient* client, WalletModel* model, QObject* parent = nullptr);

    void setAccounts(const QList<AccountKey>& accounts); // non-Bitcoin accounts are ignored
    void setGaps(int receiveGap, int changeGap);
    void start(); // (re)starts once the client is ready; rows already in the model are reused
    void stop();

    bool isScanning() const;  // true until every chain has its full gap checked
    int checkedCount() const; // Bitcoin rows with a known status
    int derivedCount() const; // Bitcoin rows derived so far

signals:
    void progress(int checked, int derived);
    void scanFinished(); // initial scan complete; live updates continue
    void error(const QString& message);

private:
    // --- implementation (BtcScanner.cpp may add private members) ---
    net::ElectrumClient* client_ = nullptr;
    WalletModel* model_ = nullptr;
    QList<AccountKey> accounts_;
    int receiveGap_ = 200;
    int changeGap_ = 50;
    bool running_ = false;
};

} // namespace wallet
