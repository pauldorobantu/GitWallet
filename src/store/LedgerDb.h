#pragma once

#include "wallet/History.h"
#include "wallet/Types.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>

struct sqlite3;
struct sqlite3_stmt;

namespace store {

// One price as it was last read.
struct SavedPrice {
    QString asset;  // wallet::AssetInfo::key
    double usd = 0.0;
    QDateTime at;   // when the source says it holds, UTC
    QString source;
};

// What each network had a full answer for, last.
struct SavedTime {
    QString network; // wallet::NetworkInfo::id
    QDateTime at;
    QDateTime noncesAt; // Evm: the last pass that read every nonce
    wallet::NonceSweep sweep; // Evm: how far the next such reading has got
};

// What the wallets have shown so far, in an encrypted database beside wallet.dat
// (SQLCipher: AES-256 pages, each with its own authentication code, under a random key
// that only wallet.dat holds). It knows networks and assets by the names they go by in
// the list of coins, so a row stays good whatever the list adds around it, and a row of
// a network that is not on the list right now is left alone.
//
// What only this PC has, as a file set aside still gives it: by a network's name, as it is kept.
struct SavedLabels {
    QHash<QString, QHash<chain::Hash256, wallet::TxNote>> notes;
    QHash<QString, QString> addresses;
    bool read = false;  // the file could be read at all, under this key
    bool whole = false; // and to the end of those tables
    int count() const;
};

// One thread at a time. Every call blocks: on the worker thread of store::Ledger, or
// in a test.
class LedgerDb
{
public:
    enum class Open {
        Ready,
        NotOurs, // not a database, under another key, or damaged: never written over
        Failed,
    };

    LedgerDb();
    ~LedgerDb();
    LedgerDb(const LedgerDb&) = delete;
    LedgerDb& operator=(const LedgerDb&) = delete;

    Open open(const QString& path, const QByteArray& key, QString* why);
    void close();
    bool isOpen() const { return db_ != nullptr; }

    // Every row of these wallets, by wallet, chain and index, `chunk` rows at a time, and
    // drops the rows of any other wallet: nothing left behind by a wallet removed while the
    // database could not be written stays on disk, as far as it can be written now. False +
    // why when reading fails; readDamaged() then says if the file is at fault. Of an id below
    // `nextWallet`, which that wallet.dat gave out and no longer has, the removal finishes, labels
    // and all: `labelsGone` names them. Any other may be a wallet.dat from before it was added.
    bool loadRows(const QSet<quint32>& wallets, int chunk,
                  const std::function<void(QVector<wallet::AddressRow>&&)>& sink, QString* why,
                  quint32 nextWallet = 0, QStringList* labelsGone = nullptr);
    bool saveRows(const QVector<wallet::AddressRow>& rows, QString* why);
    bool replaceRows(const QVector<wallet::AddressRow>& rows, QString* why); // of the networks on the list, these and no others
    // Its rows go, with the labels of those addresses unless another wallet has them too:
    // `labelsGone` names them. Only a removal does that (`labelsGone` given): one a start finds
    // left over may be a wallet.dat from before it was added.
    bool removeWallet(quint32 wallet, QString* why, QStringList* labelsGone = nullptr);
    int rowCount(); // -1 when it cannot be read
    bool readDamaged() const { return readDamaged_; }
    // The last of the reads below that hand back what they got did not get to the end of it.
    bool readFailed() const { return readFailed_; }
    bool clearTimes(QString* why); // the rows they date are gone

    bool savePrices(const QList<SavedPrice>& prices, QString* why);
    QList<SavedPrice> loadPrices(QString* why);
    bool saveTimes(const QList<SavedTime>& times, QString* why);
    QList<SavedTime> loadTimes(QString* why);

    // What one network's book needs to catch up, in one transaction: a transaction never
    // stands without what says it is complete, or the reverse.
    bool saveHistory(wallet::Network network, const wallet::HistoryDelta& delta, QString* why);
    // One network's transactions, `chunk` at a time, then what else its book holds.
    bool loadHistory(wallet::Network network, int chunk, const std::function<void(QVector<wallet::TxRecord>&&)>& sink, QString* why);
    QVector<wallet::AddressHistory> loadAddressHistory(wallet::Network network, QString* why);
    QMap<int, QByteArray> loadHeaders(wallet::Network network, QString* why);
    QHash<chain::Hash256, wallet::TxNote> loadNotes(wallet::Network network, QString* why);
    bool saveNote(wallet::Network network, const chain::Hash256& txid, const wallet::TxNote& note, QString* why);
    // In one transaction. An empty one is none.
    bool saveNotes(wallet::Network network, const QHash<chain::Hash256, wallet::TxNote>& notes, QString* why);
    // An address's label, whichever network it is on.
    bool saveAddressLabel(const QString& address, const QString& label, QString* why);
    bool saveAddressLabels(const QHash<QString, QString>& labels, QString* why);
    QHash<QString, QString> loadAddressLabels(QString* why);

    // Whatever can still be read of labels and notes in a file set aside, which stays as it is.
    static SavedLabels salvageLabels(const QString& path, const QByteArray& key);
    static void setBusyTimeoutForTest(int ms);
    // Into this one, leaving what it holds already: that is newer.
    bool restoreLabels(const SavedLabels& labels, QString* why);

private:
    bool exec(const char* sql, QString* why);
    bool prepare(const char* sql, sqlite3_stmt** out, QString* why);
    bool migrate(QString* why);
    bool loadNames(QString* why);
    int nameId(const QString& text, QString* why); // -1 on failure
    bool writeRows(const QVector<wallet::AddressRow>& rows, QString* why);
    bool writeHistory(int network, const wallet::HistoryDelta& delta, QString* why);
    bool writeNotes(int network, const QHash<chain::Hash256, wallet::TxNote>& notes, QString* why);
    bool writeLabels(const QHash<QString, QString>& labels, QString* why);
    bool readEnded(int step, QString* why); // of a read that keeps what it got
    bool writeInTransaction(const std::function<bool(QString*)>& work, QString* why);
    bool writeLabelsDurably(const std::function<bool(QString*)>& work, QString* why);
    bool damaged() const; // what SQLite just failed on says the file is
    QString lastError() const;

    sqlite3* db_ = nullptr;
    QHash<QString, int> nameIds_;
    QHash<int, QString> names_;
    bool readDamaged_ = false;
    bool readFailed_ = false;
};

} // namespace store
