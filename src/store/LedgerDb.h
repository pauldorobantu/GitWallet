#pragma once

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
    // why when reading fails; readDamaged() then says if the file is at fault.
    bool loadRows(const QSet<quint32>& wallets, int chunk,
                  const std::function<void(QVector<wallet::AddressRow>&&)>& sink, QString* why);
    bool saveRows(const QVector<wallet::AddressRow>& rows, QString* why);
    bool replaceRows(const QVector<wallet::AddressRow>& rows, QString* why); // of the networks on the list, these and no others
    bool removeWallet(quint32 wallet, QString* why);
    int rowCount(); // -1 when it cannot be read
    bool readDamaged() const { return readDamaged_; }
    bool clearTimes(QString* why); // the rows they date are gone

    bool savePrices(const QList<SavedPrice>& prices, QString* why);
    QList<SavedPrice> loadPrices(QString* why);
    bool saveTimes(const QList<SavedTime>& times, QString* why);
    QList<SavedTime> loadTimes(QString* why);

private:
    bool exec(const char* sql, QString* why);
    bool prepare(const char* sql, sqlite3_stmt** out, QString* why);
    bool migrate(QString* why);
    bool loadNames(QString* why);
    int nameId(const QString& text, QString* why); // -1 on failure
    bool writeRows(const QVector<wallet::AddressRow>& rows, QString* why);
    bool writeInTransaction(const std::function<bool(QString*)>& work, QString* why);
    bool damaged() const; // what SQLite just failed on says the file is
    QString lastError() const;

    sqlite3* db_ = nullptr;
    QHash<QString, int> nameIds_;
    QHash<int, QString> names_;
    bool readDamaged_ = false;
};

} // namespace store
