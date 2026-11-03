#include "app/WalletController.h"
#include "core/SettingsRepo.h"
#include "net/PriceFeed.h"
#include "ui/AddressTableModel.h"
#include "ui/Figures.h"
#include "ui/Holdings.h"
#include "ui/HistoryTableModel.h"
#include "ui/MainWindow.h"
#include "ui/PlaceBar.h"
#include "ui/TitleBar.h"
#include "ui/dialogs/PaymentsDialog.h"
#include "ui/pages/LockPage.h"
#include "wallet/Seal.h"
#include "ui/pages/AddressesPage.h"
#include "ui/pages/HistoryPage.h"
#include "ui/pages/HoldingsPage.h"
#include "ui/pages/PortfolioPage.h"
#include "wallet/History.h"
#include "wallet/Payments.h"
#include "wallet/UtxoHistory.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTableView>
#include <QDir>

#include <functional>
#include <memory>

// Dust and what came in, in the real window, with no server: what a test writes into the model and
// the history is what a scan would have written.
namespace app {

namespace {

using Check = std::function<void(bool, const QString&)>;

bool waitFor(const std::function<bool()>& done, int ms)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

void pause(int ms)
{
    waitFor([] { return false; }, ms);
}

void offline(WalletController* controller)
{
    controller->prices()->useTestServer(QStringLiteral("http://127.0.0.1:1"));
    wallet::ScanSettings settings = controller->store().settings;
    settings.utxoReceiveGap = 20;
    settings.utxoChangeGap = 20;
    settings.evmGap = 20;
    for (const wallet::NetworkInfo& network : wallet::networks())
        settings.servers.insert(network.id, {network.family == wallet::Family::Utxo ? QStringLiteral("127.0.0.1:1") : QStringLiteral("https://127.0.0.1:1")});
    QString error;
    controller->applySettings(settings, &error);
}

void setPrices(WalletController* controller)
{
    QList<net::PriceQuote> quotes;
    for (const wallet::AssetInfo& info : wallet::assets()) {
        net::PriceQuote quote;
        quote.asset = info.asset;
        quote.usd = info.dollar ? 1.0 : (info.symbol == QLatin1String("BTC") ? 100000.0 : 2000.0);
        quote.at = QDateTime::currentDateTimeUtc();
        quote.source = QStringLiteral("Test");
        quotes.append(quote);
    }
    controller->prices()->restore(quotes);
}

void answerAll(wallet::WalletModel* model, quint32 wallet)
{
    for (int i = 0; i < model->size(); ++i) {
        if (model->at(i).wallet != wallet || model->at(i).checked)
            continue;
        wallet::AddressRow row = model->at(i);
        row.checked = true;
        model->upsert(row);
    }
}

void putU32(chain::Bytes* out, uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out->push_back(uint8_t(value >> shift));
}

void putU64(chain::Bytes* out, uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
        out->push_back(uint8_t(value >> shift));
}

chain::Transaction transaction(const QVector<chain::OutPoint>& inputs, const QVector<QPair<chain::Bytes, qint64>>& outputs, uint32_t nonce)
{
    chain::Bytes raw;
    putU32(&raw, 2);
    raw.push_back(uint8_t(inputs.size()));
    for (const chain::OutPoint& input : inputs) {
        raw.insert(raw.end(), input.txid.rbegin(), input.txid.rend());
        putU32(&raw, input.n);
        raw.push_back(0);
        putU32(&raw, 0xffffffff);
    }
    raw.push_back(uint8_t(outputs.size()));
    for (const auto& output : outputs) {
        putU64(&raw, uint64_t(output.second));
        raw.push_back(uint8_t(output.first.size()));
        raw.insert(raw.end(), output.first.begin(), output.first.end());
    }
    putU32(&raw, nonce);
    chain::Transaction tx;
    chain::parseTransaction(raw, &tx);
    return tx;
}

chain::OutPoint somewhere(uint32_t n)
{
    chain::OutPoint point;
    point.txid.fill(uint8_t(0x40 + (n & 0x3f)));
    point.n = n;
    return point;
}

// What is known of a wallet, ready to be written into.
struct Scene {
    std::unique_ptr<ui::MainWindow> window;
    WalletController* controller = nullptr;
    wallet::WalletModel* model = nullptr;
    wallet::HistoryBook* book = nullptr;
    wallet::AccountKey btcKey;
    wallet::AccountKey ethKey;
    wallet::Network bitcoin{};
    wallet::Network ethereum{};
    quint32 shop = 0;

