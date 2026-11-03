#pragma once

#include "store/LedgerDb.h"

#include <QObject>
#include <QSet>
#include <QThread>

class QTimer;

namespace wallet {
class HistoryBook;
class WalletModel;
struct HistoryDelta;
}

namespace store {

// Keeps the database in step with the address model, off the GUI thread: what a run
// knows is there at the next start, before any server has answered. The rows it read
// reach the model a chunk at a time. What changes after that goes to the database a few
// seconds later, in one transaction, and at the latest when the window closes. A write
// that fails is tried again whole. Nearly all of it is a copy: the servers go on deciding what
// a figure is. Labels and notes are not, so they go first when read, at once when written, are
// tried again until they are in, and are saved from a file that has to be set aside. If the
// database cannot be used the wallet runs as it would without one, and says so once.
class Ledger : public QObject
{
    Q_OBJECT
public:
    explicit Ledger(wallet::WalletModel* model, QObject* parent = nullptr);
    ~Ledger() override;

    // The histories it keeps too. Before the first open().
    void addBook(wallet::HistoryBook* book);

    // Opens the database at `path` and starts reading. A file that is not a database
    // under this key, or is damaged, is moved aside, never written to, and a new one is made.
    // `nextWallet`: the id the next wallet would get, so that one below it not among `wallets` is
    // known to have been removed.
    void open(const QString& path, const QByteArray& key, const QSet<quint32>& wallets, quint32 nextWallet = 0);
    bool isOpen() const { return open_; }
    bool isLoading() const { return loading_; }
    // Labels and notes of a file set aside could not be written into this one. What is here would
    // then stand against them, so nothing about them should change until they have come over. That is
    // tried again in a while, or now by bringOverOwed().
    bool labelsOwed() const { return labelsOwed_; }
    void bringOverOwed();

    void forgetWallet(quint32 wallet); // its rows leave the database, and any still on their way are dropped
    // Every row the model has and no other, with the next write.
    void rewriteAll();
    void savePrices(const QList<SavedPrice>& prices);
    void saveTimes(const QList<SavedTime>& times);
    // A label or note goes now: only this PC has it. False while nothing can be kept. One that
    // does not go in is tried again until it does, and problem() says so.
    bool saveNote(wallet::Network network, const chain::Hash256& txid, const wallet::TxNote& note);
    bool saveAddressLabel(const QString& address, const QString& label);
    // Many, in one transaction.
    bool saveNotes(wallet::Network network, const QHash<chain::Hash256, wallet::TxNote>& notes);
    bool saveAddressLabels(const QHash<QString, QString>& labels);
    // What came in. Nothing but the list of them hangs on it: the transactions are read again.
    void savePayment(const wallet::Payment& payment);
    void setPaymentsRead(qint64 upTo);
    void deletePayment(qint64 id);
    void savePaymentState(const QHash<QString, QString>& changes);

    void flush(); // what waits goes now
    // Writes what waits and closes: when this returns, the file is complete and let go.
    void close();
    // Closes and deletes the database with what SQLite and an earlier set-aside left
    // beside it. Returns the files that would not go.
    QStringList destroy(const QString& path);

    // Older databases moved aside, by file name.
    static QStringList keptAsideFiles(const QString& path);

signals:
    // Before any row: what can be edited on a row has to be there with it.
    void labelsRead(const QHash<QString, QString>& addressLabels);
    void paymentsRead(const QList<wallet::Payment>& payments);
    void paymentStateRead(const QHash<QString, QString>& state);
    // Everything has been read. `savedAt`: when each network last had a full answer.
    void loaded(const QList<SavedPrice>& prices, const QList<SavedTime>& times);
    void problem(const QString& message);
    // Labels and notes are not going in. Its own: what is said of balances says nothing of those.
    void labelsProblem(const QString& message);
    // A new database took the place of this one. `recovered`: labels and notes that came along from
    // it, -1 if it could not be read at all. `whole`: and it had no others.
    void setAside(const QString& file, int recovered, bool whole);
    // Those of a wallet removed, which the window may not have had a row for.
    void labelsRemoved(const QStringList& addresses);
    // labelsOwed() moved. `why`: what stood in the way, if it is on.
    void labelsOwedChanged(const QString& why);

private:
    void noteRows(int first, int last);
    void noteRow(int row);
    void noteCleared();
    void schedule();
    void report(const QString& message); // at most once per open
    void noteWrite(bool ok, bool whole, const QString& why);
    void flushHistory();
    void noteHistoryWrite(wallet::HistoryBook* book, bool ok, const QString& why, const wallet::HistoryDelta& delta);
    void writeNotes(wallet::Network network, const QHash<chain::Hash256, wallet::TxNote>& notes);
    void writeLabels(const QHash<QString, QString>& labels);
    void flushLabels();
    void noteLabelsWrite(bool ok, const QString& why);
    bool bringOverPending(const QString& path, const QByteArray& key, QString* why); // on the worker; true: some are still owed
    bool handOverLabels(quint64 run, QString* why);                                   // on the worker
    template <typename Work>
    void onWorker(Work work);

    wallet::WalletModel* model_ = nullptr;
    QList<wallet::HistoryBook*> books_;
    QThread thread_;
    QObject* worker_ = nullptr;  // lives on thread_
    LedgerDb* db_ = nullptr;     // touched on thread_ only
    QTimer* timer_ = nullptr;
    QTimer* owedTimer_ = nullptr;
    QString path_;
    QByteArray key_;
    QSet<int> dirty_;            // positions in the model
    QSet<quint32> wallets_;      // whose rows may still arrive
    QSet<wallet::HistoryBook*> historyWriting_; // one write of a book at a time: a later one must not stand on one that failed
    // What did not go in, by what was last asked of each: a newer one in the queue goes on its own.
    QHash<wallet::Network, QHash<chain::Hash256, wallet::TxNote>> unsavedNotes_;
    QHash<QString, QString> unsavedLabels_;
    QHash<chain::Hash256, quint64> noteSerials_;
    QHash<QString, quint64> labelSerials_;
    quint64 labelWrites_ = 0;
    bool labelsFailing_ = false;
    bool labelsOwed_ = false;
    bool rewrite_ = false;
    bool open_ = false;
    bool loading_ = false;
    bool reported_ = false;
    bool writesFailing_ = false; // the last write of rows failed: held back from saying they are newer
    int retryMs_ = 0;            // until the next try; 0: none has failed
    quint64 loadRun_ = 0;        // what is still on its way from an earlier open() is dropped
};

} // namespace store
