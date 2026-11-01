#pragma once

#include "wallet/Types.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>

class QTimer;

namespace net {
class EthRpc;
}

namespace wallet {

class WalletModel;

// Scans m/44'/60'/0'/0/i in windows: one JSON-RPC batch reads the nonces and one
// Multicall3 eth_call reads the ETH, USDT and USDC balances of the window. An
// address counts as used if its nonce > 0 or any balance > 0; the window grows
// until `gap` consecutive unused addresses follow the last used one (at most
// 100,000 addresses per account). After the initial scan every known address
// is refreshed every `refreshSeconds`, and the window extends again if a new
// address got used. An endpoint whose eth_call shows it does not serve
// Ethereum mainnet, or answers it with something unusable, is passed over for
// a while (net::EthRpc::rejectEndpoint()).
class EthScanner : public QObject
{
    Q_OBJECT
public:
    // Rows derived at most over all accounts together (EthScanner.cpp).
    static constexpr qint64 kMaxFollowedRows = 200'000;

    EthScanner(net::EthRpc* rpc, WalletModel* model, QObject* parent = nullptr);

    void setAccounts(const QList<AccountKey>& accounts); // non-Ethereum accounts are ignored
    void setGap(int gap);
    void setRefreshSeconds(int seconds);
    void start();
    void stop();
    void refreshNow();

    bool isScanning() const;
    int checkedCount() const;  // rows whose nonce and every balance have answered
    int derivedCount() const;
    bool limitReached() const; // a window is cut at 100,000 addresses, or all of them at 200,000
    // The endpoint requests go to now was found not to serve Ethereum mainnet
    // (chain id, no Multicall3 contract, token contract missing).
    bool wrongChain() const;
    // When the last pass ended in which every queued row got a full answer
    // (local time); invalid before the first one. It describes the rows in the
    // model, so it survives stop() / start() and goes when the model is cleared.
    QDateTime balancesAt() const;
    // The most recent pass failed or left rows without an answer: what is shown
    // is as of balancesAt() at best. Turns true as soon as the pass in progress
    // gives up on a row (progress() then goes out even if no count moved) and
    // false only when a pass answers every row.
    bool refreshFailing() const;

signals:
    void progress(int checked, int derived);
    void scanFinished();
    void passFinished(); // at the end of every pass, successful or not
    // `message` is GitWallet's own sentence; `serverDetail` is what the
    // endpoint sent (one line, cleaned up, at most 200 characters) or empty.
    void problem(wallet::Problem kind, const QString& message, const QString& serverDetail);

private:
    // --- implementation (EthScanner.cpp may add private members) ---
    struct AccountState {
        AccountKey key;
        chain::HdPubKey chainNode;      // m/44'/60'/account'/0, derived once per start()
        qint64 lastUsed = -1;           // highest used index; -1 while none is known
        qint64 end = 0;                 // rows [0, end) exist in the model
        qint64 limit = qint64(1) << 31; // first index that failed to derive (2^31 = none)
        bool capReported = false;       // the window outgrew kMaxEthRows (said once per start())
    };
    struct Slot {
        int account = 0;  // position in acc_
        quint32 index = 0;
        QString address;  // filled in when the slot joins a chunk
    };
    // A problem to report, as problem() carries it.
    struct Issue {
        Problem kind = Problem::Failure;
        QString message;
        QString serverDetail;
    };

    void beginScan();
    void adoptRows(int account);
    bool deriveRow(const AccountState& account, quint32 index, AddressRow* out, QString* why) const;
    void ensureRows(int account, qint64 from, qint64 to, bool replace, bool enqueue);
    qint64 windowEnd(const AccountState& account) const; // the window is [0, windowEnd)
    bool cutAtLimit(const AccountState& account) const;  // the window wants rows past kMaxEthRows
    void extendWindowTo(int account, bool enqueue, qint64 upTo); // the part of the window below `upTo`
    void startPass();
    void deriveWindows(); // the start of a pass, a slice per event-loop turn
    void growWindows();   // during a pass, the same way
    void nextChunk();
    void sendNonces();
    void onNonces(const QList<quint64>& nonces, const QList<bool>& ok, const QList<Issue>& issues);
    void sendBalances();
    // otherChain: the eth_call proved its endpoint is not on Ethereum mainnet,
    // so nothing of the attempt (balances or nonces) is applied.
    void applyChunk(const QList<bool>& known, const QList<chain::Amount>& amounts,
                    const QList<Issue>& issues, bool otherChain);
    void finishPass();
    bool windowChecked() const;
    bool acceptReply(quint64 gen, quint64 seq);
    void defer(void (EthScanner::*step)());
    void stopWork();
    void refuseEndpoint(const QString& url, bool offMainnet);
    void noteFailure(const Issue& issue);
    void report(const Issue& issue);
    void noteProgress();
    void emitProgress();
    void countRows(int* checked, int* derived) const;
    bool ownsAccount(quint32 wallet, quint32 account) const;

    net::EthRpc* rpc_ = nullptr;
    WalletModel* model_ = nullptr;
    QList<AccountKey> accounts_;
    QSet<QPair<quint32, quint32>> owned_; // (wallet, account) of accounts_: asked once per row
    QTimer* refreshTimer_ = nullptr;
    int gap_ = 100;
    int refreshSeconds_ = 60;
    bool running_ = false;

    QTimer* retryTimer_ = nullptr;
    QTimer* progressTimer_ = nullptr;
    QList<AccountState> acc_;  // the accounts of the current start()
    int deriving_ = 0;         // the account deriveWindows() has got to
    QList<int> growing_;       // accounts whose window a used address moved, for growWindows()
    qint64 totalRows_ = 0;     // rows of all accounts' windows
    bool totalCut_ = false;    // a window was cut at the limit over all accounts
    QList<AssetInfo> tokens_;  // ERC-20s read for every address
    QList<Slot> queue_;        // rows still to check in the current pass
    qsizetype queueHead_ = 0;
    QList<Slot> chunk_;        // rows of the attempt in flight (or awaiting its retry)
    QList<quint64> nonce_;     // (a) results of the attempt, parallel to chunk_
    QList<bool> nonceOk_;
    QList<Issue> chunkIssues_; // failures of the attempt
    int attempts_ = 0;         // failed attempts of the current chunk
    quint64 gen_ = 0;          // bumped by start() / stop(): older callbacks are ignored
    quint64 nextSeq_ = 0;
    quint64 pendingSeq_ = 0;   // the request currently awaited (0 = none)
    bool passActive_ = false;
    bool passFailed_ = false;  // some chunk of the current pass failed at least once
    bool passSkipped_ = false; // some chunk of the current pass was given up: rows left unanswered
    bool finished_ = false;    // scanFinished() emitted for this start()
    QDateTime balancesAt_;     // see balancesAt()
    bool refreshFailing_ = false;
    QStringList offMainnet_;   // endpoints this start() found not to serve mainnet (full URLs)
    QStringList reported_;     // messages already emitted
    int lastChecked_ = -1;
    int lastDerived_ = -1;
};

} // namespace wallet
