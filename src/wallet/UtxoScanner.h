#pragma once

#include "wallet/ChainScanner.h"

#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QSet>
#include <QStringList>

#include <memory>

class QTimer;

namespace net {
class ElectrumClient;
}

namespace wallet {

// Walks every account's receive (0) and change (1) chain on one UTXO network from
// index 0, deriving addresses publicly from the stored xpub, and asks its Electrum
// server about each scripthash, fetching the balance of every used address. A chain
// is scanned until `gap` consecutive unused addresses follow the last used one. When
// a later address becomes used, the window grows so the gap holds again — addresses
// another app creates from the same seed therefore show up by themselves. A row
// counts as checked once its status has answered, and for a used address its first
// balance too; a known balance stays on screen, checked, until a newer one replaces it.
//
// A connection follows at most kMaxLiveRows addresses live. While they all fit, all
// are followed. Past that the next unused addresses of each chain are followed and up to a
// quarter of the places go to addresses holding coins; the rest are read again and again in
// long rounds, at a pace that public servers answered without complaint
// (docs/ENDPOINTS.md). An address a server refuses to follow is read the same way.
//
// The wallet watches at most a number of addresses tied to the gaps, so a server
// that calls every address used cannot grow it without end: a scan cut short there
// stays unfinished, limitReached() turns true, and problem() says why.
class UtxoScanner : public ChainScanner
{
    Q_OBJECT
public:
    // Addresses one connection follows live, all wallets together: below what public
    // servers let one client subscribe by default (ElectrumX: 50,000 per session;
    // Fulcrum: 75,000 per IP). One that refuses earlier gets what it takes, and the rest
    // are read.
    static constexpr int kMaxLiveRows = 40000;
    // Addresses followed live or read in rounds, all wallets together.
    static constexpr int kMaxFollowedRows = 1'000'000;
    // Rows a scan watches at most, from the sum of its chains' gaps: it falls with them.
    static int rowLimitFor(qint64 gaps);
    // The status a history in blockchain.scripthash.get_history's form stands for: the
    // one a subscription would answer. 1 with `normalized` set, 0 without history, -1
    // when it is no history.
    static int statusOfHistory(const QJsonValue& history, QString* normalized);
    // Fewer live places than a connection has: a whole scan read in rounds, to compare
    // with one followed live. For tests, on a throw-away --data-dir.
    static void setLivePlacesForTest(int places);
    // The shortest round, and how soon what failed is asked again. For tests.
    static void setPacesForTest(int minRoundMs, int retryFirstMs, int readPauseMs);
    // Plain TCP to a test's own server.
    void usePlainTcpForTest();
    // The connection, which the history of this network's addresses shares.
    net::ElectrumClient* client() const { return client_; }

    UtxoScanner(Network network, WalletModel* model, QObject* parent = nullptr);
    ~UtxoScanner() override;

    void setAccounts(const QList<AccountKey>& accounts) override;
    void setSettings(const ScanSettings& settings) override;
    // (Re)starts once the client is ready. Rows already in the model are
    // adopted instead of derived again, but every status is checked again
    // from index 0: the window follows this run's server and gaps.
    void start() override;
    void stop() override;
    // A round begins now, if none is under way: what is followed live needs no asking.
    void refreshNow() override;
    void noteGiven(const AccountKey& key, quint32 index) override;
    void restoreTimes(const QDateTime& balancesAt, const QDateTime& noncesAt, const NonceSweep& sweep) override;

    bool hasAccounts() const override { return !accounts_.isEmpty(); }
    // True until every chain has its full gap checked and every balance has
    // answered; for good while a chain is stopped at the row limit.
    bool isScanning() const override;
    bool answered() const override;
    bool rowAnswered(const AddressRow& row) const override;
    bool limitReached() const override; // some chain's window reaches past the row limit
    bool connecting() const override;
    bool wrongChain() const override; // the last server tried is on another chain, and none is in use
    bool refreshFailing() const override; // a round left addresses without an answer
    QDateTime balancesAt() const override;
    QString serverName() const override;
    ScanTiers tiers() const override;
    // A balance of this address is queued, asked for, or to be asked for again.
    bool balanceOnItsWay(const QString& scripthash) const
    {
        return balanceQueued_.contains(scripthash) || balanceSeq_.contains(scripthash) || balanceRetry_.contains(scripthash);
    }
    int checkedCount() const;  // rows whose status, and balance if used, have answered
    int derivedCount() const;  // rows derived so far

private:
    // --- implementation (UtxoScanner.cpp may add private members) ---