    int rowOf(quint32 index) const { return model->find(shop, bitcoin, btcKey.type, 0, 0, index); }
    int ethRowOf(quint32 index) const { return model->find(shop, ethereum, ethKey.type, 0, 0, index); }
    chain::Bytes scriptOf(quint32 index) const
    {
        return chain::addressFromChainNode(btcKey.node().deriveChild(0), btcKey.type, btcKey.account, 0, index, wallet::networkInfo(bitcoin).format).scriptPubKey;
    }
    // A server says the address has moved on to this status, with these coins.
    void paid(quint32 index, const QString& status, qint64 confirmed, qint64 unconfirmed = 0) const
    {
        wallet::AddressRow row = model->at(rowOf(index));
        row.status = status;
        row.used = true;
        row.checked = true;
        row.confirmed = confirmed;
        row.unconfirmed = unconfirmed;
        model->upsert(row);
    }
    int add(const chain::Transaction& tx, int height = 0) const
    {
        return book->add(tx, height, QDateTime::currentSecsSinceEpoch(), [this](const QString& scripthash) {
            const int row = model->findByScripthash(bitcoin, scripthash);
            if (row < 0)
                return false;
            book->describeAddress(scripthash, model->at(row).wallet, model->at(row).address);
            return true;
        });
    }
    int payments() const { return int(controller->payments()->all().size()); }
};

bool makeScene(Scene* scene, const QList<wallet::AccountKey>& keys, const Check& check)
{
    QString error;
    scene->window = std::make_unique<ui::MainWindow>();
    scene->window->show();
    scene->window->resize(1280, 800);
    pause(30); // what is saved opens on the first turn
    scene->controller = scene->window->controller();
    offline(scene->controller);
    setPrices(scene->controller);
    scene->model = scene->controller->model();
    scene->bitcoin = wallet::findNetwork(QStringLiteral("bitcoin"))->network;
    scene->ethereum = wallet::findNetwork(QStringLiteral("ethereum"))->network;
    QList<wallet::AccountKey> added;
    for (const wallet::AccountKey& key : keys) {
        if (key.type == chain::ScriptType::P2WPKH) {
            scene->btcKey = key;
            added.append(key);
        } else if (key.type == chain::ScriptType::EthAccount) {
            scene->ethKey = key;
            added.append(key);
        }
    }
    if (!scene->controller->store().wallets.isEmpty()) {
        scene->shop = scene->controller->store().wallets.first().id;
    } else if (!scene->controller->addKeysWallet(added, QStringLiteral("Shop"), &error)) {
        check(false, QStringLiteral("a wallet for the test: %1").arg(error));
        return false;
    } else {
        scene->shop = scene->controller->store().wallets.first().id;
    }
    scene->book = scene->controller->historyOf(scene->bitcoin)->book();
    const bool rows = waitFor([&] { return !scene->controller->loadingSaved() && scene->rowOf(9) >= 0 && scene->ethRowOf(9) >= 0; }, 20000);
    check(rows, QStringLiteral("a wallet with its rows"));
    if (!rows)
        return false;
    answerAll(scene->model, scene->shop);
    return true;
}

void testDust(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    Scene scene;
    if (!makeScene(&scene, keys, check))
        return;
    WalletController* controller = scene.controller;
    wallet::WalletModel* model = scene.model;
    const wallet::AssetInfo& btc = wallet::assetInfo(wallet::networkInfo(scene.bitcoin).coin);
    const wallet::NetworkInfo& ethereum = wallet::networkInfo(scene.ethereum);
    check(btc.dust.toDecimalString() == "1000", QStringLiteral("the list of coins says what dust is for Bitcoin"));
    check(ui::figures::dustLimits().contains(QStringLiteral("0.00001 BTC")) && ui::figures::dustLimits().contains(QStringLiteral("0.01 USDT")),
          QStringLiteral("and it reads as such: %1").arg(ui::figures::dustLimits()));

    // Below the limit, at it, a mix, and what says it is meant.
    wallet::AddressRow plain;
    plain.confirmed = 999;
    check([&] { plain.type = chain::ScriptType::P2WPKH; plain.network = scene.bitcoin; return plain.onlyDust(); }(), QStringLiteral("less than the limit is dust"));
    plain.confirmed = 1000;
    check(!plain.onlyDust(), QStringLiteral("the limit itself is not"));
    plain.confirmed = 0;
    check(!plain.onlyDust(), QStringLiteral("nor is nothing"));
    plain.confirmed = 500;
    plain.unconfirmed = -500;
    check(!plain.onlyDust(), QStringLiteral("nor what is on its way out"));

    scene.paid(1, QStringLiteral("a"), 500);
    scene.paid(2, QStringLiteral("b"), 100000);
    check(controller->markGivenOut(model->at(scene.rowOf(3)), &error), QStringLiteral("one address given out: %1").arg(error));
    scene.paid(3, QStringLiteral("c"), 500);
    check(controller->setAddressLabel(model->at(scene.rowOf(4)).address, QStringLiteral("A till"), &error), QStringLiteral("one with a label: %1").arg(error));
    scene.paid(4, QStringLiteral("d"), 500);
    scene.paid(5, QStringLiteral("e"), 300, 300);
    const wallet::Asset usdt = ethereum.tokens.first();
    {
        wallet::AddressRow row = model->at(scene.ethRowOf(1));
        row.setBalance(ethereum.coin, chain::Amount::fromU64(1000));
        row.setBalance(usdt, chain::Amount::fromU64(9999));
        row.checked = true;
        row.used = true;
        model->upsert(row);
        row = model->at(scene.ethRowOf(2));
        row.setBalance(ethereum.coin, chain::Amount::fromU64(1000));
        row.setBalance(usdt, chain::Amount::fromU64(5000000));
        row.checked = true;
        row.used = true;
        model->upsert(row);
    }

    // The list.
    ui::figures::setHideDust(true);
    scene.window->showPage(QStringLiteral("addresses"));
    auto* addresses = scene.window->findChild<ui::AddressesPage*>();
    auto* table = addresses->findChild<QTableView*>();
    auto* hide = addresses->findChild<QCheckBox*>(QStringLiteral("addressesHideDust"));
    pause(200);
    const auto listed = [&](const QString& address) {
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            if (table->model()->index(row, ui::AddressTableModel::ColAddress).data().toString() == address)
                return true;
        }
        return false;
    };
    const auto countLine = [&] {
        for (const QLabel* label : addresses->findChildren<QLabel*>()) {
            if (label->text().contains(QLatin1String("funded")))
                return label->text();
        }
        return QString();
    };
    check(hide && hide->isChecked(), QStringLiteral("dust goes unless told otherwise"));
    check(!listed(model->at(scene.rowOf(1)).address), QStringLiteral("an address with only dust leaves the list"));
    check(!listed(model->at(scene.rowOf(5)).address), QStringLiteral("what is in no block counts the same"));
    check(!listed(model->at(scene.ethRowOf(1)).address) || model->at(scene.ethRowOf(1)).address == model->at(scene.ethRowOf(2)).address, QStringLiteral("so does one with dust of two assets"));
    check(listed(model->at(scene.rowOf(2)).address), QStringLiteral("one with more stays"));
    check(listed(model->at(scene.rowOf(3)).address), QStringLiteral("so does what was given out: someone was meant to pay it"));
    check(listed(model->at(scene.rowOf(4)).address), QStringLiteral("and what has a label"));
    check(listed(model->at(scene.ethRowOf(2)).address), QStringLiteral("and dust of one asset beside more of another"));
    check(listed(model->at(scene.rowOf(0)).address), QStringLiteral("and an address with nothing: it is no dust"));
    const int shownWithout = table->model()->rowCount();
    check(shownWithout == model->size() - 3, QStringLiteral("three rows left the list (%1 of %2)").arg(shownWithout).arg(model->size()));
    check(countLine().contains(QStringLiteral("funded · 3 dust hidden")), QStringLiteral("and it says so: %1").arg(countLine()));
    check(countLine().contains(QStringLiteral("· 4 funded")), QStringLiteral("what it calls funded is what it shows: %1").arg(countLine()));
    auto* search = addresses->findChild<QLineEdit*>();
    const QString dustAddress = model->at(scene.rowOf(1)).address;
    search->setText(dustAddress);
    pause(50);
    check(listed(dustAddress), QStringLiteral("a search finds it all the same"));
    search->clear();
    pause(50);
    // It moves with what the row holds.
    scene.paid(1, QStringLiteral("a2"), 2000);
    pause(150);
    check(listed(dustAddress) && countLine().contains(QStringLiteral("2 dust hidden")), QStringLiteral("more coming in brings it back: %1").arg(countLine()));
    scene.paid(1, QStringLiteral("a3"), 500);
    pause(150);
    check(!listed(dustAddress), QStringLiteral("less takes it away"));
    hide->setChecked(false);
    pause(50);
    check(table->model()->rowCount() == model->size() && listed(dustAddress), QStringLiteral("unticked, all of them show"));
    check(!countLine().contains(QLatin1String("dust")), QStringLiteral("and nothing is said to be hidden: %1").arg(countLine()));
    check(!ui::figures::hideDust(), QStringLiteral("which stays as chosen"));
    hide->setChecked(true);
    pause(50);
    // A label given later, and a given-out mark given later, bring it back.
    check(controller->setAddressLabel(dustAddress, QStringLiteral("Mine after all"), &error), QStringLiteral("a label on it: %1").arg(error));
    pause(150);
    check(listed(dustAddress), QStringLiteral("a label says it is meant"));
    check(controller->setAddressLabel(dustAddress, QString(), &error), QStringLiteral("and off again: %1").arg(error));
    pause(150);

