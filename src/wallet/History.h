#pragma once

#include "chain/Transaction.h"
#include "wallet/Registry.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>
#include <vector>

namespace chain {

inline size_t qHash(const Hash256& hash, size_t seed = 0)
{
    return qHashBits(hash.data(), hash.size(), seed);
}

inline size_t qHash(const OutPoint& point, size_t seed = 0)
{
    return qHashMulti(seed, qHashBits(point.txid.data(), point.txid.size()), point.n);
}

} // namespace chain

namespace wallet {

class WalletModel;

// Whether what a transaction says still stands.
enum class TxSeen : quint8 {
    Alive,    // the servers list it, pending or in a block
    Replaced, // another transaction spends what it spent
    Dropped,  // it was listed, and is not any more; nothing is known to have taken its place
};

// An output of a transaction that pays one of the wallets' addresses.
struct OurOutput {
    quint32 n = 0;
    qint64 value = 0;
    QString scripthash;
};

// One transaction, as far as it touches the wallets. Inputs and outputs are all it takes to
// say what it did to each address, in whatever order the transactions arrive.
struct TxRecord {
    Network network{};
    chain::Hash256 txid{};
    qint32 height = 0;     // over 0: in that block; 0: pending; -1: pending, and spends a pending one
    qint64 time = 0;       // its block's, once that is known; until then when first seen here; 0: neither
    qint64 fee = -1;       // -1: not known yet
    qint32 vsize = 0;
    qint32 blockPos = -1;  // where its block holds it, by its proof; -1: not known
    qint64 outTotal = 0;   // what all its outputs carry
    TxSeen seen = TxSeen::Alive;
    bool proven = false;   // a Merkle proof ties it to a header of a chain with its work checked
    bool replaceable = false; // it says it may be replaced
    double usdThen = 0.0;  // one coin in USD around `time`; 0: not known
    qint64 usdThenAt = 0;  // when that price held
    chain::Hash256 replacedBy{};
    std::vector<chain::OutPoint> inputs;
    QVector<OurOutput> outputs;
};

// What a transaction did to one address.
struct TxEffect {
    int address = 0; // HistoryBook::addressAt()
    qint64 received = 0;
    qint64 spent = 0;
};

// A label, and a note of any length, that only this PC knows.
struct TxNote {
    QString label;
    QString note;
    bool isEmpty() const { return label.isEmpty() && note.isEmpty(); }
};

// One line of a file of labels, in the form other wallets read (BIP-329: JSON per line). A note
// and a network are additions of GitWallet's, which that form allows and they pass over.
struct LabelLine {
    bool isTransaction = true;
    chain::Hash256 txid{};
    QString network;  // of a transaction; empty: the first UTXO network
    QString address;  // of an address's label
    QString label;
    QString note;
};
QByteArray writeLabelLine(const LabelLine& line);
// What such a file says. A line of a kind GitWallet has no use for, or that cannot be read, is
// counted in `skipped`, and the rest still count.
QVector<LabelLine> readLabelLines(const QByteArray& file, int* skipped);

// An address whose history is complete, and up to what.
struct AddressHistory {
    QString scripthash;
    quint32 wallet = 0;
    QString address;
    QString status;
};

// What the database needs to catch up with a book.
struct HistoryDelta {
    QVector<TxRecord> txs;
    QVector<chain::Hash256> removedTxs;
    QVector<chain::Hash256> removedNotes; // of those: the ones no note has come back on since
    QVector<AddressHistory> addresses;
    QStringList removedAddresses;
    QMap<int, QByteArray> headers; // height -> the 80 bytes
    int dropHeadersFrom = -1;      // first: a chain gave way to another from there
    bool empty() const
    {
        return txs.isEmpty() && removedTxs.isEmpty() && addresses.isEmpty() && removedAddresses.isEmpty() && headers.isEmpty()
               && dropHeadersFrom < 0;
    }
};

// Every transaction of one network the wallets have been told of, and what each did to which
// address. A transaction goes in as it comes off the wire, in any order: what spends an output
// finds it whenever that arrives. One address's history is complete when every transaction its
// server listed is here, and its status says up to when. GUI thread only.
class HistoryBook : public QObject
{
    Q_OBJECT
public:
    struct AddressEntry {
        QString scripthash;
        quint32 wallet = 0; // 0: not known yet
        QString address;    // empty: not known yet
        QVector<int> txs;   // positions of what touches it
    };

    explicit HistoryBook(Network network, QObject* parent = nullptr);

    Network network() const { return network_; }
    void clear();

    int size() const { return int(txs_.size()); }
    int lineCount() const { return lines_; }     // those that touch an address: what gets a line
    int pendingCount() const { return pending_; } // not in a block, on a server's word alone
    const TxRecord& at(int position) const { return txs_.at(position); }
    const QVector<TxEffect>& effectsOf(int position) const { return effects_.at(position); }
    int find(const chain::Hash256& txid) const; // -1 if there is none

    int addressCount() const { return int(addresses_.size()); }
    const AddressEntry& addressAt(int id) const { return addresses_.at(id); }
    int findAddress(const QString& scripthash) const; // -1 if there is none