    // One chain of one account of one wallet: receive (change 0) or change (change 1).
    struct Chain {
        quint32 wallet = 0;
        chain::ScriptType type = chain::ScriptType::P2WPKH;
        quint32 account = 0;
        quint32 change = 0;
        chain::HdPubKey node; // the account node's child `change`, derived once per start()
        int gap = 0;
        int floor = 0;        // rows [0, floor) are derived whatever the statuses: another wallet's, to replace
        int next = 0;         // next index to derive (or adopt from the model)
        int lastUsed = -1;    // highest index this run has seen used
        int savedUsed = -1;   // highest index the rows in the model say is used: only says what to follow first
        GivenOut given;       // receive rows given out: a payment comes to those
        int issued = 0;       // one past the highest of them: they count as used for the window
        mutable int nextEndOf = -2; // the last used index nextEnd() went by; given out since: asked again
        mutable int nextEndIs = 0;
        bool reuse = true;    // the rows already in the model came from this xpub

        int window() const;   // max(floor, the later of lastUsed + 1 and issued, + gap); may exceed what may be derived
        int target() const;   // window() capped at the end of the chain's indices
        int base() const;     // the start of the window derived whatever the row limit: max(floor, issued + gap)
    };

    enum class SubState : quint8 {
        Queued, // waiting in subQueue_ or readQueue_
        Sent,   // handed to the client, reply pending
        Retry,  // a first read failed, or the client dropped it; waits in retry_ for the next ready() or retry round
        Live,   // subscribed; changes arrive as notifications
        Rounds, // read in long rounds; its place is in rounds_
    };

    // One address this run follows, keyed by its lowercase scripthash.
    struct Tracked {
        SubState sub = SubState::Queued;
        bool confirmed = false; // status known since the last start()
        bool counted = false;   // in checked_: confirmed, and its row checked
        bool notified = false;  // a notification arrived while the subscribe reply was pending
        bool inRounds = false;  // has its place in rounds_
        bool forCoins = false;  // live for what it holds, not for being next: one of the places kept for those
        bool viaRead = false;   // this connection's server refused to follow it: read
        bool unanswered = false; // no server will say anything of it: the scan does not wait, and its row stays unchecked
        bool excused = false;    // this server will not read it, and its balance answers: a long history
        bool readRefused = false;     // this server refused its read, and none has answered since
        bool balanceAnswered = false; // this server has given its balance
        int serversTried = 0;    // moved on for it, with no answer at all from those
        QString status;         // that status while confirmed: "" no history, else the lowercase hash
    };

    // A chain by what tells it apart, cheap to hash: a lookup per status.
    struct ChainKey {
        quint32 wallet = 0;
        quint32 account = 0;
        quint8 type = 0;
        quint8 change = 0;
        bool operator==(const ChainKey& other) const = default;
    };
    friend size_t qHash(const ChainKey& key, size_t seed)
    {
        return qHashMulti(seed, key.wallet, key.account, key.type, key.change);
    }

    void adoptModelRows();
    void onClientReady();
    void onScripthashChanged(const QString& scripthash, const QString& status);
    void onModelCleared();
    void onSubscribeReply(quint64 generation, const QString& scripthash, int status, const QString& statusText,
                          const QString& error);
    void onReadReply(quint64 generation, const QString& scripthash, bool round, int status, const QString& statusText,
                     const QString& error);
    void onBalanceReply(quint64 generation, quint64 seq, const QString& scripthash, bool ok,
                        qint64 confirmed, qint64 unconfirmed, const QString& error);
    void settleRefusedRead(const QString& scripthash);
    bool droppedByClient(const QString& error) const; // the client's own "stopped", never a server's text