    // Totals do not move for it.
    const chain::Amount inAll = ui::holdingOf(controller, btc.asset).amount;
    check(inAll.toDecimalString() == std::to_string(500 + 100000 + 500 + 500 + 600), QStringLiteral("dust still counts in the total: %1").arg(QString::fromStdString(inAll.toDecimalString())));

    // The coins. USDC is all dust here, on an address of its own.
    scene.window->showPage(QStringLiteral("portfolio"));
    auto* portfolio = scene.window->findChild<ui::PortfolioPage*>();
    QTableView* coins = portfolio->table()->view();
    const wallet::Asset usdc = ethereum.tokens.at(1);
    {
        wallet::AddressRow row = model->at(scene.ethRowOf(3));
        row.setBalance(usdc, chain::Amount::fromU64(5000));
        row.checked = true;
        row.used = true;
        model->upsert(row);
    }
    const auto coinRow = [&](const QString& name) {
        for (int row = 0; row < coins->model()->rowCount(); ++row) {
            if (coins->model()->index(row, 0).data().toString() == name)
                return row;
        }
        return -1;
    };
    const auto settled = [&] {
        answerAll(model, scene.shop);
        pause(50);
        return ui::holdingOf(controller, usdc).complete;
    };
    check(waitFor(settled, 8000), QStringLiteral("every row has answered"));
    auto* hideCoins = portfolio->findChild<QCheckBox*>(QStringLiteral("hideDust"));
    auto* hideZero = portfolio->findChild<QCheckBox*>(QStringLiteral("hideZero"));
    hideZero->setChecked(true);
    hideCoins->setChecked(true);
    pause(450);
    const QString heroWith = portfolio->findChild<QLabel*>(QStringLiteral("heroValue"))->text();
    check(coinRow(QStringLiteral("USD Coin")) < 0, QStringLiteral("a coin of only dust leaves the list"));
    check(coinRow(QStringLiteral("Bitcoin")) >= 0, QStringLiteral("one with more does not"));
    hideCoins->setChecked(false);
    pause(450);
    check(coinRow(QStringLiteral("USD Coin")) >= 0, QStringLiteral("unticked it is back"));
    check(portfolio->findChild<QLabel*>(QStringLiteral("heroValue"))->text() == heroWith, QStringLiteral("and the total never moved: %1").arg(heroWith));
    hideCoins->setChecked(true);
    pause(100);

    // One address was asked for by name.
    ui::Place place;
    place.root = ui::Place::Root::Addresses;
    place.address = model->at(scene.ethRowOf(2)); // dust of its coin, beside more of a token
    scene.window->goTo(place, ui::Place::Tab::Holdings);
    pause(450);
    ui::HoldingsPage* holdingsPage = scene.window->findChild<ui::HoldingsPage*>();
    QTableView* assets = holdingsPage->table()->view();
    bool dustShown = false;
    for (int row = 0; row < assets->model()->rowCount(); ++row)
        dustShown = dustShown || assets->model()->index(row, 0).data().toString() == QLatin1String("Ethereum");
    check(dustShown, QStringLiteral("the page of one address shows its dust"));
    scene.window->showPage(QStringLiteral("history"));

