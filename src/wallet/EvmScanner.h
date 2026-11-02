#pragma once

#include "wallet/ChainScanner.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QSet>

class QTimer;

#include <QElapsedTimer>

namespace net {
class BudgetPool;
class EthRpc;
}

namespace wallet {

class WalletModel;

// Scans m/44'/60'/account'/0/i on one Evm network in windows: one Multicall3 eth_call
// reads the balances of every asset the network lists, and a JSON-RPC batch the nonces.
// An address counts as used if its nonce > 0 or any balance > 0; the window grows until
// `gap` consecutive unused addresses follow the last used one (at most 100,000 addresses
// per account). After the initial scan every known address is read again in passes,
// `refreshSeconds` apart; a pass longer than that lets the next unused addresses and
// those holding a balance go ahead of it every `refreshSeconds`, so that a payment shows
// as soon on a large wallet as on a small one. Balances cost public endpoints little and
// nonces a lot (docs/ENDPOINTS.md), so a nonce is read for a row without an answer, and
// otherwise in a sweep now and then, which goes on where it stopped if a run ends first: all
// it can still add is an address that received and sent everything between two sweeps. The
// window extends again if a new address got used, and the end of a sweep settles it: the
// rows past the last one used go. An address stays used once it held something, as one
// swept by signature keeps its nonce at 0, unless its nonce falls, which shows the earlier
// answer was false, or the endpoints change: then a sweep reads everything anew and only
// what the rows show counts. An endpoint whose eth_call shows it does not serve this
// network, or answers it with something unusable, is passed over for a while
// (net::EthRpc::rejectEndpoint()).
class EvmScanner : public ChainScanner
{
    Q_OBJECT
public:
    // Rows derived at most over all accounts together (EvmScanner.cpp).
    static constexpr qint64 kMaxFollowedRows = 1'000'000;

    EvmScanner(Network network, net::BudgetPool* budgets, WalletModel* model, QObject* parent = nullptr);
    ~EvmScanner() override;

    void setAccounts(const QList<AccountKey>& accounts) override;
    void setSettings(const ScanSettings& settings) override;
    void start() override;
    void stop() override;
    void refreshNow() override;
    void restoreTimes(const QDateTime& balancesAt, const QDateTime& noncesAt, const NonceSweep& sweep) override;
    // http://127.0.0.1 endpoints too, for a test's server on this PC.
    void allowLocalHttpForTest();
    // A refresh as short as a test can wait for.
    static void setMinRefreshForTest(int seconds);

    bool hasAccounts() const override { return !accounts_.isEmpty(); }
    bool isScanning() const override;
    bool answered() const override { return answered_; }
    int checkedCount() const;  // rows whose nonce and every balance have answered
    int derivedCount() const;
    bool limitReached() const override; // a window is cut at 100,000 addresses, or all of them at the limit over all accounts
    // The endpoint requests go to now was found not to serve this network
    // (chain id, no Multicall3 contract, token contract missing).
    bool wrongChain() const override;
    // When the last pass began in which every queued row got a full answer
    // (local time); invalid before the first one. It describes the rows in the
    // model, so it survives stop() / start() and goes when the model is cleared.
    QDateTime balancesAt() const override;
    // The same, for the last sweep that read every nonce: when it began.
    QDateTime noncesAt() const override { return noncesAt_; }
    NonceSweep nonceSweep() const override;
    // The most recent pass failed or left rows without an answer: what is shown
    // is as of balancesAt() at best. Turns true as soon as the pass in progress
    // gives up on a row (progress() then goes out even if no count moved) and
    // false only when a pass answers every row.
    bool refreshFailing() const override;
    QString serverName() const override;
    ScanTiers tiers() const override;

private:
    // --- implementation (EvmScanner.cpp may add private members) ---
    struct AccountState {
        AccountKey key;
        chain::HdPubKey chainNode;      // m/44'/60'/account'/0, derived once per start()
        qint64 lastUsed = -1;           // highest index whose nonce or balances show use; -1 while none is known
        qint64 savedUsed = -1;          // highest index the rows call used, shown or not: only says what to read first
        qint64 end = 0;                 // rows [0, end) exist in the model
        qint64 limit = qint64(1) << 31; // first index that failed to derive (2^31 = none)
        bool capReported = false;       // the window outgrew kMaxEthRows (said once per start())
    };
    struct Slot {
        int account = 0;  // position in acc_
        quint32 index = 0;
        QString address;  // filled in when the slot joins a chunk
        bool readNonce = false; // and this, as nextChunk() decides
    };
    // A problem to report, as problem() carries it.
    struct Issue {
        Problem kind = Problem::Failure;
        QString message;
        QString serverDetail;
    };

