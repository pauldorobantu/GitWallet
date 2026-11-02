#include "app/ScaleTest.h"

#include "app/WalletController.h"
#include "net/PriceFeed.h"
#include "store/LedgerDb.h"
#include "ui/AddressTableModel.h"
#include "ui/HistoryTableModel.h"
#include "ui/MainWindow.h"
#include "ui/pages/AddressesPage.h"
#include "ui/pages/HistoryPage.h"
#include "wallet/History.h"
#include "wallet/Registry.h"
#include "wallet/UtxoHistory.h"
#include "wallet/WalletModel.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHeaderView>
#include <QLineEdit>
#include <QTableView>
#include <QVector>

#include <algorithm>
#include <utility>

namespace app {

namespace {

// Everything waiting on the event loop.
void settle()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

// A transaction paying one row, and with it spending what paid the one before: what a large
// wallet is mostly made of.
wallet::TxRecord txAt(int i, const QVector<QString>& scripthashes, wallet::Network network)
{
    wallet::TxRecord tx;
    tx.network = network;
    for (int byte = 0; byte < 4; ++byte)
        tx.txid[size_t(byte)] = uint8_t(i >> (8 * byte));
    tx.txid[31] = 0x77;
    tx.height = 900000 - i / 3;
    tx.time = 1790000000 - i * 200;
    tx.vsize = 141;
    tx.blockPos = i % 3;
    tx.outTotal = 10000 + i;
    tx.proven = true;
    tx.usdThen = 60000.0 + i % 1000;
    tx.usdThenAt = tx.time;
    chain::OutPoint point;
    point.txid = tx.txid;
    point.txid[30] = 1; // nobody's
    tx.inputs.push_back(point);
    if (i % 2 == 1) {
        // Spends what the one before paid.
        chain::OutPoint previous;
        for (int byte = 0; byte < 4; ++byte)
            previous.txid[size_t(byte)] = uint8_t((i - 1) >> (8 * byte));
        previous.txid[31] = 0x77;
        tx.inputs.push_back(previous);
    }
    wallet::OurOutput output;
    output.n = 0;
    output.value = tx.outTotal;
    output.scripthash = scripthashes.at(i % scripthashes.size());
    tx.outputs.append(output);
    return tx;
}

wallet::AddressRow rowAt(int i)
{
    const wallet::NetworkInfo& bitcoin = *wallet::findNetwork(QStringLiteral("bitcoin"));
    const wallet::NetworkInfo& ethereum = *wallet::findNetwork(QStringLiteral("ethereum"));
    wallet::AddressRow row;
    row.wallet = 1 + i / 850;
    row.index = i % 850;
    row.checked = true;
    if (i % 4 == 3) {
        row.network = ethereum.network;
        row.type = chain::ScriptType::EthAccount;
        row.address = QStringLiteral("0xtestaddress%1x%2").arg(row.wallet).arg(row.index);
        if (i % 400 == 3) {
            row.setBalance(ethereum.tokens.first(), chain::Amount::fromU64(1000000));
            row.used = true;
        }
    } else {
        row.network = bitcoin.network;
        row.address = QStringLiteral("bc1qtestaddress%1x%2").arg(row.wallet).arg(row.index);
        row.scripthash = QStringLiteral("%1").arg(qulonglong(row.wallet) * 1000000 + row.index, 64, 16, QLatin1Char('0'));
        if (i % 100 == 0) {
            row.confirmed = 1000;
            row.used = true;
        }
    }
    return row;
}

} // namespace

QString runWindowScaleTest(ui::MainWindow* window, int rows)
{
    wallet::WalletModel* model = window->controller()->model();
    QString report;
    const auto say = [&report](const QString& what, qint64 ms) {
        report += QStringLiteral("%1 ms  %2\n").arg(ms, 7).arg(what);
    };
    QElapsedTimer clock;
    QElapsedTimer whole;
    window->showPage(QStringLiteral("addresses"));
    settle();

    // As the database hands them over: a chunk per event-loop turn, with the page on show.
    constexpr int kChunk = 5000;
    whole.start();
    qint64 slowest = 0;
    for (int first = 0; first < rows; first += kChunk) {
        QVector<wallet::AddressRow> chunk;
        chunk.reserve(kChunk);
        for (int i = first; i < std::min(first + kChunk, rows); ++i)
            chunk.append(rowAt(i));
        clock.restart();
        model->addSaved(chunk);
        settle();
        slowest = std::max(slowest, clock.elapsed());
    }
    say(QStringLiteral("%1 rows reach the Addresses page, the whole time").arg(rows), whole.elapsed());
    say(QStringLiteral("the slowest chunk of 5,000 (the window does not answer this long)"), slowest);
    // Far from what these show, and well inside what a window can take.
    if (slowest > 2000)
        report += QStringLiteral("FAIL  a chunk held the window for %1 ms\n").arg(slowest);

    auto* page = window->findChild<ui::AddressesPage*>();
    QTableView* table = page ? page->findChild<QTableView*>() : nullptr;
    if (table) {
        clock.restart();
        window->grab();
        say(QStringLiteral("drawing the page"), clock.elapsed());
        clock.restart();
        table->sortByColumn(ui::AddressTableModel::ColUsd, Qt::DescendingOrder);
        say(QStringLiteral("sorting by USD"), clock.elapsed());
        clock.restart();
        table->sortByColumn(ui::AddressTableModel::ColPath, Qt::AscendingOrder);
        say(QStringLiteral("sorting by path"), clock.elapsed());
    }
    if (QLineEdit* search = page ? page->findChild<QLineEdit*>() : nullptr) {
        // The search waits for the typing to stop, past so many rows. What it then shows is
        // checked against the rows themselves.
        const auto timed = [&](const QString& text, const QString& what) {
            clock.restart();
            search->setText(text);
            settle();
            QElapsedTimer waited;
            waited.start();
            while (waited.elapsed() < 300)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            const qint64 took = clock.elapsed();
            say(what + QStringLiteral(" (250 ms of it waiting for the typing to stop)"), took);
            if (took > 10000)
                report += QStringLiteral("FAIL  %1 took %2 ms\n").arg(what).arg(took);
            if (!table)
                return;
            int expected = 0;
            for (int i = 0; i < model->size(); ++i) {
                const wallet::AddressRow& row = model->at(i);
                expected += text.isEmpty() || row.address.contains(text, Qt::CaseInsensitive) || row.path().contains(text, Qt::CaseInsensitive) ? 1 : 0;
            }
            if (table->model()->rowCount() != expected)
                report += QStringLiteral("FAIL  %1 shows %2 rows, not %3\n").arg(what).arg(table->model()->rowCount()).arg(expected);
        };
        timed(QStringLiteral("x250x"), QStringLiteral("searching what few rows match"));
        timed(QString(), QStringLiteral("clearing the search"));
        timed(QStringLiteral("7"), QStringLiteral("searching what a third of the rows match"));
        timed(QString(), QStringLiteral("clearing that search"));
        timed(QStringLiteral("bc1q"), QStringLiteral("searching what most rows match, no path can"));
        timed(QString(), QStringLiteral("clearing that search"));
    }
    if (auto* combo = page ? page->findChild<QComboBox*>(QStringLiteral("stateFilter")) : nullptr) {
        clock.restart();
        combo->setCurrentIndex(1);
        settle();
        say(QStringLiteral("showing the funded rows only"), clock.elapsed());
        if (table && table->model()->rowCount() != model->fundedCount())
            report += QStringLiteral("FAIL  the funded rows are %1, not %2\n").arg(table->model()->rowCount()).arg(model->fundedCount());
        clock.restart();
        combo->setCurrentIndex(0);
        settle();
        say(QStringLiteral("showing all again"), clock.elapsed());
        if (table && table->model()->rowCount() != model->size())
            report += QStringLiteral("FAIL  all rows are %1, not %2\n").arg(table->model()->rowCount()).arg(model->size());
    }
    if (page && table) {
        // A place narrows it in one pass, and leaving lets go of that as the page next shows.
        const wallet::Asset token = wallet::findNetwork(QStringLiteral("ethereum"))->tokens.first();
        int holding = 0;
        for (int i = 0; i < model->size(); ++i) {
            if (model->at(i).wallet == 1 && !model->at(i).amount(token).isZero())
                ++holding;
        }
        clock.restart();
        page->showPlace(1, token);
        settle();
        const qint64 narrowing = clock.elapsed();
        say(QStringLiteral("opening a coin of a wallet on its addresses"), narrowing);
        if (narrowing > 10000)
            report += QStringLiteral("FAIL  a place took %1 ms\n").arg(narrowing);
        if (table->model()->rowCount() != holding)
            report += QStringLiteral("FAIL  the place shows %1 rows, not %2\n").arg(table->model()->rowCount()).arg(holding);
        clock.restart();
        window->showPage(QStringLiteral("settings"));
        page->clearPlace();
        settle();
        const qint64 leaving = clock.elapsed();
        say(QStringLiteral("leaving it for another page"), leaving);
        if (leaving > 1000)
            report += QStringLiteral("FAIL  leaving a place for another page took %1 ms\n").arg(leaving);
        clock.restart();
        window->showPage(QStringLiteral("addresses"));
        settle();
        const qint64 whole = clock.elapsed();
        say(QStringLiteral("coming back to all of them"), whole);
        if (whole > 10000)
            report += QStringLiteral("FAIL  coming back took %1 ms\n").arg(whole);
        if (table->model()->rowCount() != model->size())
            report += QStringLiteral("FAIL  all rows are %1, not %2, after a place\n").arg(table->model()->rowCount()).arg(model->size());
    }
    if (page) {
        const QString path = window->controller()->store().dataDir() + QStringLiteral("/scale-export.csv");
        QString error;
        clock.restart();
        if (!page->exportCsv(path, &error))
            report += QStringLiteral("FAIL  the export: %1\n").arg(error);
        say(QStringLiteral("exporting every row to a file"), clock.elapsed());
        QFile::remove(path);
    }

    // What a scan does: rows answering one at a time, the page on show.
    clock.restart();
    for (int i = 0; i < 2000; ++i) {
        wallet::AddressRow row = model->at(i * 100 % model->size());
        row.nonce += 1;
        row.confirmed += row.isUtxo() ? 1 : 0;
        model->upsert(row);
    }
    settle();
    say(QStringLiteral("2,000 rows change, the model and the sorted table"), clock.elapsed());

    // What every price refresh does to the table, sorted by what moves.
    QList<net::PriceQuote> quotes = window->controller()->prices()->quotes();
    if (quotes.isEmpty()) {
        net::PriceQuote quote;
        quote.asset = wallet::findNetwork(QStringLiteral("bitcoin"))->coin;
        quote.usd = 70000.0;
        quote.source = QStringLiteral("test");
        quotes.append(quote);
    }
    for (net::PriceQuote& quote : quotes) {
        quote.usd *= 1.01;
        quote.at = QDateTime::currentDateTimeUtc();
    }
    clock.restart();
    window->controller()->prices()->restore(quotes);
    settle();
    say(QStringLiteral("prices move, the table sorted by path"), clock.elapsed());
    if (table) {
        table->sortByColumn(ui::AddressTableModel::ColUsd, Qt::DescendingOrder);
        settle();
        for (net::PriceQuote& quote : quotes) {
            quote.usd *= 1.01;
            quote.at = quote.at.addSecs(1);
        }
        clock.restart();
        window->controller()->prices()->restore(quotes);
        settle();
        say(QStringLiteral("prices move, the table sorted by USD"), clock.elapsed());
        table->sortByColumn(ui::AddressTableModel::ColPath, Qt::AscendingOrder);
        settle();
    }

    for (const QString& name : {QStringLiteral("portfolio"), QStringLiteral("wallets"), QStringLiteral("addresses"), QStringLiteral("receive")}) {
        clock.restart();
        window->showPage(name);
        settle();
        window->grab();
        say(QStringLiteral("opening %1").arg(name), clock.elapsed());
    }

    // The history of all that: a good many more transactions than addresses.
    auto* history = window->findChild<ui::HistoryPage*>();
    const QList<wallet::UtxoHistory*> histories = window->controller()->histories();
    if (history && !histories.isEmpty()) {
        wallet::HistoryBook* book = histories.first()->book();
        const wallet::Network bitcoin = book->network();
        QVector<QString> scripthashes;
        QVector<wallet::AddressHistory> known;
        for (int i = 0; i < model->size(); ++i) {
            const wallet::AddressRow& row = model->at(i);
            if (row.network == bitcoin) {
                scripthashes.append(row.scripthash);
                known.append({row.scripthash, row.wallet, row.address, QStringLiteral("aa")});
            }
        }
        const int transactions = rows + rows / 5;
        window->showPage(QStringLiteral("history"));
        settle();
        whole.restart();
        slowest = 0;
        for (int first = 0; first < transactions; first += kChunk) {
            QVector<wallet::TxRecord> chunk;
            chunk.reserve(kChunk);
            for (int i = first; i < std::min(first + kChunk, transactions); ++i)
                chunk.append(txAt(i, scripthashes, bitcoin));
            clock.restart();
            book->addSaved(chunk);
            settle();
            slowest = std::max(slowest, clock.elapsed());
        }
        book->addSavedAddresses(known);
        settle();
        say(QStringLiteral("%1 transactions reach the History page, the whole time").arg(transactions), whole.elapsed());
        say(QStringLiteral("the slowest chunk of them"), slowest);
        if (slowest > 2500)
            report += QStringLiteral("FAIL  a chunk of transactions held the window for %1 ms\n").arg(slowest);

        ui::HistoryTableModel* lines = history->tableModel();
        clock.restart();
        lines->rebuildNow();
        say(QStringLiteral("putting the whole list together"), clock.elapsed());
        if (lines->rowCount() != transactions)
            report += QStringLiteral("FAIL  the list has %1 lines, not %2\n").arg(lines->rowCount()).arg(transactions);
        if (QTableView* view = history->findChild<QTableView*>()) {
            clock.restart();
            view->sortByColumn(ui::HistoryTableModel::ColAmount, Qt::DescendingOrder);
            settle();
            say(QStringLiteral("sorting by amount"), clock.elapsed());
            // Each key once, and what is text as numbers: the columns that used to take seconds.
            QHash<quint32, QString> walletNames;
            for (const wallet::AddressHistory& one : std::as_const(known))
                walletNames.insert(one.wallet, QStringLiteral("Wallet number %1").arg(one.wallet));
            lines->setWalletNamesForTest(walletNames);
            for (const auto& [column, what] : {std::pair{int(ui::HistoryTableModel::ColWallet), QStringLiteral("wallet")},
                                               std::pair{int(ui::HistoryTableModel::ColLabel), QStringLiteral("label")},
                                               std::pair{int(ui::HistoryTableModel::ColUsdNow), QStringLiteral("USD now")}}) {
                clock.restart();
                view->sortByColumn(column, Qt::AscendingOrder);
                settle();
                const qint64 took = clock.elapsed();
                say(QStringLiteral("sorting by %1").arg(what), took);
                if (took > 2500)
                    report += QStringLiteral("FAIL  sorting by %1 held the window for %2 ms\n").arg(what).arg(took);
            }
            clock.restart();
            view->sortByColumn(ui::HistoryTableModel::ColDate, Qt::DescendingOrder);
            settle();
            say(QStringLiteral("sorting by date"), clock.elapsed());
        }
        if (auto* search = history->findChild<QLineEdit*>()) {
            const auto timed = [&](const QString& text, const QString& what, int expected) {
                clock.restart();
                search->setText(text);
                settle();
                QElapsedTimer waited;
                waited.start();
                while (waited.elapsed() < 300)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                const qint64 took = clock.elapsed();
                say(what + QStringLiteral(" (250 ms of it waiting for the typing to stop)"), took);
                if (took > 10000)
                    report += QStringLiteral("FAIL  %1 took %2 ms\n").arg(what).arg(took);
                if (expected >= 0 && lines->rowCount() != expected)
                    report += QStringLiteral("FAIL  %1 shows %2 lines, not %3\n").arg(what).arg(lines->rowCount()).arg(expected);
            };
            // What it should find, off the book: the same search done the long way.
            const QString addressWanted = known.at(known.size() / 3).address;
            const QString part = addressWanted.right(10);
            int touching = 0;
            for (int position = 0; position < book->size(); ++position) {
                const QVector<wallet::TxEffect>& effects = book->effectsOf(position);
                touching += std::any_of(effects.cbegin(), effects.cend(), [&](const wallet::TxEffect& effect) { return book->addressAt(effect.address).address.contains(part, Qt::CaseInsensitive); }) ? 1 : 0;
            }
            timed(part, QStringLiteral("searching an address few lines touch"), touching);
            if (touching == 0)
                report += QStringLiteral("FAIL  no line touches the address searched for\n");
            timed(QStringLiteral("no such thing"), QStringLiteral("searching what no line has"), 0);
            timed(QString(), QStringLiteral("clearing the search"), transactions);
        }
        if (auto* type = history->findChild<QComboBox*>(QStringLiteral("historyType"))) {
            clock.restart();
            type->setCurrentIndex(1);
            settle();
            say(QStringLiteral("showing what was received"), clock.elapsed());
            for (int row = 0; row < lines->rowCount(); row += 1000) {
                if (lines->lineAt(row)->kind != ui::TxKind::Received) {
                    report += QStringLiteral("FAIL  showing what was received shows something else\n");
                    break;
                }
            }
            clock.restart();
            type->setCurrentIndex(0);
            settle();
            say(QStringLiteral("showing all again"), clock.elapsed());
        }
        clock.restart();
        const QString wanted = known.at(known.size() / 2).scripthash;
        history->showPlace(0, bitcoin, wanted, known.at(known.size() / 2).address);
        settle();
        say(QStringLiteral("one address's history"), clock.elapsed());
        if (lines->rowCount() != book->addressAt(book->findAddress(wanted)).txs.size())
            report += QStringLiteral("FAIL  one address shows %1 lines, not %2\n").arg(lines->rowCount()).arg(book->addressAt(book->findAddress(wanted)).txs.size());
        clock.restart();
        history->clearPlace();
        settle();
        say(QStringLiteral("back to all of them"), clock.elapsed());
        {
            const QString path = window->controller()->store().dataDir() + QStringLiteral("/scale-history.csv");
            QString error;
            clock.restart();
            if (!history->exportCsvNow(path, &error))
                report += QStringLiteral("FAIL  the export of transactions: %1\n").arg(error);
            say(QStringLiteral("exporting every transaction, all at once as a test does: the window does it a piece at a time"), clock.elapsed());
            QFile::remove(path);
        }
        clock.restart();
        window->grab();
        say(QStringLiteral("drawing the page"), clock.elapsed());
        // What a block does: the state of what is pending, all the way down.
        clock.restart();
        for (int i = 0; i < 2000; ++i)
            book->setHeight(i * 100 % book->size(), 900001);
        settle();
        say(QStringLiteral("2,000 transactions change"), clock.elapsed());
        clock.restart();
        lines->rebuildNow();
        settle();
        say(QStringLiteral("and the list follows"), clock.elapsed());
        // A wallet goes, and everything is renumbered.
        clock.restart();
        book->forgetWallet(known.first().wallet);
        settle();
        const qint64 forgetting = clock.elapsed();
        say(QStringLiteral("a wallet removed from all that"), forgetting);
        if (forgetting > 2500)
            report += QStringLiteral("FAIL  removing a wallet held the window for %1 ms\n").arg(forgetting);
        if (lines->rowCount() != book->lineCount())
            report += QStringLiteral("FAIL  after removing a wallet the list has %1 lines, not %2\n").arg(lines->rowCount()).arg(book->lineCount());
        // What a start has to read of them, from the file.
        {
            const QString path = window->controller()->store().dataDir() + QStringLiteral("/scale-history.db");
            store::LedgerDb db;
            QString why;
            if (db.open(path, QByteArray(32, 'k'), &why) != store::LedgerDb::Open::Ready) {
                report += QStringLiteral("FAIL  a database for the history: %1\n").arg(why);
            } else {
                clock.restart();
                book->addHeader(1, QByteArray(80, 'h'));
                book->takeBack([&] {
                    wallet::HistoryDelta all;
                    for (int position = 0; position < book->size(); ++position)
                        all.txs.append(book->at(position));
                    return all;
                }());
                const wallet::HistoryDelta delta = book->takeDelta();
                if (!db.saveHistory(bitcoin, delta, &why))
                    report += QStringLiteral("FAIL  writing the history: %1\n").arg(why);
                say(QStringLiteral("writing %1 transactions to the database").arg(delta.txs.size()), clock.elapsed());
                db.close();
                db.open(path, QByteArray(32, 'k'), &why);
                clock.restart();
                qint64 firstChunk = -1;
                qint64 read = 0;
                if (!db.loadHistory(bitcoin, kChunk, [&](QVector<wallet::TxRecord>&& chunk) {
                        if (firstChunk < 0)
                            firstChunk = clock.elapsed();
                        read += chunk.size();
                    }, &why))
                    report += QStringLiteral("FAIL  reading the history: %1\n").arg(why);
                const qint64 readAll = clock.elapsed();
                say(QStringLiteral("reading them back: the first chunk"), firstChunk);
                say(QStringLiteral("all of them"), readAll);
                if (read != delta.txs.size())
                    report += QStringLiteral("FAIL  read back %1 transactions, not %2\n").arg(read).arg(delta.txs.size());
                if (firstChunk > 3000)
                    report += QStringLiteral("FAIL  the first chunk of a history took %1 ms: nothing shows until it comes\n").arg(firstChunk);
                db.close();
            }
            QFile::remove(path);
            QFile::remove(path + QStringLiteral("-wal"));
            QFile::remove(path + QStringLiteral("-shm"));
        }
        clock.restart();
        book->clear();
        settle();
        say(QStringLiteral("clearing every transaction"), clock.elapsed());
    }

    clock.restart();
    model->clear();
    settle();
    say(QStringLiteral("clearing every row"), clock.elapsed());
    return report;
}

} // namespace app