    void scheduleDerive();
    void deriveSlice();
    int adoptOrDerive(int chainIndex, int index); // 1 if it derived, 0 if it adopted a model row
    void track(const QString& scripthash);
    int rowLimit() const;                 // Bitcoin rows a scan follows at most, from the gaps
    qint64 reservedRows() const;          // rows the chains' base windows still have to derive
    bool mayDerive(const Chain& c) const; // its next row lies in its window and within the row limit

    void schedulePump();
    void pumpRequests();
    void sendSubscribe(const QString& scripthash);
    void sendRead(const QString& scripthash, bool round);
    bool takeReadToken();
    void nextRoundRead();
    void armRoundTimer();
    void finishRoundIfDone();
    void pauseReads();
    bool readsPaused() const;
    void noteRefusal(bool read);
    // Who gets one of the live places.
    bool tiered() const { return tiered_; } // not every address fits them
    void noteTiered();                // once a wallet outgrows them it stays that way for the run
    int placesNow() const;            // what this connection takes
    void rebalanceLive();             // every row's place, once, when who deserves one changed
    int hotAhead(const Chain& c) const; // unused addresses after the last used one a chain keeps live
    int nextEnd(const Chain& c) const;  // the last of those, past what was given out among them
    bool isNext(const Chain& c, int index) const; // one of those
    bool waitingForPayment(const Chain& c, int index, const AddressRow& row) const; // given out, and no history yet
    bool wantsLive(const Chain& c, int index, const AddressRow& row) const;
    bool mayGoLive(Tracked& tracked, const Chain& c, int index, const AddressRow& row); // there is a place for it: takes it
    void placeRow(const QString& scripthash, const Chain& c, int index, const AddressRow& row);
    void reconsider(const QString& scripthash); // its status or balance moved: its place may have too
    void reconsiderRange(int chainIndex, int from, int to);
    void leaveLive(const QString& scripthash, Tracked& tracked); // read in rounds from now on
    void makeRoom();                  // every place is taken, and what was given out needs one: another gives way
    void freeCoinPlace(Tracked& tracked);
    void enterRounds(const QString& scripthash, Tracked& tracked);
    void dropRowsPastWindows(); // once this run knows its windows
    void queueBalance(const QString& scripthash);
    void sendBalance(const QString& scripthash);

    void armRetry();
    void requeueFailed();

    void applyStatus(const QString& scripthash, bool used, const QString& status);
    void applyBalance(const QString& scripthash, qint64 confirmed, qint64 unconfirmed);
    void noteUsed(int chainIndex, int index);
    void reportLimits();
    void markConfirmed(const QString& scripthash);
    void recount(const QString& scripthash); // checked_ after its status or row changed
    int findChain(const AddressRow& row) const;

    bool busy() const;
    void checkIdle();
    void noteProgress();
    void emitProgress();
    // problem(), at most once per sentence and server wording, and at most
    // kMaxErrorsPerRun times per start(). `serverText` is cleaned up here.
    void report(Problem kind, const QString& message, const QString& serverText = QString());

    net::ElectrumClient* client_ = nullptr;
    QList<AccountKey> accounts_;
    int receiveGap_ = 200;
    int changeGap_ = 50;
    QStringList serverLines_;            // as Settings holds them; none: the built-in list
    bool running_ = false;