    void beginScan();
    void adoptRows();
    bool deriveRow(const AccountState& account, quint32 index, AddressRow* out, QString* why) const;
    void ensureRows(int account, qint64 from, qint64 to, bool replace, bool enqueue);
    qint64 windowEnd(const AccountState& account) const; // the window is [0, windowEnd)
    bool cutAtLimit(const AccountState& account) const;  // the window wants rows past kMaxEthRows
    void extendWindowTo(int account, bool enqueue, qint64 upTo); // the part of the window below `upTo`
    void startPass();
    void deriveWindows(); // the start of a pass, a slice per event-loop turn
    void growWindows();   // during a pass, the same way
    void nextChunk();
    void queueHotRound(); // the next unused addresses and the funded ones, ahead of a long pass
    bool hotRow(const AccountState& account, const AddressRow& row) const;
    void sendNonces();
    void onNonces(const QList<quint64>& nonces, const QList<bool>& ok, const QList<Issue>& issues);
    void sendBalances();
    // otherChain: the eth_call proved its endpoint is not on Ethereum mainnet,
    // so nothing of the attempt (balances or nonces) is applied.
    void applyChunk(const QList<bool>& known, const QList<chain::Amount>& amounts,
                    const QList<Issue>& issues, bool otherChain);
    void finishPass();
    void settleWindows(); // every nonce was just read: the rows say where the windows end
    bool noncesDue() const;
    void decideNonces();  // at the start of a pass
    void moveSweepOn();   // the rows up to the next one in the pass have had their turn
    void placeRestoredSweep();
    bool sweepWants(const Slot& slot) const;
    bool windowChecked() const;
    bool acceptReply(quint64 gen, quint64 seq);
    void defer(void (EvmScanner::*step)());
    void stopWork();
    void refuseEndpoint(const QString& url, bool offMainnet);
    void noteFailure(const Issue& issue);
    void report(const Issue& issue);
    void noteProgress();
    void emitProgress();
    void countRows(int* checked, int* derived) const;
    bool ownsAccount(quint32 wallet, quint32 account) const;

    net::EthRpc* rpc_ = nullptr;
    QList<AccountKey> accounts_;
    QSet<QPair<quint32, quint32>> owned_; // (wallet, account) of accounts_: asked once per row
    QTimer* refreshTimer_ = nullptr;
    int gap_ = 100;
    int refreshSeconds_ = 60;
    bool running_ = false;

    QStringList serverLines_;  // as Settings holds them; none: the built-in list
    QTimer* retryTimer_ = nullptr;
    QTimer* progressTimer_ = nullptr;
    QTimer* hotTimer_ = nullptr;
    QList<AccountState> acc_;  // the accounts of the current start()
    int deriving_ = 0;         // the account deriveWindows() has got to
    QList<int> growing_;       // accounts whose window a used address moved, for growWindows()
    qint64 totalRows_ = 0;     // rows of all accounts' windows
    bool totalCut_ = false;    // a window was cut at the limit over all accounts
    QList<AssetInfo> tokens_;  // the tokens read for every address
    QList<Slot> queue_;        // rows still to check in the current pass
    qsizetype queueHead_ = 0;
    QList<Slot> hotQueue_;     // rows going ahead of the pass
    int hotRows_ = 0;          // how many the last short round read
    QElapsedTimer passClock_;
    int lastPassSeconds_ = 0;
    int chunkLimit_ = 0;       // addresses per attempt now: halves when an endpoint fails, grows back as it answers
    bool noncesDecided_ = false;
    bool noncesWanted_ = false;   // the endpoints changed: a sweep begins that reads everything anew
    bool firstPass_ = false;
    bool configured_ = false;     // setSettings() has been called
    int nonceFailures_ = 0;       // batches at the smallest size in a row that missed a nonce
    int nonceLimit_ = 0;          // nonces per batch now: the limit an endpoint has shown it holds to, as far as known
    int nonceCeiling_ = 0;        // what a batch may grow back to, for as long as these endpoints stay
    int goodBatches_ = 0;         // since the ceiling last moved: it is tried again now and then
    QDateTime sweepEndedAt_;      // this run: the next one waits from here, whatever the last took
    QDateTime noncesAt_;          // see noncesAt()
    QDateTime passStartedAt_;
    bool sweepOpen_ = false;      // every nonce is being read
    bool sweepAsking_ = false;    // ... and this pass is asking
    bool sweepClears_ = false;    // see NonceSweep::clears
    QDateTime sweepFrom_;         // when it began
    int sweepAccount_ = 0;        // where it has got to: position in acc_, acc_.size() at the end
    quint32 sweepIndex_ = 0;
    QDateTime sweepRetryAt_;      // no endpoint gave nonces: not before this
    NonceSweep restoredSweep_;    // waits for beginScan() to place it among the accounts
    QSet<QPair<int, quint32>> nonceMissed_; // (account, index) of rows a sweep passed without a nonce: asked next pass
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
    bool answered_ = false;    // see ChainScanner::answered()
    QDateTime balancesAt_;     // see balancesAt()
    bool refreshFailing_ = false;
    QStringList offMainnet_;   // endpoints this start() found not to serve mainnet (full URLs)
    QStringList reported_;     // messages already emitted
    int lastChecked_ = -1;
    int lastDerived_ = -1;
};

} // namespace wallet
