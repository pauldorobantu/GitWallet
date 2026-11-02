#pragma once

#include "chain/Transaction.h"
#include "wallet/Registry.h"

#include <QHash>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QVector>

class QTimer;

namespace net {
class ElectrumClient;
}

namespace wallet {

class HistoryBook;

// The headers of one UTXO network that GitWallet can vouch for itself, whatever a server is
// worth: up to the checkpoint in the list of coins each comes with a Merkle branch to the root
// of all of them, and past it each has to follow the one before, with the work its difficulty
// asks for and the difficulty the rule gives. A server has only its word for the rest: a
// transaction is believed in a block once a proof ties it to a header from here.
// GUI thread only.
class HeaderChain : public QObject
{
    Q_OBJECT
public:
    HeaderChain(const ChainProofs& proofs, net::ElectrumClient* client, HistoryBook* book, QObject* parent = nullptr);
    ~HeaderChain() override;

    void setProofsForTest(const ChainProofs& proofs) { proofs_ = proofs; }
    static void setRetryForTest(int ms); // 0: as it is
    void start(); // reads what the book kept, then follows the client
    void stop();

    // The highest height this chain vouches for: the checkpoint, or past it.
    int tip() const { return proofs_.checkpointHeight + int(above_.size()); }
    const chain::BlockHeader* at(int height) const; // nullptr while it does not stand here
    // Whether a block at that height is one this chain goes by, read or not: nothing takes back
    // what lies under the checkpoint.
    bool holds(int height) const { return height <= proofs_.checkpointHeight || aboveAt(height) != nullptr; }
    const ChainProofs& proofs() const { return proofs_; }
    // This connection's chain did not hold: nothing comes of asking, until it has been let go or
    // has had another chance.
    bool givenUp() const { return refused_; }
    // A header below the checkpoint comes when it comes (headerReady); one past it with sync().
    void ask(int height);
    // The one kept under the checkpoint may be from before the checkpoint moved over it: a branch to
    // the checkpoint says which stands. headerMoved if it was not that one.
    void askAgain(int height);
    void sync(); // ask for what lies past the tip

signals:
    void headerReady(int height);
    void headerMoved(int height); // what was kept there was another block
    void tipMoved();
    void reorged(int fromHeight); // the blocks from there on gave way to others
    // A server said what cannot be: nothing it said went in, and the caller may leave it.
    void misbehaved(const QString& why);
    // A server will not give what is asked, or names a tip it does not serve. Once per try: it is
    // asked again in a while, and the caller may leave it.
    void headersRefused();

private:
    using Candidate = QVector<chain::BlockHeader>;

    const chain::BlockHeader* aboveAt(int height) const;
    // Whether `header`, at `height`, follows from `prior`. `prior` gives the header at a height
    // below it, or nullptr where there is none.
    template <typename Prior>
    bool follows(const chain::BlockHeader& header, int height, Prior prior) const;
    bool parseChunk(const QJsonValue& result, Candidate* out) const;
    void onChunk(quint64 generation, int from, const QJsonValue& result, const QString& error);
    void onBranch(quint64 generation, int height, const QJsonValue& result, const QString& error);
    void lookForFork(quint64 generation);
    void onForkChunk(quint64 generation, int from, const QJsonValue& result, const QString& error);
    void appendAbove(const Candidate& chunk);
    void retryLater();
    void noteRefused();
    void misbehave(const QString& why);
    void sendAsks();

    ChainProofs proofs_;
    net::ElectrumClient* client_ = nullptr;
    HistoryBook* book_ = nullptr;
    QTimer* retry_ = nullptr;
    QHash<int, chain::BlockHeader> below_; // at or under the checkpoint, as they were asked for
    Candidate above_;                      // the headers right after the checkpoint, none left out
    QSet<int> wanted_;                     // below the checkpoint, to ask
    QSet<int> asked_;                      // on their way
    QSet<int> refusedAsks_;                // refused: a few at a time in a while, or of the next server
    QHash<int, chain::Hash256> askedAgain_; // those kept before, while the branch of the one that stands comes
    quint64 generation_ = 0;
    int forkDepth_ = 0;
    bool running_ = false;
    bool syncing_ = false;
    bool resync_ = false;    // the tip moved while one was on its way
    bool refused_ = false;   // this connection's chain does not hold: nothing more is asked of it for a while
};

} // namespace wallet
