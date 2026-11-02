#include "store/Ledger.h"

#include "wallet/WalletModel.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace store {

namespace {

Q_STATIC_LOGGING_CATEGORY(lcLedger, "gitwallet.ledger")

constexpr int kFlushMs = 5000;
constexpr int kChunk = 5000;
constexpr int kMaxRetryMs = 5 * 60 * 1000;

// The file beside `path` a database moved aside goes by.
QString asideName(const QString& path)
{
    return path + QStringLiteral(".unread-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

QStringList companions(const QString& path)
{
    return {path + QStringLiteral("-wal"), path + QStringLiteral("-shm")};
}

} // namespace

Ledger::Ledger(wallet::WalletModel* model, QObject* parent) : QObject(parent), model_(model)
{
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(kFlushMs);
    connect(timer_, &QTimer::timeout, this, &Ledger::flush);

    if (model_) {
        connect(model_, &wallet::WalletModel::rowsAppended, this, &Ledger::noteRows);
        connect(model_, &wallet::WalletModel::rowChanged, this, &Ledger::noteRow);
        connect(model_, &wallet::WalletModel::cleared, this, &Ledger::noteCleared);
        connect(model_, &wallet::WalletModel::rowsDropped, this, &Ledger::noteCleared);
    }

    worker_ = new QObject;
    worker_->moveToThread(&thread_);
    db_ = new LedgerDb;
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.setObjectName(QStringLiteral("ledger"));
    thread_.start();
}

Ledger::~Ledger()
{
    close();
    thread_.quit();
    thread_.wait();
    delete db_; // its thread has ended: nothing else touches it now
}

template <typename Work>
void Ledger::onWorker(Work work)
{
    QMetaObject::invokeMethod(worker_, std::move(work), Qt::QueuedConnection);
}

void Ledger::open(const QString& path, const QByteArray& key, const QSet<quint32>& wallets)
{
    wallets_ = wallets;
    open_ = true;
    loading_ = true;
    reported_ = false;
    writesFailing_ = false;
    retryMs_ = 0;
    timer_->setInterval(kFlushMs);
    dirty_.clear();
    rewrite_ = false;
    // A scan that has run without a database: nothing it wrote was noted, and what it has
    // had answered is newer than what the file holds.
    const bool hadRows = model_ && model_->checkedCount() > 0;
    const quint64 run = ++loadRun_;
    const QPointer<Ledger> self(this);
    onWorker([this, self, run, hadRows, path, key, wallets] {
        QString why;
        LedgerDb::Open opened = db_->open(path, key, &why);
        // Another key's, or damaged: never written over. wallet.dat was replaced, or this
        // file was. Everything in it can be read again from the servers.
        const auto moveAside = [&] {
            qCWarning(lcLedger).noquote() << "the database is set aside:" << why;
            db_->close();
            const QString aside = asideName(path);
            if (!QFile::rename(path, aside)) {
                // Another program holds it: it stays as it is, WAL included, for the next start.
                why = QStringLiteral("it is damaged and could not be moved aside");
                opened = LedgerDb::Open::Failed;
                return;
            }
            for (const QString& extra : companions(path))
                QFile::remove(extra);
            opened = db_->open(path, key, &why);
            QMetaObject::invokeMethod(this, [this, self, run, hadRows, aside] {
                if (!self || run != loadRun_) return;
                // What it handed over before it failed is no more to be trusted than the rest.
                // What a scan brought did not come from it, and goes into the new file.
                if (!hadRows && model_ && model_->size() > 0) model_->clear();
                dirty_.clear();
                rewrite_ = false;
                emit setAside(aside);
            }, Qt::QueuedConnection);
        };
        if (opened == LedgerDb::Open::NotOurs)
            moveAside();

        QList<SavedPrice> prices;
        QList<SavedTime> times;
        if (opened == LedgerDb::Open::Ready) {
            const auto chunk = [this, self, run](QVector<wallet::AddressRow>&& rows) {
                QMetaObject::invokeMethod(this, [this, self, run, rows = std::move(rows)] {
                    if (!self || !model_ || run != loadRun_) return;
                    QVector<wallet::AddressRow> known;
                    known.reserve(rows.size());
                    for (const wallet::AddressRow& row : rows) {
                        if (wallets_.contains(row.wallet))
                            known.append(row);
                    }
                    model_->addSaved(known);
                }, Qt::QueuedConnection);
            };
            if (db_->loadRows(wallets, kChunk, chunk, &why)) {
                prices = db_->loadPrices(nullptr);
                times = db_->loadTimes(nullptr);
            } else if (db_->readDamaged()) {
                // The check passed, or could not run, and a page still fails.
                moveAside();
            } else {
                // The disk's doing, or a full one: nothing wrong with the file, which stays.
                opened = LedgerDb::Open::Failed;
            }
        }
        if (opened != LedgerDb::Open::Ready) {
            db_->close();
            const QString message = QStringLiteral("Saved balances are not being kept: the database could not be opened (%1).").arg(why);
            QMetaObject::invokeMethod(this, [this, self, run, hadRows, message] {
                if (!self || run != loadRun_) return;
                open_ = false;
                loading_ = false;
                // Part of what it read is no figure to go by.
                if (!hadRows && model_ && model_->size() > 0) model_->clear();
                report(message);
                emit loaded({}, {});
            }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(this, [this, self, run, hadRows, prices, times] {
            if (!self || run != loadRun_) return;
            loading_ = false;
            // A clear while it was reading: only now is the model all there is to write.
            rewrite_ = rewrite_ || hadRows;
            if (rewrite_ || !dirty_.isEmpty())
                schedule();
            emit loaded(prices, times);
        }, Qt::QueuedConnection);
    });
}

void Ledger::noteRows(int first, int last)
{
    // What the database itself handed over needs no writing back.
    if (!open_ || loading_ || rewrite_)
        return;
    for (int row = first; row <= last; ++row)
        dirty_.insert(row);
    schedule();
}

void Ledger::noteRow(int row)
{
    if (!open_ || rewrite_)
        return;
    dirty_.insert(row);
    schedule();
}

void Ledger::noteCleared()
{
    // Rows moved or went: positions say nothing any more, and no time in the file dates
    // what is left. A scanner whose rows stayed saves its own again at the next tick.
    dirty_.clear();
    rewriteAll();
    if (open_) {
        onWorker([this] {
            QString why;
            if (!db_->clearTimes(&why))
                qCWarning(lcLedger).noquote() << "scan times could not be cleared:" << why;
        });
    }
}

void Ledger::rewriteAll()
{
    if (!open_)
        return;
    rewrite_ = true;
    schedule();
}

void Ledger::schedule()
{
    if (!timer_->isActive())
        timer_->start();
}

void Ledger::report(const QString& message)
{
    if (reported_)
        return;
    reported_ = true;
    qCWarning(lcLedger).noquote() << message;
    emit problem(message);
}

void Ledger::flush()
{
    timer_->stop();
    // Part of the rows are in the model yet: loaded() comes back for what waits.
    if (!open_ || !model_ || loading_ || (dirty_.isEmpty() && !rewrite_))
        return;
    const QPointer<Ledger> self(this);
    const auto done = [this, self](bool ok, bool whole, const QString& why) {
        QMetaObject::invokeMethod(this, [this, self, ok, whole, why] {
            if (self) noteWrite(ok, whole, why);
        }, Qt::QueuedConnection);
    };
    if (rewrite_) {
        rewrite_ = false;
        dirty_.clear();
        QVector<wallet::AddressRow> rows;
        rows.reserve(model_->size());
        for (int i = 0; i < model_->size(); ++i)
            rows.append(model_->at(i));
        onWorker([this, done, rows = std::move(rows)] {
            QString why;
            const bool ok = db_->replaceRows(rows, &why);
            done(ok, true, why);
        });
        return;
    }
    // In the model's order, which is how they will want to be read.
    QList<int> positions = dirty_.values();
    std::sort(positions.begin(), positions.end());
    QVector<wallet::AddressRow> rows;
    rows.reserve(positions.size());
    for (const int i : std::as_const(positions)) {
        if (i >= 0 && i < model_->size())
            rows.append(model_->at(i));
    }
    dirty_.clear();
    onWorker([this, done, rows = std::move(rows)] {
        QString why;
        const bool ok = db_->saveRows(rows, &why);
        done(ok, false, why);
    });
}

void Ledger::noteWrite(bool ok, bool whole, const QString& why)
{
    if (!open_)
        return;
    if (ok) {
        // A small write that fits says nothing of the whole table still owed.
        if (whole || !rewrite_) {
            writesFailing_ = false;
            retryMs_ = 0;
            timer_->setInterval(kFlushMs);
        }
        return;
    }
    report(QStringLiteral("Saved balances could not be written: %1").arg(why));
    // Which rows it held is gone with the batch, so all of them go next time.
    writesFailing_ = true;
    rewrite_ = true;
    dirty_.clear();
    retryMs_ = qMin(retryMs_ > 0 ? retryMs_ * 2 : kFlushMs, kMaxRetryMs);
    timer_->start(retryMs_);
}

void Ledger::forgetWallet(quint32 wallet)
{
    wallets_.remove(wallet);
    if (!open_)
        return;
    onWorker([this, wallet] {
        QString why;
        if (!db_->removeWallet(wallet, &why))
            qCWarning(lcLedger).noquote() << "rows of a removed wallet stay until the next start:" << why;
    });
}

void Ledger::savePrices(const QList<SavedPrice>& prices)
{
    if (!open_)
        return;
    onWorker([this, prices] {
        QString why;
        if (!db_->savePrices(prices, &why))
            qCWarning(lcLedger).noquote() << "prices could not be saved:" << why;
    });
}

void Ledger::saveTimes(const QList<SavedTime>& times)
{
    // Those would date rows that are not in the file.
    if (!open_ || writesFailing_ || loading_)
        return;
    onWorker([this, times] {
        QString why;
        if (!db_->saveTimes(times, &why))
            qCWarning(lcLedger).noquote() << "scan times could not be saved:" << why;
    });
}

void Ledger::close()
{
    flush();
    open_ = false;
    loading_ = false;
    ++loadRun_;
    // BlockingQueuedConnection: everything queued before this has run when it returns.
    if (thread_.isRunning())
        QMetaObject::invokeMethod(worker_, [this] { db_->close(); }, Qt::BlockingQueuedConnection);
}

QStringList Ledger::destroy(const QString& path)
{
    dirty_.clear();
    rewrite_ = false;
    wallets_.clear();
    close();
    QStringList files{path};
    files += companions(path);
    const QDir folder = QFileInfo(path).dir();
    const QStringList aside = keptAsideFiles(path);
    for (const QString& name : aside)
        files.append(folder.filePath(name));
    QStringList left;
    for (const QString& file : std::as_const(files)) {
        if (QFile::exists(file) && !QFile::remove(file))
            left.append(file);
    }
    return left;
}

QStringList Ledger::keptAsideFiles(const QString& path)
{
    const QFileInfo info(path);
    return info.dir().entryList({info.fileName() + QStringLiteral(".unread-*")}, QDir::Files);
}

} // namespace store
