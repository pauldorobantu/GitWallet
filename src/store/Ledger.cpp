#include "store/Ledger.h"

#include "wallet/History.h"
#include "wallet/WalletModel.h"

#include <QCoreApplication>
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
constexpr int kOwedRetryMs = 60 * 1000;

// The file beside `path` a database moved aside goes by.
QString asideName(const QString& path)
{
    return path + QStringLiteral(".unread-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

QStringList companions(const QString& path)
{
    return {path + QStringLiteral("-wal"), path + QStringLiteral("-shm")};
}

// Beside a file set aside whose labels and notes have yet to come over: the disk was full, say.
QString pendingMark()
{
    return QStringLiteral(".labels-pending");
}

} // namespace

Ledger::Ledger(wallet::WalletModel* model, QObject* parent) : QObject(parent), model_(model)
{
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(kFlushMs);
    connect(timer_, &QTimer::timeout, this, &Ledger::flush);

    owedTimer_ = new QTimer(this);
    owedTimer_->setInterval(kOwedRetryMs);
    connect(owedTimer_, &QTimer::timeout, this, &Ledger::bringOverOwed);

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

void Ledger::addBook(wallet::HistoryBook* book)
{
    books_.append(book);
    connect(book, &wallet::HistoryBook::changed, this, [this] {
        if (open_ && !loading_)
            schedule();
    });
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

// A mark that leads to no file is none. One whose file cannot be read just now, held by another
// program or under another key, says nothing of what that holds, and is all that leads back to it.
bool Ledger::bringOverPending(const QString& path, const QByteArray& key, QString* why)
{
    const QFileInfo info(path);
    // The newest first: what stands in the file by then stays, so the newer word is the one kept.
    const QStringList marks = info.dir().entryList({info.fileName() + QStringLiteral(".unread-*") + pendingMark()}, QDir::Files, QDir::Name | QDir::Reversed);
    bool owed = false;
    for (const QString& name : marks) {
        const QString mark = info.dir().filePath(name);
        const QString aside = mark.chopped(pendingMark().size());
        if (!QFile::exists(aside)) {
            QFile::remove(mark);
            continue;
        }
        const SavedLabels saved = LedgerDb::salvageLabels(aside, key);
        QFile::remove(aside + QStringLiteral("-shm"));
        if (!saved.read)
            continue;
        if (saved.count() == 0 || db_->restoreLabels(saved, why))
            QFile::remove(mark);
        else
            owed = true;
    }
    return owed;
}

// What can be edited on a line has to be there before the line is.
bool Ledger::handOverLabels(quint64 run, QString* why)
{
    const QPointer<Ledger> self(this);
    const QHash<QString, QString> labels = db_->loadAddressLabels(why);
    if (db_->readFailed())
        return false;
    for (wallet::HistoryBook* book : std::as_const(books_)) {
        const QHash<chain::Hash256, wallet::TxNote> notes = db_->loadNotes(book->network(), why);
        if (db_->readFailed())
            return false;
        QMetaObject::invokeMethod(this, [self, run, book, notes, this] {
            if (self && run == loadRun_) book->addSavedNotes(notes);
        }, Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(this, [this, self, run, labels] {
        if (self && run == loadRun_) emit labelsRead(labels);
    }, Qt::QueuedConnection);
    return true;
}

void Ledger::bringOverOwed()
{
    if (!open_ || loading_ || !labelsOwed_)
        return;
    const quint64 run = loadRun_;
    const QPointer<Ledger> self(this);
    onWorker([this, self, run, path = path_, key = key_] {
        QString why;
        if (bringOverPending(path, key, &why)) {
            qCInfo(lcLedger).noquote() << "labels and notes could not be brought over yet:" << why;
            return;
        }
        handOverLabels(run, &why);
        QMetaObject::invokeMethod(this, [this, self, run] {
            if (!self || run != loadRun_) return;
            labelsOwed_ = false;
            owedTimer_->stop();
            emit labelsOwedChanged(QString());
        }, Qt::QueuedConnection);
    });
}

void Ledger::open(const QString& path, const QByteArray& key, const QSet<quint32>& wallets, quint32 nextWallet)
{
    wallets_ = wallets;
    open_ = true;
    loading_ = true;
    reported_ = false;
    writesFailing_ = false;
    labelsFailing_ = false;
    labelsOwed_ = false;
    owedTimer_->stop();
    path_ = path;
    key_ = key;
    retryMs_ = 0;
    timer_->setInterval(kFlushMs);
    dirty_.clear();
    rewrite_ = false;
    historyWriting_.clear();
    unsavedNotes_.clear();
    unsavedLabels_.clear();
    // A scan that has run without a database: nothing it wrote was noted, and what it has
    // had answered is newer than what the file holds.
    const bool hadRows = model_ && model_->checkedCount() > 0;
    const quint64 run = ++loadRun_;
    const QPointer<Ledger> self(this);
    onWorker([this, self, run, hadRows, path, key, wallets, nextWallet] {
        QString why;
        bool owed = false;
        QString owedWhy;
        QStringList labelsGone;
        LedgerDb::Open opened = db_->open(path, key, &why);
        // Another key's, or damaged: never written over. wallet.dat was replaced, or this
        // file was. All but its labels and notes can be read again from the servers, and those are
        // read out of it as far as it gives them.
        const auto moveAside = [&] {
            qCWarning(lcLedger).noquote() << "the database is set aside:" << why;
            db_->close();
            const QString aside = asideName(path);
            // Before it moves: from there on all that leads back to it is this, whatever stops what
            // comes next, a full disk or the power. It goes when what it holds has come over.
            const QString mark = aside + pendingMark();
            QFile(mark).open(QIODevice::WriteOnly);
            if (!QFile::rename(path, aside)) {
                QFile::remove(mark);
                // Another program holds it: it stays as it is, WAL included, for the next start.
                why = QStringLiteral("it is damaged and could not be moved aside");
                opened = LedgerDb::Open::Failed;
                return;
            }
            // What the WAL holds is part of it.
            QFile::rename(path + QStringLiteral("-wal"), aside + QStringLiteral("-wal"));
            for (const QString& extra : companions(path))
                QFile::remove(extra);
            const SavedLabels saved = LedgerDb::salvageLabels(aside, key);
            // A reading leaves these behind, however little there was for them to hold.
            QFile::remove(aside + QStringLiteral("-shm"));
            if (QFileInfo(aside + QStringLiteral("-wal")).size() == 0)
                QFile::remove(aside + QStringLiteral("-wal"));
            opened = db_->open(path, key, &why);
            int recovered = saved.read ? saved.count() : -1;
            bool whole = saved.whole;
            bool over = saved.read && saved.count() == 0;
            if (saved.count() > 0) {
                QString lost = why;
                over = opened == LedgerDb::Open::Ready && db_->restoreLabels(saved, &lost);
                if (!over) {
                    // Not said to have come along, and asked for again.
                    qCWarning(lcLedger).noquote() << "labels and notes could not be brought over:" << lost;
                    recovered = -1;
                    whole = false;
                }
            }
            if (over)
                QFile::remove(mark);
            QMetaObject::invokeMethod(this, [this, self, run, hadRows, aside, recovered, whole] {
                if (!self || run != loadRun_) return;
                // What it handed over before it failed is no more to be trusted than the rest.
                // What a scan brought did not come from it, and goes into the new file.
                if (!hadRows && model_ && model_->size() > 0) model_->clear();
                for (wallet::HistoryBook* book : std::as_const(books_))
                    book->clear();
                dirty_.clear();
                rewrite_ = false;
                emit setAside(aside, recovered, whole);
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
            // Of what a window has shown, a history is the longest to read again: it goes last, on
            // top of the rows, and a part of it that does not load leaves those as they are.
            const auto history = [&]() {
                for (wallet::HistoryBook* book : std::as_const(books_)) {
                    const wallet::Network network = book->network();
                    const auto txs = [this, self, run, book](QVector<wallet::TxRecord>&& records) {
                        QMetaObject::invokeMethod(this, [self, run, book, records = std::move(records), this] {
                            if (self && run == loadRun_) book->addSaved(records);
                        }, Qt::QueuedConnection);
                    };
                    if (!db_->loadHistory(network, kChunk, txs, &why))
                        return false;
                    const QVector<wallet::AddressHistory> addresses = db_->loadAddressHistory(network, &why);
                    if (db_->readFailed())
                        return false;
                    const QMap<int, QByteArray> headers = db_->loadHeaders(network, &why);
                    if (db_->readFailed())
                        return false;
                    QMetaObject::invokeMethod(this, [self, run, book, addresses, headers, this] {
                        if (!self || run != loadRun_) return;
                        book->addSavedAddresses(addresses);
                        book->addSavedHeaders(headers);
                    }, Qt::QueuedConnection);
                }
                return true;
            };
            owed = bringOverPending(path, key, &owedWhy);
            if (handOverLabels(run, &why) && db_->loadRows(wallets, kChunk, chunk, &why, nextWallet, &labelsGone) && history()) {
                prices = db_->loadPrices(nullptr);
                times = db_->loadTimes(nullptr);
            } else if (db_->readDamaged()) {
                // The check passed, or could not run, and a page still fails.
                moveAside();
                if (opened == LedgerDb::Open::Ready) {
                    owed = bringOverPending(path, key, &owedWhy);
                    if (!handOverLabels(run, &why))
                        opened = LedgerDb::Open::Failed;
                }
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
                for (wallet::HistoryBook* book : std::as_const(books_))
                    book->clear();
                report(message);
                emit loaded({}, {});
            }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(this, [this, self, run, hadRows, prices, times, owed, owedWhy, labelsGone] {
            if (!self || run != loadRun_) return;
            loading_ = false;
            // A clear while it was reading: only now is the model all there is to write.
            rewrite_ = rewrite_ || hadRows;
            if (rewrite_ || !dirty_.isEmpty())
                schedule();
            if (!labelsGone.isEmpty())
                emit labelsRemoved(labelsGone);
            if (owed) {
                labelsOwed_ = true;
                owedTimer_->start();
                emit labelsOwedChanged(owedWhy);
            }
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

void Ledger::flushHistory()
{
    const QPointer<Ledger> self(this);
    for (wallet::HistoryBook* book : std::as_const(books_)) {
        // What comes next waits to hear of this: an address's status never goes in on top of a
        // write that failed with the transactions it stands for.
        if (historyWriting_.contains(book))
            continue;
        wallet::HistoryDelta delta = book->takeDelta();
        if (delta.empty())
            continue;
        historyWriting_.insert(book);
        const wallet::Network network = book->network();
        onWorker([this, self, book, network, delta = std::move(delta)] {
            QString why;
            const bool ok = db_->saveHistory(network, delta, &why);
            QMetaObject::invokeMethod(this, [this, self, book, ok, why, delta] {
                if (self) noteHistoryWrite(book, ok, why, delta);
            }, Qt::QueuedConnection);
        });
    }
}

void Ledger::noteHistoryWrite(wallet::HistoryBook* book, bool ok, const QString& why, const wallet::HistoryDelta& delta)
{
    historyWriting_.remove(book);
    if (!open_)
        return;
    if (ok) {
        if (!writesFailing_) {
            retryMs_ = 0;
            timer_->setInterval(kFlushMs);
        }
        schedule(); // what has come since
        return;
    }
    report(QStringLiteral("Saved transactions could not be written: %1").arg(why));
    book->takeBack(delta);
    retryMs_ = qMin(retryMs_ > 0 ? retryMs_ * 2 : kFlushMs, kMaxRetryMs);
    timer_->start(retryMs_);
}

void Ledger::flush()
{
    timer_->stop();
    if (open_ && !loading_) {
        flushHistory();
        flushLabels();
    }
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
    const QPointer<Ledger> self(this);
    onWorker([this, self, wallet] {
        QString why;
        QStringList labels;
        if (!db_->removeWallet(wallet, &why, &labels))
            qCWarning(lcLedger).noquote() << "rows of a removed wallet stay until the next start:" << why;
        if (!labels.isEmpty()) {
            QMetaObject::invokeMethod(this, [this, self, labels] {
                if (self) emit labelsRemoved(labels);
            }, Qt::QueuedConnection);
        }
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

bool Ledger::saveNote(wallet::Network network, const chain::Hash256& txid, const wallet::TxNote& note)
{
    QHash<chain::Hash256, wallet::TxNote> one;
    one.insert(txid, note);
    return saveNotes(network, one);
}

bool Ledger::saveNotes(wallet::Network network, const QHash<chain::Hash256, wallet::TxNote>& notes)
{
    if (!open_)
        return false;
    writeNotes(network, notes);
    return true;
}

bool Ledger::saveAddressLabel(const QString& address, const QString& label)
{
    QHash<QString, QString> one;
    one.insert(address, label);
    return saveAddressLabels(one);
}

bool Ledger::saveAddressLabels(const QHash<QString, QString>& labels)
{
    if (!open_)
        return false;
    writeLabels(labels);
    return true;
}

void Ledger::writeNotes(wallet::Network network, const QHash<chain::Hash256, wallet::TxNote>& notes)
{
    const quint64 serial = ++labelWrites_;
    for (auto it = notes.cbegin(); it != notes.cend(); ++it) {
        noteSerials_.insert(it.key(), serial);
        unsavedNotes_[network].remove(it.key());
    }
    const QPointer<Ledger> self(this);
    onWorker([this, self, network, notes, serial] {
        QString why;
        const bool ok = db_->saveNotes(network, notes, &why);
        QMetaObject::invokeMethod(this, [this, self, network, notes, serial, ok, why] {
            if (!self)
                return;
            if (!ok) {
                for (auto it = notes.cbegin(); it != notes.cend(); ++it) {
                    if (noteSerials_.value(it.key()) == serial)
                        unsavedNotes_[network].insert(it.key(), *it);
                }
            }
            noteLabelsWrite(ok, why);
        }, Qt::QueuedConnection);
    });
}

void Ledger::writeLabels(const QHash<QString, QString>& labels)
{
    const quint64 serial = ++labelWrites_;
    for (auto it = labels.cbegin(); it != labels.cend(); ++it) {
        labelSerials_.insert(it.key(), serial);
        unsavedLabels_.remove(it.key());
    }
    const QPointer<Ledger> self(this);
    onWorker([this, self, labels, serial] {
        QString why;
        const bool ok = db_->saveAddressLabels(labels, &why);
        QMetaObject::invokeMethod(this, [this, self, labels, serial, ok, why] {
            if (!self)
                return;
            if (!ok) {
                for (auto it = labels.cbegin(); it != labels.cend(); ++it) {
                    if (labelSerials_.value(it.key()) == serial)
                        unsavedLabels_.insert(it.key(), *it);
                }
            }
            noteLabelsWrite(ok, why);
        }, Qt::QueuedConnection);
    });
}

void Ledger::flushLabels()
{
    const auto notes = std::exchange(unsavedNotes_, {});
    for (auto it = notes.cbegin(); it != notes.cend(); ++it) {
        if (!it->isEmpty())
            writeNotes(it.key(), *it);
    }
    const QHash<QString, QString> labels = std::exchange(unsavedLabels_, {});
    if (!labels.isEmpty())
        writeLabels(labels);
}

void Ledger::noteLabelsWrite(bool ok, const QString& why)
{
    if (!open_)
        return;
    if (ok) {
        labelsFailing_ = false;
        return;
    }
    // Their own message: these exist nowhere else, whatever else has been said since this was opened.
    qCWarning(lcLedger).noquote() << "a label or note could not be saved:" << why;
    if (!labelsFailing_) {
        labelsFailing_ = true;
        emit labelsProblem(QStringLiteral("Labels and notes could not be written: %1. They are on screen and are being tried again, "
                                          "but until that works only this window has them.").arg(why));
    }
    schedule();
}

void Ledger::close()
{
    // Nothing more of what an open is reading is wanted, and what is on its way has run when this
    // comes back: what it says of itself is heard first, so that what failed goes with the rest.
    ++loadRun_;
    if (thread_.isRunning()) {
        QMetaObject::invokeMethod(worker_, [] {}, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    }
    flush();
    open_ = false;
    loading_ = false;
    labelsOwed_ = false;
    owedTimer_->stop();
    key_.clear();
    historyWriting_.clear();
    // BlockingQueuedConnection: everything queued before this has run when it returns.
    if (thread_.isRunning())
        QMetaObject::invokeMethod(worker_, [this] { db_->close(); }, Qt::BlockingQueuedConnection);
}

QStringList Ledger::destroy(const QString& path)
{
    dirty_.clear();
    rewrite_ = false;
    wallets_.clear();
    unsavedNotes_.clear();
    unsavedLabels_.clear();
    // All of it is about to go: nothing is written for that.
    for (wallet::HistoryBook* book : std::as_const(books_))
        book->takeDelta();
    close();
    unsavedNotes_.clear();
    unsavedLabels_.clear();
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
