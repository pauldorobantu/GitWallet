#include "app/ScaleTest.h"

#include "app/WalletController.h"
#include "net/PriceFeed.h"
#include "ui/AddressTableModel.h"
#include "ui/MainWindow.h"
#include "ui/pages/AddressesPage.h"
#include "wallet/Registry.h"
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

namespace app {

namespace {

// Everything waiting on the event loop.
void settle()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
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

    for (const QString& name : {QStringLiteral("overview"), QStringLiteral("wallets"), QStringLiteral("addresses")}) {
        clock.restart();
        window->showPage(name);
        settle();
        window->grab();
        say(QStringLiteral("opening %1").arg(name), clock.elapsed());
    }
    clock.restart();
    model->clear();
    settle();
    say(QStringLiteral("clearing every row"), clock.elapsed());
    return report;
}

} // namespace app