    QList<Chain> chains_;
    QHash<ChainKey, int> chainByKey_;    // positions in chains_: many wallets, a lookup per status
    QHash<QString, Tracked> tracked_;    // every row followed since the last start()
    QStringList subQueue_;               // scripthashes to subscribe, in derivation order
    QStringList readQueue_;              // scripthashes to read for the first time
    QStringList rounds_;                 // what long rounds read, in order
    qsizetype roundsHead_ = 0;           // where the round in progress has got to
    int roundsCount_ = 0;                // rows being read in rounds now (rounds_ keeps a place for those gone live)
    int roundReadsOut_ = 0;              // this round's reads awaiting a reply
    int readsOut_ = 0;                   // reads of any kind awaiting a reply
    int refusedInRound_ = 0;             // this round's reads refused, whatever their balances said
    int excusedRows_ = 0;                // Tracked::excused on this server
    int answersHere_ = 0;                // requests this connection has answered
    int refusedHere_ = 0;                // and refused
    int refusedReads_ = 0;               // reads refused since one last answered
    int refusedInARow_ = 0;              // requests of any kind refused since one last answered
    qint64 firstRefusedMs_ = 0;          // when the first of refusedReads_ came
    QString pauseServer_;                // who asked for the pause
    QString lastServer_;                 // of the last ready(): another means what it said goes for nothing
    bool roundInterrupted_ = false;      // the connection went during the round: its length says nothing
    bool answeredSaid_ = false;          // answered() has been true in this run
    QSet<QString> roundFailed_;          // this round's reads that failed, and that no balance has made up for
    bool lastRoundFailed_ = false;       // the last finished round left addresses without an answer
    bool forceRound_ = false;            // refreshNow() asked for one
    qint64 readsPausedUntilMs_ = 0;      // a server said no: no reads until then
    int readPauseMs_ = 0;                // the next pause; 0 = the first
    int liveCount_ = 0;                  // subscriptions sent or standing
    int coinPlaces_ = 0;                 // of those, how many are live for what they hold
    int placesHere_ = 0;                 // live places this connection takes: fewer once a server refused many
    int refusalsHere_ = 0;               // subscriptions it refused
    bool tiered_ = false;                // see tiered()
    bool wrongChain_ = false;            // see wrongChain()
    int hotReceive_ = 0;                 // hotAhead() of a receive chain, while tiered
    int readTokens_ = 0;                 // reads the pace allows right now
    QTimer* readTimer_ = nullptr;        // refills readTokens_ while reads wait
    qint64 roundStartMs_ = 0;
    qint64 roundEndMs_ = 0;
    int lastRoundSeconds_ = 0;
    QDateTime balancesAt_;               // see balancesAt()
    QDateTime roundStartedAt_;           // when the round in progress began
    QDateTime runStartedAt_;             // when this start() began
    QStringList balanceQueue_;           // scripthashes whose balance to fetch (sent first)
    QSet<QString> balanceQueued_;        // what balanceQueue_ holds
    QHash<QString, quint64> balanceSeq_; // newest get_balance per scripthash
    QStringList retry_;                  // failed first reads and dropped requests, queued again on ready() / retry round
    QSet<QString> balanceRetry_;         // failed balance fetches, queued again on ready() / retry round
    QSet<QString> reportedErrors_;       // sentence + normalized server text already emitted this run
    quint64 generation_ = 0;             // bumped by start() / stop(); older replies are dropped
    quint64 nextBalanceSeq_ = 0;
    qint64 lastProgressMs_ = 0;
    qint64 lastRetryMs_ = 0;             // when the last retry round ran
    int retryDelayMs_ = 0;               // delay of the next retry round; 0 = the first delay
    int outstanding_ = 0;                // requests of any run the client has not answered yet
    int subSent_ = 0;                    // this run's subscriptions and first reads awaiting a reply
    int balanceSent_ = 0;                // this run's balance requests awaiting a reply
    int checked_ = 0;                    // tracked rows counted (Tracked::counted)
    bool deriveScheduled_ = false;
    bool pumpScheduled_ = false;
    bool retryArmed_ = false;            // a retry round of this generation is posted
    bool progressArmed_ = false;         // a throttled progress() is posted
    bool progressDirty_ = false;
    bool idleReported_ = false;          // the final progress() of this busy spell is out
    bool finished_ = false;              // scanFinished() already emitted for this start()
    bool limitReported_ = false;         // reportLimits() already spoke for this start()
};

} // namespace wallet