    // The history.
    auto* history = scene.window->findChild<ui::HistoryPage*>();
    ui::HistoryTableModel* lines = history->tableModel();
    auto* hideLines = history->findChild<QCheckBox*>(QStringLiteral("historyHideDust"));
    hideLines->setChecked(true);
    const int dustLine = scene.add(transaction({somewhere(1)}, {{scene.scriptOf(6), 500}}, 1));
    const int bigLine = scene.add(transaction({somewhere(2)}, {{scene.scriptOf(6), 500000}}, 2));
    const int givenLine = scene.add(transaction({somewhere(3)}, {{scene.scriptOf(3), 500}}, 3));
    const int labelLine = scene.add(transaction({somewhere(4)}, {{scene.scriptOf(4), 500}}, 4));
    const int notedLine = scene.add(transaction({somewhere(5)}, {{scene.scriptOf(7), 500}}, 5));
    wallet::TxNote note;
    note.note = QStringLiteral("A customer's test payment");
    scene.book->setNote(scene.book->at(notedLine).txid, note);
    const auto shownLine = [&](int position) {
        for (int row = 0; row < lines->rowCount(); ++row) {
            if (lines->lineAt(row)->position == position)
                return true;
        }
        return false;
    };
    lines->rebuildNow();
    check(!shownLine(dustLine), QStringLiteral("next to nothing coming in leaves the history"));
    check(shownLine(bigLine), QStringLiteral("what is more stays"));
    check(shownLine(givenLine), QStringLiteral("so does what came to an address given out"));
    check(shownLine(labelLine), QStringLiteral("to one with a label"));
    check(shownLine(notedLine), QStringLiteral("or has a note"));
    check(lines->dustHidden() == 1, QStringLiteral("and it counts what it left out: %1").arg(lines->dustHidden()));
    auto* lineSearch = history->findChild<QLineEdit*>();
    lineSearch->setText(QString::fromStdString(chain::hashHex(scene.book->at(dustLine).txid)));
    pause(350);
    check(shownLine(dustLine), QStringLiteral("a search finds it"));
    lineSearch->clear();
    pause(350);
    const wallet::AddressRow six = model->at(scene.rowOf(6));
    history->showPlace(six.wallet, six.network, six.scripthash, six.address);
    check(shownLine(dustLine) && !hideLines->isVisible(), QStringLiteral("one address's history has it, with nothing to untick"));
    history->clearPlace();
    pause(50);
    history->showAll();
    hideLines->setChecked(false);
    check(shownLine(dustLine) && lines->dustHidden() == 0, QStringLiteral("unticked it is there"));
    hideLines->setChecked(true);