    // What the address has received less what it has spent, by what stands, and without `leftOut`.
    qint64 netOf(const QString& scripthash, const QSet<chain::Hash256>* leftOut = nullptr) const;
    // The status its history here is complete up to: what the servers' own status is compared with.
    QString statusOf(const QString& scripthash) const { return statuses_.value(scripthash); }
    const QHash<QString, QString>& statuses() const { return statuses_; }
    bool hasEffectOn(int position, const QString& scripthash) const;

    // A transaction parsed from what a server sent. `isOurs` says whether a script's hash is one
    // of the wallets' addresses, and `addressOf` what such a script reads as. Goes in, or if it
    // is here already picks up what it pays that was not known to be ours then. Returns its position.
    int add(const chain::Transaction& tx, int height, qint64 firstSeen, const std::function<bool(const QString&)>& isOurs);
    // Off the database: already whole, as it was.
    void addSaved(const QVector<TxRecord>& records);
    void addSavedAddresses(const QVector<AddressHistory>& addresses);
    void addSavedHeaders(const QMap<int, QByteArray>& headers);
    void addSavedNotes(const QHash<chain::Hash256, TxNote>& notes);

    // What a server says of it now. A height it had no proof at takes the proof back.
    void setHeight(int position, int height);
    // It is listed no more: Replaced if something else spends what it spent, else Dropped.
    void markGone(int position);
    void setProven(int position, bool proven, int blockPos = -1);
    void setBlockTime(int position, qint64 time);
    void setVsize(int position, int vsize);
    void setUsdThen(int position, double usd, qint64 at);
    // Its history is complete up to this status.
    void setStatus(const QString& scripthash, const QString& status);
    // Whose it is and how it reads, once a row says.
    void describeAddress(const QString& scripthash, quint32 wallet, const QString& address);
    // Blocks at or over this height went: what they held is proven no more.
    void unproveFrom(int height);

    // A wallet was removed: its addresses go, with what touches only those, and the notes on it.
    void forgetWallet(quint32 wallet);
    // Those of wallets no longer in the file go the same way.
    void keepWallets(const QSet<quint32>& wallets, quint32 nextWallet = 0);
    // What was replaced or dropped before this moment goes too, unless it has a note, or something
    // spends what it paid.
    void forgetGoneBefore(qint64 seconds);

    // Headers this chain has vouched for, by height, as they will be wanted at the next start.
    const QMap<int, QByteArray>& headers() const { return headers_; }
    void addHeader(int height, const QByteArray& raw);
    void dropHeadersFrom(int height);

    // Everything changed since the last call; the book forgets it was asked. What touches no
    // address yet stays out: it may be nothing, and if it is something it comes back.
    HistoryDelta takeDelta();
    void takeBack(const HistoryDelta& failed); // that write failed: what it held, as far as it still stands, goes next

    // Labels and notes stay for as long as a wallet holds what they are about.
    TxNote noteOf(const chain::Hash256& txid) const { return notes_.value(txid); }
    void setNote(const chain::Hash256& txid, const TxNote& note);
    const QHash<chain::Hash256, TxNote>& notes() const { return notes_; }

signals:
    void changed(); // lines of the table may have moved; comes as often as things do
    void added(int position); // a transaction not here before came off the wire, not off the database
    void spendFound(int position); // a transaction here turned out to spend one of the wallets' coins
    void statusSet(const QString& scripthash); // its history is complete up to a status
    void cleared();
    void positionsAboutToMove(); // still as they were
    void positionsMoved(); // what was at a position may be gone: nobody goes by one kept from before
    void notesChanged();

private:
    int addressId(const QString& scripthash); // makes one if needed
    void noteOutput(int position, int outputAt);
    void forget(const QSet<QString>& scripthashes);
    void removeRecords(const std::vector<char>& leaving, const QSet<int>& goingAddresses, const QSet<QString>& goingScripthashes);
    void noteInput(int position, const chain::OutPoint& point);
    void addEffect(int position, int address, qint64 received, qint64 spent);
    void settleConflicts(int position);
    bool isReplacedIn(int position, chain::Hash256* by) const;
    void touch(int position);
    void updateFee(int position);

    struct Owned { // an output paying one of the wallets' addresses
        int address = 0;
        qint64 value = 0;
    };

    Network network_{};
    QVector<TxRecord> txs_;
    QVector<QVector<TxEffect>> effects_; // by position
    QHash<chain::Hash256, int> positions_;
    QHash<chain::OutPoint, Owned> owned_;
    QMultiHash<chain::OutPoint, int> spenders_;
    QVector<AddressEntry> addresses_;
    QHash<QString, int> addressIds_;
    QHash<QString, QString> statuses_;
    QHash<chain::Hash256, TxNote> notes_;
    QHash<int, QHash<int, int>> effectIndex_; // a transaction with a great many effects: address -> where
    int lines_ = 0;
    int pending_ = 0;
    QSet<int> dirtyTxs_;
    QVector<chain::Hash256> removedTxs_;
    QSet<QString> dirtyAddresses_;
    QSet<QString> removedAddresses_;
    QMap<int, QByteArray> headers_;
    QMap<int, QByteArray> dirtyHeaders_;
    int dropHeadersFrom_ = -1;
};

} // namespace wallet
