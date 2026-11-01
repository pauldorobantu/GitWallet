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
    struct AccountState {
        AccountKey key;
        chain::HdPubKey chainNode;      // m/44'/60'/account'/0, derived once per start()
        qint64 lastUsed = -1;           // highest used index; -1 while none is known
        qint64 end = 0;                 // rows [0, end) exist in the model
        qint64 limit = qint64(1) << 31; // first index that failed to derive (2^31 = none)
    };
    struct Slot {
        int account = 0;  // position in acc_
        quint32 index = 0;
        QString address;  // filled in when the slot joins a chunk
    };

    void beginScan();
    void adoptRows(int account);
    bool deriveRow(const AccountState& account, quint32 index, AddressRow* out,
                   QString* problem) const;
    void ensureRows(int account, qint64 from, qint64 to, bool replace, bool enqueue);
    void extendWindow(int account, bool enqueue);
    void startPass();
    void nextChunk();
    void sendNonces();
    void onNonces(const QList<quint64>& nonces, const QList<bool>& ok, const QStringList& problems);
    void sendBalances();
    // otherChain: the eth_call proved its endpoint is not on Ethereum mainnet,
    // so nothing of the attempt (balances or nonces) is applied.
    void applyChunk(const QList<bool>& known, const QList<chain::Amount>& amounts,
                    const QStringList& problems, bool otherChain);
    void finishPass();
    bool windowChecked() const;
    bool acceptReply(quint64 gen, quint64 seq);
    void defer(void (EthScanner::*step)());
    void stopWork();
    void noteFailure(const QString& message);
    void report(const QString& message);
    void noteProgress();
    void emitProgress();
    void countRows(int* checked, int* derived) const;
    bool ownsAccount(quint32 account) const;

    net::EthRpc* rpc_ = nullptr;
    WalletModel* model_ = nullptr;
    QList<AccountKey> accounts_;
    QTimer* refreshTimer_ = nullptr;
    int gap_ = 100;
    int refreshSeconds_ = 60;
    bool running_ = false;

    QTimer* retryTimer_ = nullptr;
    QTimer* progressTimer_ = nullptr;
    QList<AccountState> acc_;  // the accounts of the current start()
    QList<AssetInfo> tokens_;  // ERC-20s read for every address
    QList<Slot> queue_;        // rows still to check in the current pass
    qsizetype queueHead_ = 0;
    QList<Slot> chunk_;        // rows of the attempt in flight (or awaiting its retry)
    QList<quint64> nonce_;     // (a) results of the attempt, parallel to chunk_
    QList<bool> nonceOk_;
    QStringList chunkErrors_;  // failure texts of the attempt
    int attempts_ = 0;         // failed attempts of the current chunk
    quint64 gen_ = 0;          // bumped by start() / stop(): older callbacks are ignored
    quint64 nextSeq_ = 0;
    quint64 pendingSeq_ = 0;   // the request currently awaited (0 = none)
    bool passActive_ = false;
    bool passFailed_ = false;  // some chunk of the current pass failed at least once
    bool finished_ = false;    // scanFinished() emitted for this start()
    QStringList reported_;     // error texts already emitted
    int lastChecked_ = -1;
    int lastDerived_ = -1;
};

} // namespace wallet