    check(controller->removeAllWallets(&error), QStringLiteral("it goes: %1").arg(error));
}

void testPayments(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    wallet::PaymentBook::setPauseForTest(10);
    SettingsRepo().set(QStringLiteral("ui.paymentAlerts"), QStringLiteral("1"));
    ui::figures::setHideDust(true);
    QList<wallet::Payment> told;
    {
        Scene scene;
        if (!makeScene(&scene, keys, check))
            return;
        WalletController* controller = scene.controller;
        wallet::WalletModel* model = scene.model;
        wallet::PaymentBook* payments = controller->payments();
        QObject::connect(payments, &wallet::PaymentBook::told, payments, [&told](const QList<wallet::Payment>& some) { told.append(some); });
        ui::TitleBar* title = scene.window->titleBar();
        check(scene.payments() == 0 && title->unreadPayments() == 0, QStringLiteral("nothing has come in"));

        // A payment.
        scene.paid(1, QStringLiteral("s1"), 0, 50000);
        const int first = scene.add(transaction({somewhere(1)}, {{scene.scriptOf(1), 50000}}, 1));
        check(scene.payments() == 1, QStringLiteral("a transaction on an address whose status moved is a payment"));
        const wallet::Payment one = payments->all().value(0);
        check(one.amount.toDecimalString() == "50000" && one.asset == QLatin1String("bitcoin/btc") && one.hasTx && one.txid == scene.book->at(first).txid, QStringLiteral("of that much, in that coin, by that transaction"));
        check(one.wallet == scene.shop && one.address == model->at(scene.rowOf(1)).address, QStringLiteral("to that wallet and address"));
        check(qAbs(one.usd - 50.0) < 0.001, QStringLiteral("worth what it was worth then: %1").arg(one.usd));
        check(!one.whileClosed && !one.read, QStringLiteral("not looked at yet"));
        check(title->unreadPayments() == 1, QStringLiteral("the bell says so"));
        check(waitFor([&] { return !told.isEmpty(); }, 3000), QStringLiteral("and it is told"));
        check(scene.window->lastPaymentAlert().contains(QStringLiteral("not in a block yet")) && !scene.window->lastPaymentAlert().contains(QStringLiteral("Received")) && scene.window->lastPaymentAlert().contains(QStringLiteral("$50.00")) && scene.window->lastPaymentAlert().contains(QStringLiteral("Shop")),
              QStringLiteral("as what it is, not as received: %1").arg(scene.window->lastPaymentAlert()));
        scene.book->setStatus(model->at(scene.rowOf(1)).scripthash, QStringLiteral("s1"));

        // It is told once.
        scene.add(transaction({somewhere(1)}, {{scene.scriptOf(1), 50000}}, 1));
        check(scene.payments() == 1, QStringLiteral("once"));

        // What is not one.
        scene.paid(2, QStringLiteral("o1"), 0, 3000);
        const chain::Transaction own = transaction({somewhere(2)}, {{scene.scriptOf(2), 3000}}, 2);
        scene.add(own);
        check(scene.payments() == 2, QStringLiteral("(a payment to spend)"));
        const chain::OutPoint ownCoin{own.txid, 0};
        scene.paid(3, QStringLiteral("o2"), 0, 2500);
        scene.add(transaction({ownCoin}, {{scene.scriptOf(3), 2500}}, 3));
        check(scene.payments() == 2, QStringLiteral("a transaction that spends the wallets' own coins is none, whatever comes back"));

        // The first look at an address: what it had is no news, however new the look.
        const int unanswered = model->size();
        (void)unanswered;
        wallet::AddressRow fresh = model->at(scene.rowOf(4));
        fresh.checked = false;
        model->upsert(fresh);
        scene.paid(4, QStringLiteral("f1"), 4000);
        scene.add(transaction({somewhere(4)}, {{scene.scriptOf(4), 4000}}, 4));
        check(scene.payments() == 2, QStringLiteral("an address with no answer before has told of nothing new"));

        // An address with a history not read yet: its next move brings all of that with it.
        scene.paid(5, QStringLiteral("h1"), 4000);
        check(scene.payments() == 2, QStringLiteral("(a status alone is no payment)"));
        wallet::AddressRow behind = model->at(scene.rowOf(6));
        behind.status = QStringLiteral("old");
        behind.used = true;
        behind.checked = true;
        behind.confirmed = 1000000;
        model->upsert(behind); // from nothing to this: that one was a real move
        scene.add(transaction({somewhere(6)}, {{scene.scriptOf(6), 1000000}}, 6));
        check(scene.payments() == 3, QStringLiteral("(a payment seen for the first time on an empty address)"));
        wallet::AddressRow notYet = model->at(scene.rowOf(7));
        notYet.checked = false;
        model->upsert(notYet);
        wallet::AddressRow caughtUp = notYet;
        caughtUp.status = QStringLiteral("long ago");
        caughtUp.used = true;
        caughtUp.checked = true;
        caughtUp.confirmed = 7000;
        model->upsert(caughtUp); // an old status, with nothing here up to it
        const int before = scene.payments();
        scene.paid(7, QStringLiteral("later"), 9000);
        scene.add(transaction({somewhere(7)}, {{scene.scriptOf(7), 7000}}, 7));
        scene.add(transaction({somewhere(8)}, {{scene.scriptOf(7), 2000}}, 8));
        check(scene.payments() == before, QStringLiteral("a history being caught up on says nothing, new or old"));

        // A higher fee on the same payment.
        scene.paid(8, QStringLiteral("r1"), 0, 60000);
        const int replaced = scene.add(transaction({somewhere(9)}, {{scene.scriptOf(8), 60000}}, 9));
        const int kept = scene.payments();
        const chain::Hash256 replacedId = scene.book->at(replaced).txid;
        scene.paid(8, QStringLiteral("r2"), 0, 60000);
        const int replacing = scene.add(transaction({somewhere(9)}, {{scene.scriptOf(8), 60000}}, 10));
        check(scene.payments() == kept, QStringLiteral("the same payment with a higher fee is no second one"));
        check(payments->all().constLast().txid == scene.book->at(replacing).txid && payments->all().constLast().txid != replacedId, QStringLiteral("and goes by the transaction that took its place"));
        scene.paid(8, QStringLiteral("r3"), 0, 70000);
        scene.add(transaction({somewhere(9)}, {{scene.scriptOf(8), 70000}}, 11));
        check(scene.payments() == kept + 1, QStringLiteral("one that pays another amount is"));

        // Dust.
        told.clear();
        int count = scene.payments();
        scene.paid(9, QStringLiteral("d1"), 500);
        scene.add(transaction({somewhere(10)}, {{scene.scriptOf(9), 500}}, 12));
        check(scene.payments() == count, QStringLiteral("dust goes untold"));
        wallet::AddressRow given = model->at(scene.rowOf(10));
        check(controller->markGivenOut(given, &error), QStringLiteral("(given out: %1)").arg(error));
        scene.paid(10, QStringLiteral("d2"), 500);
        scene.add(transaction({somewhere(11)}, {{scene.scriptOf(10), 500}}, 13));
        check(scene.payments() == ++count, QStringLiteral("unless someone was given that address"));
        check(controller->setAddressLabel(model->at(scene.rowOf(11)).address, QStringLiteral("Mine"), &error), QStringLiteral("(a label: %1)").arg(error));
        scene.paid(11, QStringLiteral("d3"), 500);
        scene.add(transaction({somewhere(12)}, {{scene.scriptOf(11), 500}}, 14));
        check(scene.payments() == ++count, QStringLiteral("or it has a label"));
        ui::figures::setHideDust(false);
        scene.paid(12, QStringLiteral("d4"), 500);
        scene.add(transaction({somewhere(13)}, {{scene.scriptOf(12), 500}}, 15));
        check(scene.payments() == ++count, QStringLiteral("or the lists show dust"));
        ui::figures::setHideDust(true);

        // Paid on two addresses at once: one payment.
        scene.paid(13, QStringLiteral("t1"), 10000);
        scene.paid(14, QStringLiteral("t2"), 20000);
        scene.add(transaction({somewhere(14)}, {{scene.scriptOf(14), 20000}, {scene.scriptOf(13), 10000}}, 16));
        check(scene.payments() == ++count && payments->all().constLast().addresses == 2 && payments->all().constLast().amount.toDecimalString() == "30000", QStringLiteral("a transaction paying two addresses is one payment of all of it"));

        // Ethereum goes by the balance.
        const wallet::NetworkInfo& ethereum = wallet::networkInfo(scene.ethereum);
        const wallet::Asset usdt = ethereum.tokens.first();
        const auto setToken = [&](quint32 index, quint64 units, bool checked = true) {
            wallet::AddressRow row = model->at(scene.ethRowOf(index));
            row.setBalance(usdt, chain::Amount::fromU64(units));
            row.checked = checked;
            row.used = true;
            model->upsert(row);
        };
        setToken(1, 25000000);
        check(scene.payments() == ++count && payments->all().constLast().asset == QLatin1String("ethereum/usdt") && !payments->all().constLast().hasTx, QStringLiteral("more of a token on an address that had answered is a payment"));
        check(payments->all().constLast().amount.toDecimalString() == "25000000" && qAbs(payments->all().constLast().usd - 25.0) < 0.001, QStringLiteral("of the difference"));
        setToken(1, 30000000);
        check(scene.payments() == ++count && payments->all().constLast().amount.toDecimalString() == "5000000", QStringLiteral("the next one of what it gained"));
        setToken(1, 12000000);
        check(scene.payments() == count, QStringLiteral("less is none"));
        setToken(1, 30000000);
        check(scene.payments() == count, QStringLiteral("nor is a source behind the others coming back to what was there"));
        setToken(1, 12000000);
        setToken(1, 32000000);
        check(scene.payments() == ++count, QStringLiteral("anything else coming back is"));
        wallet::AddressRow noAnswer = model->at(scene.ethRowOf(2));
        noAnswer.checked = false;
        model->upsert(noAnswer);
        setToken(2, 90000000);
        check(scene.payments() == count, QStringLiteral("a first answer tells of nothing new"));
        setToken(3, 5000);
        check(scene.payments() == count, QStringLiteral("nor does dust of a token"));

        // One already in a block when it is first read is received.
        pause(50);
        scene.paid(18, QStringLiteral("b1"), 80000);
        scene.add(transaction({somewhere(31)}, {{scene.scriptOf(18), 80000}}, 31), 900100);
        check(waitFor([&] { return scene.window->lastPaymentAlert().contains(QStringLiteral("Received")); }, 3000), QStringLiteral("what is in a block when first read is told as received: %1").arg(scene.window->lastPaymentAlert()));
        count = scene.payments();

        // What becomes of one told of while in no block.
        pause(50);
        scene.paid(19, QStringLiteral("w1"), 0, 90000);
        const int waitingAt = scene.add(transaction({somewhere(32)}, {{scene.scriptOf(19), 90000}}, 32));
        pause(50);
        const QString saidThen = scene.window->lastPaymentAlert();
        const int unreadThen = payments->unread();
        scene.book->setHeight(waitingAt, 900200);
        pause(50);
        check(scene.window->lastPaymentAlert() == saidThen, QStringLiteral("a server saying it is in a block is no proof: nothing is said yet"));
        scene.book->setProven(waitingAt, true);
        check(waitFor([&] { return scene.window->lastPaymentAlert().startsWith(QStringLiteral("In a block")); }, 3000), QStringLiteral("proven in one, it is told again: %1").arg(scene.window->lastPaymentAlert()));
        check(scene.window->lastPaymentAlert().contains(QStringLiteral("$90.00")) && scene.window->lastPaymentAlert().contains(QStringLiteral("Shop")), QStringLiteral("with what it is"));
        check(payments->unread() == unreadThen && scene.payments() == count + 1, QStringLiteral("as no new payment"));
        count = scene.payments();
        const QString saidOnce = scene.window->lastPaymentAlert();
        scene.book->setStatus(model->at(scene.rowOf(19)).scripthash, QStringLiteral("w1"));
        pause(50);
        check(scene.window->lastPaymentAlert() == saidOnce, QStringLiteral("and once"));

        // And one that will not come.
        wallet::AddressRow goneRow = model->at(scene.rowOf(0));
        goneRow.status = QStringLiteral("g1");
        goneRow.used = true;
        goneRow.checked = true;
        goneRow.unconfirmed = 70000;
        model->upsert(goneRow);
        const int droppedAt = scene.add(transaction({somewhere(33)}, {{scene.scriptOf(0), 70000}}, 33));
        check(scene.payments() == ++count, QStringLiteral("(one that will be dropped)"));
        pause(50);
        scene.book->markGone(droppedAt);
        check(waitFor([&] { return scene.window->lastPaymentAlert().startsWith(QStringLiteral("Replaced or dropped")); }, 3000), QStringLiteral("dropped, it is told as not paid: %1").arg(scene.window->lastPaymentAlert()));

        // Two at once are one thing to say too.
        scene.paid(13, QStringLiteral("two1"), 0, 11000);
        const int twoA = scene.add(transaction({somewhere(34)}, {{scene.scriptOf(13), 11000}}, 34));
        scene.paid(14, QStringLiteral("two2"), 0, 12000);
        const int twoB = scene.add(transaction({somewhere(35)}, {{scene.scriptOf(14), 12000}}, 35));
        count += 2;
        pause(50);
        scene.book->setHeight(twoA, 900300);
        scene.book->setProven(twoA, true);
        scene.book->setHeight(twoB, 900300);
        scene.book->setProven(twoB, true);
        check(waitFor([&] { return scene.window->lastPaymentAlert().contains(QStringLiteral("2 payments got into a block")); }, 3000), QStringLiteral("a block with two is one thing to say: %1").arg(scene.window->lastPaymentAlert()));

        // Many at once are one thing to say.
        pause(50);
        told.clear();
        setToken(4, 10000000);
        setToken(5, 10000000);
        setToken(6, 10000000);
        check(waitFor([&] { return told.size() == 3; }, 3000), QStringLiteral("a burst is told"));
        check(scene.window->lastPaymentAlert().contains(QStringLiteral("3 payments came in")) && scene.window->lastPaymentAlert().contains(QStringLiteral("$30.00")), QStringLiteral("as one: %1").arg(scene.window->lastPaymentAlert()));

        // What was dropped goes from the history in time, and a server can list it again.
        pause(50);
        const int toldBefore = scene.payments();
        scene.paid(17, QStringLiteral("x1"), 0, 40000);
        const chain::Transaction listedAgain = transaction({somewhere(30)}, {{scene.scriptOf(17), 40000}}, 30);
        const int listedAt = scene.add(listedAgain);
        check(scene.payments() == toldBefore + 1, QStringLiteral("(one told of)"));
        scene.book->markGone(listedAt);
        scene.book->forgetGoneBefore(QDateTime::currentSecsSinceEpoch() + 10);
        check(scene.book->find(listedAgain.txid) < 0, QStringLiteral("(gone from the history)"));
        scene.book->setStatus(model->at(scene.rowOf(17)).scripthash, QStringLiteral("x1"));
        scene.paid(17, QStringLiteral("x2"), 0, 40000);
        const int listedBack = scene.add(listedAgain);
        check(scene.book->find(listedAgain.txid) >= 0 && scene.payments() == toldBefore + 1, QStringLiteral("what was told of and comes back is not told again"));
        scene.book->markGone(listedBack);
        scene.book->forgetGoneBefore(QDateTime::currentSecsSinceEpoch() + 10);
        pause(50);

        // At the start, with the saved figures still being checked.
        controller->setShowingSavedForTest(true);
        setToken(7, 10000000);
        controller->setShowingSavedForTest(false);
        check(payments->all().constLast().whileClosed, QStringLiteral("what comes up while saved figures are being checked came while it was closed"));
        pause(50);
        check(scene.window->lastPaymentAlert().contains(QStringLiteral("while GitWallet was closed")), QStringLiteral("and is told as such: %1").arg(scene.window->lastPaymentAlert()));
        // A transaction is read a moment after its address moved, by which time the start may be over.
        controller->setShowingSavedForTest(true);
        scene.paid(15, QStringLiteral("c1"), 30000);
        controller->setShowingSavedForTest(false);
        scene.add(transaction({somewhere(20)}, {{scene.scriptOf(15), 30000}}, 20));
        check(payments->all().constLast().whileClosed, QStringLiteral("what moved at the start came while it was closed, however late its transaction is read"));
        pause(50);
        scene.paid(16, QStringLiteral("c2"), 30000);
        scene.add(transaction({somewhere(21)}, {{scene.scriptOf(16), 30000}}, 21));
        check(!payments->all().constLast().whileClosed, QStringLiteral("what moved after it did not"));
        pause(50);

        // Locked, nothing is said of what becomes of one.
        wallet::seal::setCostForTest(256, 1);
        const QString testPassword = QStringLiteral("a test sentence here");
        bool passwordSet = false;
        controller->changePassword(QString(), testPassword, false, scene.window.get(), [&passwordSet](const QString&) { passwordSet = true; });
        check(waitFor([&] { return passwordSet && controller->hasPassword(); }, 15000), QStringLiteral("(a password)"));
        scene.paid(12, QStringLiteral("k1"), 0, 15000);
        const int lockedAt = scene.add(transaction({somewhere(36)}, {{scene.scriptOf(12), 15000}}, 36));
        pause(50);
        scene.window->lockNow();
        const QString saidLocked = scene.window->lastPaymentAlert();
        scene.book->setHeight(lockedAt, 900400);
        scene.book->setProven(lockedAt, true);
        pause(100);
        check(scene.window->isLocked() && scene.window->lastPaymentAlert() == saidLocked, QStringLiteral("a locked window says nothing of what became of a payment: %1").arg(scene.window->lastPaymentAlert()));
        bool passwordOff = false;
        controller->changePassword(testPassword, QString(), false, scene.window.get(), [&passwordOff](const QString&) { passwordOff = true; });
        check(waitFor([&] { return passwordOff && !controller->hasPassword(); }, 15000), QStringLiteral("(no password again)"));
        wallet::seal::setCostForTest(0, 0);
        scene.window->lockPage()->field()->insert(testPassword);
        scene.window->unlockForTest();

        // Nothing said when it is not wanted, and the list still kept.
        const int kept2 = scene.payments();
        const QString lastSaid = scene.window->lastPaymentAlert();
        SettingsRepo().set(QStringLiteral("ui.paymentAlerts"), QStringLiteral("0"));
        setToken(8, 10000000);
        pause(50);
        check(scene.window->lastPaymentAlert() == lastSaid && scene.payments() == kept2 + 1, QStringLiteral("with alerts off the list is kept, and nothing said"));
        SettingsRepo().set(QStringLiteral("ui.paymentAlerts"), QStringLiteral("1"));

        // The list.
        const int unread = payments->unread();
        check(unread == scene.payments() && title->unreadPayments() == unread, QStringLiteral("every one waits to be looked at: %1").arg(unread));
        scene.window->openPayments();
        auto* dialog = scene.window->findChild<ui::PaymentsDialog*>();
        check(dialog && dialog->isVisible(), QStringLiteral("the bell opens the list"));
        if (dialog) {
            QTableView* list = dialog->view();
            check(list->model()->rowCount() == scene.payments(), QStringLiteral("with all of them"));
            check(list->model()->index(0, 1).data().toString() == QStringLiteral("+10.00 USDT"), QStringLiteral("the latest first: %1").arg(list->model()->index(0, 1).data().toString()));
            check(list->model()->index(0, 3).data().toString() == QStringLiteral("Shop"), QStringLiteral("named by its wallet"));
            check(list->model()->index(0, 5).data().toString() == QStringLiteral("Balance went up"), QStringLiteral("saying what that rests on"));
            const int lastRow = list->model()->rowCount() - 1;
            check(list->model()->index(lastRow, 5).data().toString() == QStringLiteral("Not in a block yet"), QStringLiteral("a transaction by how it stands: %1").arg(list->model()->index(lastRow, 5).data().toString()));
            scene.book->setHeight(first, 900000);
            check(list->model()->index(lastRow, 5).data().toString() == QStringLiteral("In a block, proof on its way"), QStringLiteral("on a server's word: %1").arg(list->model()->index(lastRow, 5).data().toString()));
            scene.book->setProven(first, true);
            check(list->model()->index(lastRow, 5).data().toString() == QStringLiteral("In a block"), QStringLiteral("proven"));
            check(list->model()->index(0, 1).data(Qt::FontRole).value<QFont>().weight() > QFont::Normal, QStringLiteral("what has not been looked at is bold"));
            const int shownNow = list->model()->rowCount();
            setToken(9, 10000000);
            check(list->model()->rowCount() == shownNow + 1, QStringLiteral("it goes on filling while it is open"));
            auto* open = dialog->findChild<QPushButton*>(QStringLiteral("paymentsOpen"));
            check(open && !open->isEnabled(), QStringLiteral("nothing chosen, nothing to open"));
            list->selectRow(lastRow + 1);
            check(open && open->isEnabled(), QStringLiteral("a row chosen can be"));
            if (open)
                open->click();
            check(!dialog->isVisible() || waitFor([&] { return !scene.window->findChild<ui::PaymentsDialog*>() || !scene.window->findChild<ui::PaymentsDialog*>()->isVisible(); }, 2000), QStringLiteral("it closes"));
            check(payments->unread() == 0 && title->unreadPayments() == 0, QStringLiteral("and all of it counts as looked at"));
            const ui::Place place = scene.window->placeBar()->place();
            check(place.address && place.address->address == model->at(scene.rowOf(1)).address && scene.window->placeBar()->tab() == ui::Place::Tab::History, QStringLiteral("on the address it came to, on its history"));
        }

        // A new one after that counts alone.
        setToken(10, 10000000);
        check(payments->unread() == 1 && title->unreadPayments() == 1, QStringLiteral("what comes next counts from one"));
        told.clear();
        pause(50);
    }

    // Gone with the window, and back with the next.
    {
        Scene scene;
        if (!makeScene(&scene, keys, check))
            return;
        wallet::PaymentBook* payments = scene.controller->payments();
        check(waitFor([&] { return scene.payments() > 0; }, 8000), QStringLiteral("what came in is there at the next start"));
        check(payments->unread() == 1 && scene.window->titleBar()->unreadPayments() == 1, QStringLiteral("as it stood: %1 not looked at").arg(payments->unread()));
        const int count = scene.payments();
        const wallet::Payment first = payments->all().first();
        check(first.amount.toDecimalString() == "50000" && first.hasTx && first.asset == QLatin1String("bitcoin/btc"), QStringLiteral("the first of them whole"));
        QList<wallet::Payment> told2;
        QObject::connect(payments, &wallet::PaymentBook::told, payments, [&told2](const QList<wallet::Payment>& some) { told2.append(some); });
        pause(50);
        check(told2.isEmpty(), QStringLiteral("and none is told again"));
        // Told of before this start, far back by now, gone from the history since, and listed again.
        QList<wallet::Payment> since;
        for (int i = 0; i < 100; ++i) {
            wallet::Payment later;
            later.id = payments->all().constLast().id + 1 + i;
            later.at = later.id;
            later.asset = QStringLiteral("bitcoin/btc");
            later.amount = chain::Amount::fromU64(5000);
            later.read = true;
            since.append(later);
        }
        payments->addSaved(since);
        const int countNow = scene.payments();
        const chain::Transaction listedAgain = transaction({somewhere(30)}, {{scene.scriptOf(17), 40000}}, 30);
        check(scene.book->find(listedAgain.txid) < 0, QStringLiteral("(not in the history)"));
        scene.book->setStatus(scene.model->at(scene.rowOf(17)).scripthash, scene.model->at(scene.rowOf(17)).status);
        scene.paid(17, QStringLiteral("x3"), 0, 40000);
        scene.add(listedAgain);
        check(scene.book->find(listedAgain.txid) >= 0 && scene.payments() == countNow, QStringLiteral("nor counted twice"));
        Q_UNUSED(count);

        // The latest so many stay.
        QList<wallet::Payment> many;
        for (int i = 0; i < wallet::PaymentBook::kMaxKept; ++i) {
            wallet::Payment old;
            old.id = i + 1;
            old.at = 1000 + i;
            old.asset = QStringLiteral("bitcoin/btc");
            old.amount = chain::Amount::fromU64(1000);
            old.read = true;
            many.append(old);
        }
        payments->addSaved(many);
        check(scene.payments() == wallet::PaymentBook::kMaxKept, QStringLiteral("the list keeps the latest %1").arg(wallet::PaymentBook::kMaxKept));
        check(payments->all().constLast().id > many.constLast().id, QStringLiteral("those"));

        // A wallet removed takes what it was paid along.
        QString removeError;
        const int ofShop = int(std::count_if(payments->all().cbegin(), payments->all().cend(), [&scene](const wallet::Payment& payment) { return payment.wallet == scene.shop; }));
        check(ofShop > 0, QStringLiteral("(some are of that wallet)"));
        check(scene.controller->removeWallet(scene.shop, &removeError), QStringLiteral("a wallet removed: %1").arg(removeError));
        check(std::none_of(payments->all().cbegin(), payments->all().cend(), [&scene](const wallet::Payment& payment) { return payment.wallet == scene.shop; }), QStringLiteral("takes its payments along"));
        check(scene.window->titleBar()->unreadPayments() == payments->unread(), QStringLiteral("off the bell too"));
        check(scene.controller->removeAllWallets(&removeError), QStringLiteral("it all goes: %1").arg(removeError));
        check(scene.payments() == 0, QStringLiteral("and so does the list"));
    }
    wallet::PaymentBook::setPauseForTest(1000);
}

} // namespace

