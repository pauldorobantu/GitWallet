#pragma once

#include "store/LedgerDb.h"

#include <QObject>
#include <QSet>
#include <QThread>

class QTimer;

namespace wallet {
class WalletModel;
}

namespace store {

// Keeps the database in step with the address model, off the GUI thread: what a run
// knows is there at the next start, before any server has answered. The rows it read
// reach the model a chunk at a time. What changes after that goes to the database a few
// seconds later, in one transaction, and at the latest when the window closes. A write
// that fails is tried again whole. All of it is a copy: the servers go on deciding what a
// figure is. If the database cannot be used the wallet runs as it would without one, and
// says so once.
class Ledger : public QObject
{
    Q_OBJECT
public:
    explicit Ledger(wallet::WalletModel* model, QObject* parent = nullptr);
    ~Ledger() override;

    // Opens the database at `path` and starts reading. A file that is not a database
    // under this key, or is damaged, is moved aside, never written to, and a new one is made.
    void open(const QString& path, const QByteArray& key, const QSet<quint32>& wallets);
    bool isOpen() const { return open_; }
    bool isLoading() const { return loading_; }

    void forgetWallet(quint32 wallet); // its rows leave the database, and any still on their way are dropped
    // Every row the model has and no other, with the next write.
    void rewriteAll();
    void savePrices(const QList<SavedPrice>& prices);
    void saveTimes(const QList<SavedTime>& times);

    void flush(); // what waits goes now
    // Writes what waits and closes: when this returns, the file is complete and let go.
    void close();
    // Closes and deletes the database with what SQLite and an earlier set-aside left
    // beside it. Returns the files that would not go.
    QStringList destroy(const QString& path);

    // Older databases moved aside, by file name.
    static QStringList keptAsideFiles(const QString& path);

signals:
    // Everything has been read. `savedAt`: when each network last had a full answer.
    void loaded(const QList<SavedPrice>& prices, const QList<SavedTime>& times);
    void problem(const QString& message);
    void setAside(const QString& file); // a new database took the place of this one

private:
    void noteRows(int first, int last);
    void noteRow(int row);
    void noteCleared();
    void schedule();
    void report(const QString& message); // at most once per open
    void noteWrite(bool ok, bool whole, const QString& why);
    template <typename Work>
    void onWorker(Work work);

    wallet::WalletModel* model_ = nullptr;
    QThread thread_;
    QObject* worker_ = nullptr;  // lives on thread_
    LedgerDb* db_ = nullptr;     // touched on thread_ only
    QTimer* timer_ = nullptr;
    QSet<int> dirty_;            // positions in the model
    QSet<quint32> wallets_;      // whose rows may still arrive
    bool rewrite_ = false;
    bool open_ = false;
    bool loading_ = false;
    bool reported_ = false;
    bool writesFailing_ = false; // the last write of rows failed: held back from saying they are newer
    int retryMs_ = 0;            // until the next try; 0: none has failed
    quint64 loadRun_ = 0;        // what is still on its way from an earlier open() is dropped
};

} // namespace store