// On the real platform, where the tray is: whether a notification gets as far as Windows.
bool checkTrayAlert(const QString& file)
{
    const QString wasSet = SettingsRepo().get(QStringLiteral("ui.paymentAlertsWindows"));
    SettingsRepo().set(QStringLiteral("ui.paymentAlertsWindows"), QStringLiteral("1"));
    ui::MainWindow window;
    window.show();
    pause(300);
    window.showMinimized();
    pause(300);
    const Qt::ApplicationState state = QGuiApplication::applicationState();
    window.sayPaymentForTest(QStringLiteral("GitWallet test: a check of its notifications, on a folder with no wallet. No payment came in."));
    pause(1000);
    SettingsRepo().set(QStringLiteral("ui.paymentAlertsWindows"), wasSet);
    const QJsonObject said{{QStringLiteral("trayAvailable"), QSystemTrayIcon::isSystemTrayAvailable()},
                           {QStringLiteral("supportsMessages"), QSystemTrayIcon::supportsMessages()},
                           {QStringLiteral("trayShown"), window.trayShown()},
                           {QStringLiteral("applicationActive"), state == Qt::ApplicationActive},
                           {QStringLiteral("platform"), QGuiApplication::platformName()}};
    QFile out(file);
    return out.open(QIODevice::WriteOnly | QIODevice::Truncate) && out.write(QJsonDocument(said).toJson()) > 0;
}

void runAlertsTest(const Check& check, const QList<wallet::AccountKey>& someKeys)
{
    testDust(check, someKeys);
    testPayments(check, someKeys);
}

} // namespace app
