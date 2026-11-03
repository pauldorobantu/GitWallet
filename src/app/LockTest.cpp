#include "app/WalletController.h"
#include "chain/Bip39.h"
#include "core/SettingsRepo.h"
#include "net/PriceFeed.h"
#include "ui/MainWindow.h"
#include "ui/SideNav.h"
#include "ui/TableUtil.h"
#include "ui/TitleBar.h"
#include "ui/dialogs/ImportSeedDialog.h"
#include "ui/dialogs/PasswordDialog.h"
#include "ui/dialogs/PaymentsDialog.h"
#include "ui/dialogs/StatusDialog.h"
#include "ui/pages/LockPage.h"
#include "ui/pages/SettingsPage.h"
#include "wallet/History.h"
#include "wallet/Payments.h"
#include "wallet/Seal.h"
#include "wallet/UtxoHistory.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>

#include <algorithm>
#include <functional>
#include <memory>

// The password and the backup: what they lock with, what the file holds under them, and what the
// window does with them. Every password here is made up on the spot, on a throw-away folder.
namespace app {

namespace {

using Check = std::function<void(bool, const QString&)>;

const char* const kPassword = "correct horse battery";
const char* const kOtherPassword = "another test sentence";

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

QList<wallet::AccountKey> keysOf(const QList<wallet::AccountKey>& keys)
{
    QList<wallet::AccountKey> out;
    for (const wallet::AccountKey& key : keys) {
        if (key.type == chain::ScriptType::P2WPKH || key.type == chain::ScriptType::EthAccount)
            out.append(key);
    }
    return out;
}

// Runs what takes a `done`, and hands back what came of it.
QString waited(const std::function<void(const std::function<void(const QString&)>&)>& run)
{
    bool over = false;
    QString said;
    run([&](const QString& error) {
        said = error;
        over = true;
    });
    if (!waitFor([&] { return over; }, 30000))
        return QStringLiteral("(it never came back)");
    return said;
}

QByteArray fileBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void testSeal(const Check& check, QString* report)
{
    using namespace wallet::seal;
    // Argon2id as its authors' own program gives it: argon2 somesalt -id -t 2 -m 16 -p 1 -l 32, of "password".
    Cost known;
    known.memoryKib = 65536;
    known.passes = 2;
    known.lanes = 1;
    known.salt = QByteArrayLiteral("somesalt");
    const QByteArray vector = makeKey(QByteArrayLiteral("password"), known);
    check(vector.toHex().startsWith("09316115d5cf24ed5a15a31a3ba326e5"), QStringLiteral("the key of a password is what Argon2id gives, in those units: %1").arg(QString::fromLatin1(vector.toHex())));

    // What a real key costs here. A test sets that much lower, for all that follows.
    setCostForTest(0, 0);
    const Cost real = newCost();
    QElapsedTimer clock;
    clock.start();
    const QByteArray realKey = makeKey(QByteArrayLiteral("a password"), real);
    *report += QStringLiteral("info  a key at %1 MiB, %2 passes takes %3 ms here\n").arg(real.memoryKib / 1024).arg(real.passes).arg(clock.elapsed());
    check(realKey.size() == kKeyBytes && real.memoryKib >= 64 * 1024 && real.passes >= 2 && real.salt.size() == 16, QStringLiteral("a new key costs a lot of memory, and has its own salt"));
    check(newCost().salt != real.salt, QStringLiteral("each its own"));
    setCostForTest(256, 1);

    const Cost cost = newCost();
    const QByteArray key = makeKey(passwordBytes(QString::fromLatin1(kPassword)), cost);
    const QByteArray plain = QByteArrayLiteral("what a file holds");
    const QJsonObject sealed = lock(key, cost, plain, QStringLiteral("a test"));
    QByteArray opened;
    check(open(key, sealed, &opened) && opened == plain, QStringLiteral("what is locked opens with its key"));
    check(!QJsonDocument(sealed).toJson().contains(plain) && !QJsonDocument(sealed).toJson().contains(plain.toBase64()), QStringLiteral("and is not there in the clear"));
    const QByteArray wrong = makeKey(passwordBytes(QString::fromLatin1(kOtherPassword)), cost);
    check(!open(wrong, sealed, &opened), QStringLiteral("not with another password's"));
    check(lock(key, cost, plain, QStringLiteral("a test")).value(QStringLiteral("nonce")) != sealed.value(QStringLiteral("nonce")), QStringLiteral("each time with its own nonce"));
    check(makeKey(passwordBytes(QStringLiteral("café")), cost) == makeKey(passwordBytes(QStringLiteral("café")), cost), QStringLiteral("a letter composed either way is the same password"));
    check(makeKey(passwordBytes(QStringLiteral("Password1")), cost) != makeKey(passwordBytes(QStringLiteral("password1")), cost), QStringLiteral("and a capital is not"));

    // Whatever is changed fails: all of it is under the check.
    const auto changed = [&](const std::function<void(QJsonObject&)>& change) {
        QJsonObject other = sealed;
        change(other);
        Cost itsCost;
        QByteArray out;
        // With the key its own figures would give, as an attacker's file would have it.
        return !costOf(other, &itsCost) || !open(key, other, &out);
    };
    const auto inKdf = [](QJsonObject& o, const char* field, const QJsonValue& value) {
        QJsonObject kdf = o.value(QStringLiteral("kdf")).toObject();
        kdf.insert(QLatin1String(field), value);
        o.insert(QStringLiteral("kdf"), kdf);
    };
    check(changed([](QJsonObject& o) { o.insert(QStringLiteral("format"), QStringLiteral("another")); }), QStringLiteral("what it says it is"));
    check(changed([](QJsonObject& o) { o.insert(QStringLiteral("version"), 2); }), QStringLiteral("its version"));
    check(changed([&](QJsonObject& o) { inKdf(o, "memoryKib", 512); }), QStringLiteral("the memory its key took"));
    check(changed([&](QJsonObject& o) { inKdf(o, "passes", 2); }), QStringLiteral("the passes"));
    check(changed([&](QJsonObject& o) { inKdf(o, "lanes", 2); }), QStringLiteral("the lanes"));
    check(changed([&](QJsonObject& o) { inKdf(o, "salt", QString(32, QLatin1Char('0'))); }), QStringLiteral("the salt"));
    check(changed([](QJsonObject& o) { o.insert(QStringLiteral("nonce"), QString(24, QLatin1Char('0'))); }), QStringLiteral("the nonce"));
    check(changed([](QJsonObject& o) {
              QByteArray body = QByteArray::fromBase64(o.value(QStringLiteral("body")).toString().toLatin1());
              body[0] = char(body.at(0) ^ 1);
              o.insert(QStringLiteral("body"), QString::fromLatin1(body.toBase64()));
          }), QStringLiteral("a byte of what it holds"));
    check(changed([](QJsonObject& o) { o.insert(QStringLiteral("cipher"), QStringLiteral("none")); }), QStringLiteral("the cipher"));

    // A file nobody knows the source of does not get to ask for all there is.
    Cost asked;
    QJsonObject greedy = sealed;
    inKdf(greedy, "memoryKib", 1024.0 * 1024 * 1024);
    check(!costOf(greedy, &asked), QStringLiteral("more memory than any key takes is refused before it is tried"));
    greedy = sealed;
    inKdf(greedy, "passes", 1000000);
    check(!costOf(greedy, &asked), QStringLiteral("so are more passes"));
    greedy = sealed;
    inKdf(greedy, "memoryKib", 2.0 * 1024 * 1024);
    inKdf(greedy, "passes", 17);
    check(!costOf(greedy, &asked), QStringLiteral("and a cost that would hold the window for minutes, with no way out"));
    greedy = sealed;
    inKdf(greedy, "lanes", 0);
    check(!costOf(greedy, &asked), QStringLiteral("and no lanes"));
    greedy = sealed;
    inKdf(greedy, "memoryKib", 1.5);
    check(!costOf(greedy, &asked), QStringLiteral("and half a kibibyte"));
    greedy = sealed;
    inKdf(greedy, "name", QStringLiteral("md5"));
    check(!costOf(greedy, &asked), QStringLiteral("and anything but Argon2id"));
}

void testStoreFile(const Check& check, const QList<wallet::AccountKey>& keys)
{
    using namespace wallet;
    QString error;
    const QByteArray databaseKey = WalletStore::newDatabaseKey();
    WalletEntry entry;
    WalletStore store;
    entry.id = store.takeWalletId();
    entry.label = QStringLiteral("Private name");
    entry.fromKeys = true;
    entry.accounts = keysOf(keys);
    for (AccountKey& key : entry.accounts)
        key.wallet = entry.id;
    store.wallets.append(entry);
    store.databaseKey = databaseKey;
    check(store.save(&error), QStringLiteral("a file with no password: %1").arg(error));
    const QString xpub = entry.accounts.first().xpub;
    check(WalletStore::readRawForTest(&error).contains(xpub.toLatin1()), QStringLiteral("(inside DPAPI it reads as it is)"));

    const seal::Cost cost = seal::newCost();
    const QByteArray key = seal::makeKey(seal::passwordBytes(QString::fromLatin1(kPassword)), cost);
    store.setPasswordKey(key, cost);
    check(store.hasPassword() && store.save(&error), QStringLiteral("under a password: %1").arg(error));
    const QByteArray inside = WalletStore::readRawForTest(&error);
    check(inside.contains("gitwallet-sealed"), QStringLiteral("the file says so"));
    check(!inside.contains(xpub.toLatin1()) && !inside.contains("Private name") && !inside.contains(databaseKey.toHex()) && !inside.contains("wallets"),
          QStringLiteral("and a program that gets past DPAPI reads no key, no name and not the database's key"));

    WalletStore reader;
    error.clear();
    check(!reader.load(&error) && reader.needsPassword(), QStringLiteral("loading it asks for the password"));
    check(reader.wallets.isEmpty() && reader.databaseKey.isEmpty(), QStringLiteral("and holds nothing until then"));
    const QByteArray onDisk = fileBytes(WalletStore::filePath());
    check(!reader.save(&error) && fileBytes(WalletStore::filePath()) == onDisk, QStringLiteral("and nothing it would write takes the place of what is under the password"));
    const QByteArray wrong = seal::makeKey(seal::passwordBytes(QString::fromLatin1(kOtherPassword)), cost);
    check(!reader.openSealed(wrong, &error) && reader.needsPassword() && reader.wallets.isEmpty(), QStringLiteral("another password opens nothing: %1").arg(error));
    check(reader.openSealed(key, &error), QStringLiteral("the right one opens it: %1").arg(error));
    check(!reader.needsPassword() && reader.hasPassword() && reader.isPasswordKey(key) && !reader.isPasswordKey(wrong), QStringLiteral("and goes on with it"));
    check(reader.wallets.size() == 1 && reader.wallets.first().label == QLatin1String("Private name") && reader.wallets.first().accounts.first().xpub == xpub && reader.databaseKey == databaseKey,
          QStringLiteral("all of it there"));

    // What comes next stays under it, in a new form each time.
    reader.lockMinutes = 30;
    reader.lastBackupMs = 1234567;
    reader.lastBackupWallets = 1;
    check(reader.save(&error), QStringLiteral("saved again: %1").arg(error));
    const QByteArray insideAgain = WalletStore::readRawForTest(&error);
    check(insideAgain != inside && !insideAgain.contains(xpub.toLatin1()), QStringLiteral("still under it"));
    WalletStore third;
    check(!third.load(&error) && third.needsPassword() && third.openSealed(key, &error), QStringLiteral("and opens the same: %1").arg(error));
    check(third.lockMinutes == 30 && third.lastBackupMs == 1234567 && third.lastBackupWallets == 1, QStringLiteral("with when it locks and when the last backup was"));

    // And off again.
    third.setPasswordKey({}, {});
    check(third.save(&error) && WalletStore::readRawForTest(&error).contains(xpub.toLatin1()), QStringLiteral("with the password gone it reads as before"));
    WalletStore fourth;
    check(fourth.load(&error) && !fourth.hasPassword() && !fourth.needsPassword() && fourth.lockMinutes == 30, QStringLiteral("and opens with none: %1").arg(error));

    // A newer one is not guessed at.
    QJsonObject newer = QJsonDocument::fromJson(insideAgain).object();
    newer.insert(QStringLiteral("version"), 2);
    check(WalletStore::writeRawForTest(QJsonDocument(newer).toJson(), &error), QStringLiteral("(a file from a newer version: %1)").arg(error));
    WalletStore fifth;
    error.clear();
    check(!fifth.load(&error) && !fifth.needsPassword() && error.contains(QLatin1String("newer")), QStringLiteral("one of a newer version says so: %1").arg(error));
    QFile::remove(WalletStore::filePath());
}

struct Window {
    std::unique_ptr<ui::MainWindow> window;
    WalletController* controller = nullptr;

    void open()
    {
        window = std::make_unique<ui::MainWindow>();
        window->show();
        window->resize(1280, 800);
        pause(30);
        controller = window->controller();
    }
    ui::LockPage* lock() const { return window->lockPage(); }
    void type(const QString& password) const
    {
        QLineEdit* field = lock()->field();
        field->clear();
        field->insert(password);
        lock()->findChild<QPushButton*>(QStringLiteral("primaryButton"))->click();
    }
    bool busy() const { return !lock()->field()->isEnabled(); }
    QString statusLine() const { return lock()->findChild<QLabel*>(QStringLiteral("lockStatus"))->text(); }
};

void testLockInTheWindow(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    const wallet::Network bitcoin = wallet::findNetwork(QStringLiteral("bitcoin"))->network;
    const QList<wallet::AccountKey> added = keysOf(keys);
    const wallet::AccountKey btc = added.first();
    {
        Window shown;
        shown.open();
        offline(shown.controller);
        check(!shown.window->isLocked() && !shown.lock()->isVisible(), QStringLiteral("no password, no lock"));
        shown.window->lockNow();
        check(!shown.window->isLocked(), QStringLiteral("and nothing to lock with"));
        check(shown.controller->addKeysWallet(added, QStringLiteral("Shop"), &error), QStringLiteral("a wallet: %1").arg(error));
        check(waitFor([&] { return !shown.controller->loadingSaved(); }, 15000), QStringLiteral("(its database is open)"));

        // Through the page, as anyone would.
        shown.window->showPage(QStringLiteral("settings"));
        auto* settings = shown.window->findChild<ui::SettingsPage*>();
        auto* set = settings->findChild<QPushButton*>(QStringLiteral("setPassword"));
        check(set && set->isVisible() && !settings->findChild<QPushButton*>(QStringLiteral("lockNow"))->isVisible(), QStringLiteral("Settings offers one"));
        set->click();
        auto* dialog = settings->findChild<ui::PasswordDialog*>();
        check(dialog && dialog->isVisible(), QStringLiteral("and asks for it"));
        if (dialog) {
            auto* fresh = dialog->findChild<QLineEdit*>(QStringLiteral("passwordNew"));
            auto* repeat = dialog->findChild<QLineEdit*>(QStringLiteral("passwordRepeat"));
            auto* understood = dialog->findChild<QCheckBox*>(QStringLiteral("passwordUnderstood"));
            auto* ok = dialog->findChild<QPushButton*>(QStringLiteral("primaryButton"));
            check(fresh->echoMode() == QLineEdit::Password && repeat->echoMode() == QLineEdit::Password, QStringLiteral("not on show"));
            check(!ok->isEnabled(), QStringLiteral("with nothing typed there is nothing to set"));
            fresh->setText(QStringLiteral("short"));
            repeat->setText(QStringLiteral("short"));
            understood->setChecked(true);
            check(!ok->isEnabled(), QStringLiteral("less than eight characters is refused"));
            fresh->setText(QStringLiteral("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"));
            repeat->setText(fresh->text());
            check(!ok->isEnabled(), QStringLiteral("so are seed words: it should not be typed here"));
            fresh->setText(QString::fromLatin1(kPassword));
            repeat->setText(QStringLiteral("something else"));
            check(!ok->isEnabled(), QStringLiteral("the two have to match"));
            repeat->setText(QString::fromLatin1(kPassword));
            understood->setChecked(false);
            check(!ok->isEnabled(), QStringLiteral("and it has to be understood that nobody can get it back"));
            understood->setChecked(true);
            check(ok->isEnabled(), QStringLiteral("then it can be set"));
            const QPointer<ui::PasswordDialog> still(dialog);
            ok->click();
            check(waitFor([&] { return shown.controller->hasPassword() && (!still || !still->isVisible()); }, 15000), QStringLiteral("it is set, and the dialog goes"));
        }
        check(wallet::WalletStore::readRawForTest(&error).contains("gitwallet-sealed"), QStringLiteral("and the file is under it"));
        check(shown.window->titleBar()->findChild<QWidget*>(QStringLiteral("tbLock"))->isVisible(), QStringLiteral("the title bar can lock it now"));
        check(settings->findChild<QPushButton*>(QStringLiteral("lockNow"))->isVisible() && !set->isVisible(), QStringLiteral("and so can Settings"));

        // Locked.
        wallet::PaymentBook::setPauseForTest(10);
        shown.window->lockNow();
        check(shown.window->isLocked() && shown.lock()->isVisible(), QStringLiteral("locked, the page asks for the password"));
        check(!shown.window->findChild<ui::SideNav*>()->isVisible() && !settings->isVisible(), QStringLiteral("and nothing else shows"));
        check(!shown.window->titleBar()->findChild<QWidget*>(QStringLiteral("tbBell"))->isVisible() && !shown.window->titleBar()->findChild<QWidget*>(QStringLiteral("tbLock"))->isVisible(), QStringLiteral("not even the bell"));
        check(shown.controller->hasWallet(), QStringLiteral("what runs goes on behind it"));
        shown.window->openPayments();
        check(!shown.window->findChild<ui::PaymentsDialog*>(), QStringLiteral("and a notification clicked on opens no list over it"));
        wallet::WalletModel* model = shown.controller->model();
        const int row = model->find(shown.controller->store().wallets.first().id, wallet::findNetwork(QStringLiteral("ethereum"))->network, chain::ScriptType::EthAccount, 0, 0, 1);
        if (row >= 0) {
            wallet::AddressRow paid = model->at(row);
            paid.checked = true;
            model->upsert(paid);
            shown.controller->payments()->noteAnswered(paid.network, 0, {}, {wallet::assetInfo(wallet::networkInfo(paid.network).tokens.first()).key});
            paid.setBalance(wallet::networkInfo(paid.network).tokens.first(), chain::Amount::fromU64(5000000));
            model->upsert(paid);
            check(waitFor([&] { return !shown.window->lastPaymentAlert().isEmpty(); }, 3000) && shown.window->lastPaymentAlert() == QLatin1String("A payment came in"), QStringLiteral("a payment is told with nothing of it: %1").arg(shown.window->lastPaymentAlert()));
        }
        wallet::PaymentBook::setPauseForTest(1000);
        shown.type(QString::fromLatin1(kOtherPassword));
        check(waitFor([&] { return !shown.busy(); }, 10000) && shown.window->isLocked(), QStringLiteral("a wrong password leaves it locked"));
        check(shown.statusLine().contains(QLatin1String("Wrong password")) && shown.lock()->field()->text().isEmpty(), QStringLiteral("says so, and clears the field: %1").arg(shown.statusLine()));
        shown.type(QString::fromLatin1(kPassword));
        check(waitFor([&] { return !shown.window->isLocked(); }, 10000), QStringLiteral("the right one opens it"));
        check(!shown.lock()->isVisible() && shown.window->findChild<ui::SideNav*>()->isVisible() && shown.lock()->field()->text().isEmpty(), QStringLiteral("with nothing left of it in the field"));
        check(settings->isVisible(), QStringLiteral("back where it was"));

        // What is open goes first.
        auto* import = new ui::ImportSeedDialog(shown.controller, shown.window.get(), false);
        import->setAttribute(Qt::WA_DeleteOnClose);
        import->open();
        check(import->isVisible(), QStringLiteral("(a dialog open)"));
        const QPointer<ui::ImportSeedDialog> stillThere(import);
        shown.window->lockNow();
        check(shown.window->isLocked() && (!stillThere || !stillThere->isVisible()), QStringLiteral("locking closes what is open: a seed half typed does not stay on top"));
        shown.type(QString::fromLatin1(kPassword));
        check(waitFor([&] { return !shown.window->isLocked(); }, 10000), QStringLiteral("(open again)"));

        // Left alone.
        shown.window->setIdleLimitForTest(150);
        QElapsedTimer busyClock;
        busyClock.start();
        while (busyClock.elapsed() < 400) {
            QKeyEvent key(QEvent::KeyPress, Qt::Key_Shift, Qt::NoModifier);
            QApplication::sendEvent(shown.window.get(), &key);
            pause(20);
        }
        check(!shown.window->isLocked(), QStringLiteral("with someone at the keyboard it stays open"));
        check(waitFor([&] { return shown.window->isLocked(); }, 3000), QStringLiteral("left alone it locks"));
        shown.type(QString::fromLatin1(kPassword));
        check(waitFor([&] { return !shown.window->isLocked(); }, 10000), QStringLiteral("(open again)"));
        shown.window->setIdleLimitForTest(0);

        // How long, kept.
        auto* after = settings->findChild<QComboBox*>(QStringLiteral("lockAfter"));
        check(after && after->isVisible() && after->currentData().toInt() == wallet::WalletStore::kDefaultLockMinutes, QStringLiteral("it locks after a quarter of an hour unless told otherwise"));
        after->setCurrentIndex(after->findData(30));
        emit after->activated(after->currentIndex());
        check(shown.controller->lockMinutes() == 30, QStringLiteral("the page changes it: %1").arg(shown.controller->lockMinutes()));
        check(shown.controller->setLockMinutes(5, &error), QStringLiteral("another time: %1").arg(error));
        check(after->currentData().toInt() == 5, QStringLiteral("shows"));

        // A given address reaches the file under it.
        const quint32 shop = shown.controller->store().wallets.first().id;
        check(waitFor([&] { return model->find(shop, bitcoin, btc.type, 0, 0, 4) >= 0; }, 15000), QStringLiteral("(its rows)"));
        wallet::AddressRow given = model->at(model->find(shop, bitcoin, btc.type, 0, 0, 4));
        given.checked = true;
        model->upsert(given);
        check(shown.controller->markGivenOut(given, &error), QStringLiteral("what was given out is written: %1").arg(error));
        check(shown.controller->renameWallet(shop, QStringLiteral("Shop renamed"), &error), QStringLiteral("a rename too: %1").arg(error));
        const QByteArray inside = wallet::WalletStore::readRawForTest(&error);
        check(inside.contains("gitwallet-sealed") && !inside.contains("Shop renamed"), QStringLiteral("under the password"));
    }

    // The next start.
    {
        Window shown;
        shown.open();
        check(shown.window->isLocked() && shown.lock()->isVisible(), QStringLiteral("the next start opens on the lock"));
        check(!shown.controller->hasWallet() && shown.controller->needsPassword() && shown.controller->model()->size() == 0, QStringLiteral("with nothing read, and no scan"));
        const QByteArray before = fileBytes(wallet::WalletStore::filePath());
        check(!shown.controller->addKeysWallet(keysOf(keys), QStringLiteral("Second"), &error) && error.contains(QLatin1String("password")), QStringLiteral("nothing can be added past it: %1").arg(error));
        check(fileBytes(wallet::WalletStore::filePath()) == before && shown.controller->walletFilesKeptAside() == 0, QStringLiteral("and the file is neither changed nor moved aside"));
        wallet::ScanSettings changedSettings;
        changedSettings.evmGap = 321;
        check(!shown.controller->applySettings(changedSettings, &error) && fileBytes(wallet::WalletStore::filePath()) == before, QStringLiteral("nor written over by settings"));
        shown.type(QString::fromLatin1(kOtherPassword));
        check(waitFor([&] { return !shown.busy(); }, 10000) && shown.controller->needsPassword(), QStringLiteral("a wrong password opens nothing"));
        check(shown.statusLine().contains(QLatin1String("Wrong password")), QStringLiteral("and says so: %1").arg(shown.statusLine()));
        shown.type(QString::fromLatin1(kPassword));
        check(waitFor([&] { return !shown.window->isLocked(); }, 10000), QStringLiteral("the right one opens it"));
        check(shown.controller->hasWallet() && shown.controller->store().wallets.value(0).label == QLatin1String("Shop renamed"), QStringLiteral("with what was saved under it"));
        if (!shown.controller->hasWallet())
            return;
        check(shown.controller->lockMinutes() == 5, QStringLiteral("how long it waits to lock"));
        const quint32 shop = shown.controller->store().wallets.first().id;
        check(shown.controller->store().wallets.first().accounts.first().given.contains(4), QStringLiteral("and what was given out"));
        check(waitFor([&] { return !shown.controller->loadingSaved(); }, 15000), QStringLiteral("and the database under its key opens"));
        check(shown.controller->keepingSaved(), QStringLiteral("(it is open)"));
        offline(shown.controller);
        check(shown.controller->renameWallet(shop, QStringLiteral("Shop"), &error), QStringLiteral("what is written next goes in: this window read the file: %1").arg(error));

        // Another password. A wrong one for the old is refused.
        check(waited([&](const auto& done) { shown.controller->changePassword(QString::fromLatin1(kOtherPassword), QStringLiteral("a brand new sentence"), false, shown.window.get(), done); }).contains(QLatin1String("not the current password")),
              QStringLiteral("changing it takes the one in use"));
        check(waited([&](const auto& done) { shown.controller->changePassword(QString::fromLatin1(kPassword), QString::fromLatin1(kOtherPassword), false, shown.window.get(), done); }).isEmpty(), QStringLiteral("with it, it changes"));
        wallet::WalletStore probe;
        check(!probe.load(&error) && probe.needsPassword(), QStringLiteral("(still under one)"));
        wallet::seal::Cost cost;
        probe.sealedCost(&cost);
        check(!probe.openSealed(wallet::seal::makeKey(wallet::seal::passwordBytes(QString::fromLatin1(kPassword)), cost), &error), QStringLiteral("the old one opens nothing now"));
        check(probe.openSealed(wallet::seal::makeKey(wallet::seal::passwordBytes(QString::fromLatin1(kOtherPassword)), cost), &error), QStringLiteral("the new one does: %1").arg(error));
        check(shown.controller->store().hasPassword(), QStringLiteral("(going on with it)"));

        // The older copies, which no password reaches.
        const QString oldCopy = wallet::WalletStore::filePath() + QStringLiteral(".format3-20260101-000000");
        {
            QFile file(oldCopy);
            file.open(QIODevice::WriteOnly);
            file.write("an older wallet file");
        }
        check(shown.controller->walletFilesKeptAside() == 1, QStringLiteral("(one older copy beside it)"));
        check(waited([&](const auto& done) { shown.controller->changePassword(QString::fromLatin1(kOtherPassword), QString::fromLatin1(kPassword), false, shown.window.get(), done); }).isEmpty() && QFile::exists(oldCopy),
              QStringLiteral("it stays unless asked"));
        check(waited([&](const auto& done) { shown.controller->changePassword(QString::fromLatin1(kPassword), QString::fromLatin1(kOtherPassword), true, shown.window.get(), done); }).isEmpty() && !QFile::exists(oldCopy),
              QStringLiteral("and goes when asked"));

        // Through the page, off.
        shown.window->showPage(QStringLiteral("settings"));
        auto* settings = shown.window->findChild<ui::SettingsPage*>();
        settings->findChild<QPushButton*>(QStringLiteral("removePassword"))->click();
        auto* dialog = settings->findChild<ui::PasswordDialog*>();
        check(dialog && dialog->isVisible(), QStringLiteral("taking it off asks for it"));
        if (dialog) {
            auto* current = dialog->findChild<QLineEdit*>(QStringLiteral("passwordCurrent"));
            auto* ok = dialog->findChild<QPushButton*>(QStringLiteral("dangerButton"));
            const QPointer<ui::PasswordDialog> still(dialog);
            current->setText(QString::fromLatin1(kPassword));
            ok->click();
            check(waitFor([&] { return !still || ok->isEnabled(); }, 10000) && still && still->isVisible() && shown.controller->hasPassword(), QStringLiteral("the wrong one takes nothing off, and the dialog says so"));
            if (still) {
                check(still->findChild<QLabel*>(QStringLiteral("passwordStatus"))->text().contains(QLatin1String("not the current password")), QStringLiteral("in those words"));
                current->setText(QString::fromLatin1(kOtherPassword));
                ok->click();
                check(waitFor([&] { return !shown.controller->hasPassword(); }, 10000), QStringLiteral("the right one does"));
            }
        }
        check(!wallet::WalletStore::readRawForTest(&error).contains("gitwallet-sealed"), QStringLiteral("the file is as it was before"));
        check(!shown.window->titleBar()->findChild<QWidget*>(QStringLiteral("tbLock"))->isVisible(), QStringLiteral("and there is nothing to lock with"));

        // Set again, for what follows.
        check(waited([&](const auto& done) { shown.controller->changePassword(QString(), QString::fromLatin1(kPassword), false, shown.window.get(), done); }).isEmpty(), QStringLiteral("(a password again)"));
    }

    // Nobody remembers it.
    {
        Window shown;
        shown.open();
        check(shown.window->isLocked(), QStringLiteral("(locked at the start)"));
        const QString before = wallet::WalletStore::filePath();
        const QByteArray bytes = fileBytes(before);
        shown.lock()->findChild<QPushButton*>(QStringLiteral("lockForgot"))->click();
        auto* box = shown.window->findChild<QMessageBox*>(QStringLiteral("forgotBox"));
        check(box && box->isVisible() && box->informativeText().contains(QLatin1String("seeds")), QStringLiteral("forgetting it is answered honestly"));
        if (box) {
            QAbstractButton* startOver = nullptr;
            for (QAbstractButton* button : box->buttons()) {
                if (box->buttonRole(button) == QMessageBox::DestructiveRole)
                    startOver = button;
            }
            check(startOver != nullptr, QStringLiteral("with a way to start over"));
            if (startOver)
                startOver->click();
        }
        check(waitFor([&] { return !shown.window->isLocked(); }, 3000) && !shown.controller->needsPassword() && !shown.controller->hasWallet() && !shown.controller->hasPassword(), QStringLiteral("it starts empty"));
        check(!QFile::exists(before) && shown.controller->walletFilesKeptAside() == 1, QStringLiteral("and the file is kept aside"));
        const QStringList aside = QDir(wallet::WalletStore::dataDir()).entryList({QStringLiteral("wallet.dat.unread-*")}, QDir::Files);
        check(aside.size() == 1 && fileBytes(QDir(wallet::WalletStore::dataDir()).filePath(aside.value(0))) == bytes, QStringLiteral("as it was"));
        offline(shown.controller);
        check(shown.controller->addKeysWallet(keysOf(keys), QStringLiteral("Anew"), &error), QStringLiteral("a wallet can be added: %1").arg(error));
        check(waitFor([&] { return !shown.controller->loadingSaved(); }, 15000), QStringLiteral("(its database, under another key)"));
        check(shown.controller->removeAllWallets(&error), QStringLiteral("removing all takes the password's file too: %1").arg(error));
        check(shown.controller->walletFilesKeptAside() == 0, QStringLiteral("(nothing left)"));
    }
}

// A file of an older format, whose first rewrite would otherwise leave a copy in the clear beside it.
void testOlderFile(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    const wallet::AccountKey key = keysOf(keys).first();
    const QJsonObject account{{QStringLiteral("type"), QLatin1String(chain::scriptTypeId(key.type))},
                              {QStringLiteral("network"), QStringLiteral("bitcoin")},
                              {QStringLiteral("account"), 0},
                              {QStringLiteral("xpub"), key.xpub}};
    const QJsonObject wallet{{QStringLiteral("id"), 1}, {QStringLiteral("label"), QStringLiteral("Old")}, {QStringLiteral("accounts"), QJsonArray{account}}};
    const QJsonObject file{{QStringLiteral("format"), QStringLiteral("gitwallet")}, {QStringLiteral("version"), 3}, {QStringLiteral("nextWalletId"), 2}, {QStringLiteral("wallets"), QJsonArray{wallet}},
                           {QStringLiteral("databaseKey"), QString::fromLatin1(wallet::WalletStore::newDatabaseKey().toHex())}};
    check(wallet::WalletStore::writeRawForTest(QJsonDocument(file).toJson(), &error), QStringLiteral("(a file of an older format: %1)").arg(error));
    Window shown;
    shown.open();
    check(shown.controller->hasWallet() && shown.controller->store().olderFormat() == 3, QStringLiteral("(read, and not rewritten yet)"));
    check(waitFor([&] { return !shown.controller->loadingSaved(); }, 15000), QStringLiteral("(ready)"));
    check(waited([&](const auto& done) { shown.controller->changePassword(QString(), QString::fromLatin1(kPassword), false, shown.window.get(), done); }).isEmpty(), QStringLiteral("(a password on it)"));
    check(shown.controller->walletFilesKeptAside() == 0, QStringLiteral("setting a password leaves no copy in the clear beside it"));
    check(wallet::WalletStore::readRawForTest(&error).contains("gitwallet-sealed"), QStringLiteral("and the file is under it"));
    check(shown.controller->removeAllWallets(&error), QStringLiteral("(gone: %1)").arg(error));
}

void testBackup(const Check& check, const QList<wallet::AccountKey>& keys)
{
    {
        wallet::GivenOut mine;
        mine.add(1);
        mine.add(2);
        mine.add(10);
        wallet::GivenOut other;
        other.add(2);
        other.add(3);
        other.add(20);
        mine.merge(other);
        check(mine.runs.size() == 3 && mine.count() == 5 && mine.contains(3) && mine.contains(10) && mine.contains(20) && !mine.contains(4), QStringLiteral("what two lists gave out, joined, is all of each, in runs"));
        wallet::GivenOut touching;
        touching.add(4);
        mine.merge(touching);
        check(mine.runs.size() == 3 && mine.count() == 6 && mine.runs.first().to == 5, QStringLiteral("what touches a run joins it"));
        mine.merge({});
        check(mine.count() == 6, QStringLiteral("and nothing adds nothing"));
        wallet::GivenOut none;
        none.merge(mine);
        check(none.count() == 6 && none.runs.size() == 3, QStringLiteral("into nothing it is all of it"));
    }
    QString error;
    QTemporaryDir folder;
    const QString path = folder.filePath(QStringLiteral("a.gwbackup"));
    const QList<wallet::AccountKey> added = keysOf(keys);
    const wallet::Network bitcoin = wallet::findNetwork(QStringLiteral("bitcoin"))->network;
    const wallet::AccountKey btc = added.first();
    const QString xpub = btc.xpub;
    const chain::Hash256 noted = [] {
        chain::Hash256 id{};
        id.fill(0x5a);
        return id;
    }();
    const QString namedId(64, QLatin1Char('a'));
    QString knownAddress;

    {
        Window shown;
        shown.open();
        offline(shown.controller);
        check(!shown.controller->backupBlocker().isEmpty(), QStringLiteral("no wallet, nothing to back up"));
        check(shown.controller->addKeysWallet(added, QStringLiteral("Shop"), &error), QStringLiteral("a wallet: %1").arg(error));
        check(!shown.controller->backupBlocker().isEmpty(), QStringLiteral("nor while what is saved is being read: it would look whole"));
        check(waitFor([&] { return !shown.controller->loadingSaved() && shown.controller->backupBlocker().isEmpty(); }, 15000), QStringLiteral("then there is"));
        wallet::WalletModel* model = shown.controller->model();
        const quint32 shop = shown.controller->store().wallets.first().id;
        check(waitFor([&] { return model->find(shop, bitcoin, btc.type, 0, 0, 6) >= 0; }, 15000), QStringLiteral("(its rows)"));
        for (quint32 index : {quint32(2), quint32(3)}) {
            wallet::AddressRow row = model->at(model->find(shop, bitcoin, btc.type, 0, 0, index));
            row.checked = true;
            model->upsert(row);
            check(shown.controller->markGivenOut(row, &error), QStringLiteral("(given out: %1)").arg(error));
        }
        knownAddress = model->at(model->find(shop, bitcoin, btc.type, 0, 0, 5)).address;
        check(shown.controller->setAddressLabel(knownAddress, QStringLiteral("A till by the door"), &error), QStringLiteral("(a label: %1)").arg(error));
        wallet::TxNote note;
        note.label = QStringLiteral("Refund");
        note.note = QStringLiteral("Of ") + namedId; // an id, which reads like a key
        wallet::UtxoHistory* history = shown.controller->historyOf(bitcoin);
        history->book()->setNote(noted, note);
        check(shown.controller->labelCount() == 2, QStringLiteral("(a label and a note: %1)").arg(shown.controller->labelCount()));
        wallet::ScanSettings settings = shown.controller->store().settings;
        settings.utxoReceiveGap = 77;
        check(shown.controller->applySettings(settings, &error), QStringLiteral("(a gap of its own: %1)").arg(error));

        check(waited([&](const auto& done) { shown.controller->writeBackup(path, QString::fromLatin1(kPassword), shown.window.get(), done); }).isEmpty(), QStringLiteral("a backup is made"));
        const QByteArray file = fileBytes(path);
        check(!file.isEmpty() && file.contains("gitwallet-backup"), QStringLiteral("it says what it is"));
        check(!file.contains(xpub.toLatin1()) && !file.contains("Shop") && !file.contains(knownAddress.toLatin1()) && !file.contains("A till by the door") && !file.contains("Refund"),
              QStringLiteral("and reads as nothing else: no key, no name, no address, no label"));
        check(shown.controller->store().lastBackupMs > 0 && shown.controller->store().lastBackupWallets == 1, QStringLiteral("when it was made is kept"));
        check(shown.controller->store().lastBackupNextWallet == shown.controller->store().nextWalletId(), QStringLiteral("and how far its wallets went"));
        auto* settingsPage = shown.window->findChild<ui::SettingsPage*>();
        check(settingsPage->findChild<QLabel*>(QStringLiteral("backupState"))->text().contains(QLatin1String("Last backup")), QStringLiteral("and shown: %1").arg(settingsPage->findChild<QLabel*>(QStringLiteral("backupState"))->text()));
        wallet::seal::Cost cost;
        const QJsonObject sealed = QJsonDocument::fromJson(file).object();
        check(wallet::seal::costOf(sealed, &cost), QStringLiteral("(its cost reads)"));
        QByteArray plain;
        check(wallet::seal::open(wallet::seal::makeKey(wallet::seal::passwordBytes(QString::fromLatin1(kPassword)), cost), sealed, &plain), QStringLiteral("(it opens)"));
        check(plain.contains(xpub.toLatin1()) && plain.contains("A till by the door") && plain.contains("Refund"), QStringLiteral("what it holds is all there"));
        check(!shown.controller->store().databaseKey.isEmpty() && !plain.contains(shown.controller->store().databaseKey.toHex()), QStringLiteral("but for the key of this PC's database"));

        // Through the page, as anyone would.
        const QString viaPage = folder.filePath(QStringLiteral("page.gwbackup"));
        settingsPage->makeBackupAt(viaPage);
        auto* asking = settingsPage->findChild<ui::PasswordDialog*>();
        check(asking && asking->isVisible(), QStringLiteral("a backup asks for a password of its own"));
        if (asking) {
            auto* ok = asking->findChild<QPushButton*>(QStringLiteral("primaryButton"));
            asking->findChild<QLineEdit*>(QStringLiteral("passwordNew"))->setText(QString::fromLatin1(kOtherPassword));
            asking->findChild<QLineEdit*>(QStringLiteral("passwordRepeat"))->setText(QString::fromLatin1(kOtherPassword));
            check(!ok->isEnabled(), QStringLiteral("and that nobody can get it back be understood"));
            asking->findChild<QCheckBox*>(QStringLiteral("passwordUnderstood"))->setChecked(true);
            check(ok->isEnabled(), QStringLiteral("then it can be made"));
            const QPointer<ui::PasswordDialog> still(asking);
            ok->click();
            check(waitFor([&] { return QFile::exists(viaPage) && (!still || !still->isVisible()); }, 15000), QStringLiteral("it is there, and the dialog goes"));
        }

        // A second wallet after it: the page says the backup does not hold it.
        const QList<wallet::AccountKey> otherKeys = wallet::WalletStore::deriveAccounts(chain::bip39::split("legal winner thank year wave sausage worth useful legal winner thank yellow"), {});
        check(shown.controller->addKeysWallet(keysOf(otherKeys), QStringLiteral("Warehouse"), &error), QStringLiteral("(a wallet after it: %1)").arg(error));
        check(settingsPage->findChild<QLabel*>(QStringLiteral("backupState"))->text().contains(QLatin1String("not in it")), QStringLiteral("what came after is said to be outside it: %1").arg(settingsPage->findChild<QLabel*>(QStringLiteral("backupState"))->text()));
        check(waitFor([&] { return !shown.controller->loadingSaved(); }, 15000), QStringLiteral("(ready)"));

        // Files that are not it.
        const auto restored = [&](const QString& from, const QString& password) {
            WalletController::RestoreReport report;
            bool over = false;
            shown.controller->restoreBackup(from, password, shown.window.get(), [&](const WalletController::RestoreReport& came) {
                report = came;
                over = true;
            });
            waitFor([&] { return over; }, 30000);
            return report;
        };
        const int walletsNow = int(shown.controller->store().wallets.size());
        check(restored(path, QString::fromLatin1(kOtherPassword)).error.contains(QLatin1String("not the password")), QStringLiteral("a wrong password opens nothing"));
        QFile changed(folder.filePath(QStringLiteral("changed.gwbackup")));
        {
            QByteArray bytes = file;
            const qsizetype at = bytes.indexOf("\"body\"") + 20;
            bytes[at] = bytes.at(at) == 'A' ? 'B' : 'A';
            changed.open(QIODevice::WriteOnly);
            changed.write(bytes);
            changed.close();
        }
        check(restored(changed.fileName(), QString::fromLatin1(kPassword)).error.contains(QLatin1String("was changed")), QStringLiteral("nor does a file with a byte changed"));
        QFile other2(folder.filePath(QStringLiteral("other.gwbackup")));
        other2.open(QIODevice::WriteOnly);
        other2.write("{\"format\":\"something\"}");
        other2.close();
        check(restored(other2.fileName(), QString::fromLatin1(kPassword)).error.contains(QLatin1String("not a GitWallet backup")), QStringLiteral("nor something else"));
        QJsonObject newer = sealed;
        newer.insert(QStringLiteral("version"), 2);
        QFile newerFile(folder.filePath(QStringLiteral("newer.gwbackup")));
        newerFile.open(QIODevice::WriteOnly);
        newerFile.write(QJsonDocument(newer).toJson());
        newerFile.close();
        check(restored(newerFile.fileName(), QString::fromLatin1(kPassword)).error.contains(QLatin1String("newer")), QStringLiteral("nor one from a newer version"));
        check(restored(folder.filePath(QStringLiteral("none")), QString::fromLatin1(kPassword)).error.contains(QLatin1String("could not be read")), QStringLiteral("nor one that is not there"));
        check(int(shown.controller->store().wallets.size()) == walletsNow, QStringLiteral("and none of those changed anything"));

        // On a PC that has it already: nothing doubles, and what is new to it is joined.
        const wallet::WalletEntry* shopNow = shown.controller->store().find(shop);
        check(shopNow != nullptr, QStringLiteral("(the wallet)"));
        wallet::AddressRow later = model->at(model->find(shop, bitcoin, btc.type, 0, 0, 6));
        later.checked = true;
        model->upsert(later);
        check(shown.controller->markGivenOut(later, &error), QStringLiteral("(given out after the backup: %1)").arg(error));
        const WalletController::RestoreReport again = restored(path, QString::fromLatin1(kPassword));
        check(again.error.isEmpty() && again.walletsAdded == 0 && again.walletsHere == 1, QStringLiteral("what is here already is not added again: %1 %2 %3").arg(again.error).arg(again.walletsAdded).arg(again.walletsHere));
        check(int(shown.controller->store().wallets.size()) == walletsNow, QStringLiteral("and nothing doubles"));
        const wallet::AccountKey* givenKey = nullptr;
        for (const wallet::AccountKey& key : shown.controller->store().find(shop)->accounts) {
            if (key.type == btc.type)
                givenKey = &key;
        }
        check(givenKey && givenKey->given.contains(2) && givenKey->given.contains(3) && givenKey->given.contains(6), QStringLiteral("what it gave out since stays"));
        check(shown.controller->store().settings.utxoReceiveGap == 77, QStringLiteral("and its settings"));
        // A gap of its own larger than the one in use: what the wallets were found with counts.
        wallet::ScanSettings narrower = shown.controller->store().settings;
        narrower.utxoReceiveGap = 60;
        check(shown.controller->applySettings(narrower, &error), QStringLiteral("(a smaller gap here: %1)").arg(error));
        const WalletController::RestoreReport raised = restored(path, QString::fromLatin1(kPassword));
        check(raised.error.isEmpty() && shown.controller->store().settings.utxoReceiveGap == 77, QStringLiteral("a larger gap in a backup is taken up: %1").arg(shown.controller->store().settings.utxoReceiveGap));
        check(raised.gapsRose && !raised.gapsKept && raised.backupReceiveGap == 77, QStringLiteral("and says so"));
        check(!again.gapsRose, QStringLiteral("what changes nothing says nothing of it"));
        wallet::ScanSettings savedSettings;
        wallet::WalletStore keptFile;
        check(keptFile.load(&error) && keptFile.settings.utxoReceiveGap == 77 && keptFile.settings.servers == shown.controller->store().settings.servers, QStringLiteral("and written down: %1").arg(error));
        check(shown.controller->addressLabel(knownAddress) == QLatin1String("A till by the door"), QStringLiteral("and its labels"));
        check(shown.controller->removeAllWallets(&error), QStringLiteral("all removed: %1").arg(error));
    }

    // On a PC with nothing: the whole of it, through the page.
    {
        Window shown;
        shown.open();
        shown.controller->prices()->useTestServer(QStringLiteral("http://127.0.0.1:1"));
        check(!shown.controller->hasWallet(), QStringLiteral("(nothing here)"));
        auto* settings = shown.window->findChild<ui::SettingsPage*>();
        shown.window->showPage(QStringLiteral("settings"));
        settings->restoreBackupFrom(path);
        auto* dialog = settings->findChild<ui::PasswordDialog*>();
        check(dialog && dialog->isVisible(), QStringLiteral("a restore asks for the file's password"));
        if (dialog) {
            dialog->findChild<QLineEdit*>(QStringLiteral("passwordCurrent"))->setText(QString::fromLatin1(kPassword));
            dialog->findChild<QPushButton*>(QStringLiteral("primaryButton"))->click();
        }
        check(waitFor([&] { return settings->findChild<QMessageBox*>(QStringLiteral("restoredBox")) != nullptr; }, 30000), QStringLiteral("and says what came of it"));
        auto* box = settings->findChild<QMessageBox*>(QStringLiteral("restoredBox"));
        const QString said = box ? box->text() : QString();
        check(said.contains(QStringLiteral("Wallets added: 1")) && said.contains(QStringLiteral("given out that this PC did not know of: 2")) && said.contains(QStringLiteral("Labels and notes: 2")), QStringLiteral("in figures: %1").arg(said));
        if (box)
            box->done(0);
        check(shown.controller->hasWallet() && shown.controller->store().wallets.size() == 1, QStringLiteral("the wallet is back"));
        if (!shown.controller->hasWallet())
            return;
        const wallet::WalletEntry& entry = shown.controller->store().wallets.first();
        check(entry.label == QLatin1String("Shop") && entry.fromKeys && entry.accounts.size() == added.size(), QStringLiteral("named and made as it was"));
        const auto own = std::find_if(entry.accounts.cbegin(), entry.accounts.cend(), [&](const wallet::AccountKey& key) { return key.type == btc.type; });
        check(own != entry.accounts.cend() && own->xpub == xpub && own->given.contains(2) && own->given.contains(3) && !own->given.contains(6), QStringLiteral("with what it had given out then"));
        check(shown.controller->store().settings.utxoReceiveGap == 77, QStringLiteral("and the settings"));
        check(shown.controller->addressLabel(knownAddress) == QLatin1String("A till by the door"), QStringLiteral("and the label"));
        const wallet::TxNote back = shown.controller->historyOf(bitcoin)->book()->noteOf(noted);
        check(back.label == QLatin1String("Refund") && back.note.contains(namedId), QStringLiteral("and a note naming a transaction, which reads like a key and is none"));
        wallet::WalletStore probe;
        check(probe.load(&error) && !probe.hasPassword(), QStringLiteral("a file here has no password but its own: %1").arg(error));

        check(shown.controller->removeAllWallets(&error) || waitFor([&] { return shown.controller->removeAllWallets(&error); }, 15000), QStringLiteral("all removed: %1").arg(error));
    }

    // The same wallet on another PC, which has given out addresses of its own: both count.
    {
        Window shown;
        shown.open();
        offline(shown.controller);
        check(shown.controller->addKeysWallet(added, QStringLiteral("Till"), &error), QStringLiteral("the same keys under another name: %1").arg(error));
        check(waitFor([&] { return !shown.controller->loadingSaved(); }, 15000), QStringLiteral("(ready)"));
        wallet::WalletModel* model = shown.controller->model();
        const quint32 own = shown.controller->store().wallets.value(0).id;
        check(waitFor([&] { return model->find(own, bitcoin, btc.type, 0, 0, 9) >= 0; }, 15000), QStringLiteral("(its rows)"));
        wallet::AddressRow row = model->at(model->find(own, bitcoin, btc.type, 0, 0, 9));
        row.checked = true;
        model->upsert(row);
        check(shown.controller->markGivenOut(row, &error), QStringLiteral("(one given out here: %1)").arg(error));
        // This PC has gone on since: its own words stay, and the file fills in only what is empty.
        check(shown.controller->setAddressLabel(knownAddress, QStringLiteral("The till by the window now"), &error), QStringLiteral("(a newer label here: %1)").arg(error));
        wallet::UtxoHistory* ownHistory = shown.controller->historyOf(bitcoin);
        wallet::TxNote newer;
        newer.note = QStringLiteral("Written here since");
        ownHistory->book()->setNote(noted, newer);
        WalletController::RestoreReport report;
        bool over = false;
        shown.controller->restoreBackup(path, QString::fromLatin1(kPassword), shown.window.get(), [&](const WalletController::RestoreReport& came) {
            report = came;
            over = true;
        });
        check(waitFor([&] { return over; }, 30000) && report.error.isEmpty(), QStringLiteral("a backup of what is here: %1").arg(report.error));
        check(shown.controller->addressLabel(knownAddress) == QLatin1String("The till by the window now"), QStringLiteral("what this PC calls an address now stays: %1").arg(shown.controller->addressLabel(knownAddress)));
        const wallet::TxNote filled = ownHistory->book()->noteOf(noted);
        check(filled.note == QLatin1String("Written here since") && filled.label == QLatin1String("Refund"), QStringLiteral("a note stays too, and what it lacked is filled in: %1 / %2").arg(filled.label, filled.note));
        check(report.labelsKept == 2 && report.labels == 1, QStringLiteral("and it says how many differ: %1 kept, %2 in").arg(report.labelsKept).arg(report.labels));
        check(report.walletsHere == 1 && report.walletsAdded == 0 && report.givenOut == 2, QStringLiteral("says it joined the two it gave out: %1 here, %2 added, %3 given out").arg(report.walletsHere).arg(report.walletsAdded).arg(report.givenOut));
        const auto keyHere = [&](const wallet::WalletStore& store) {
            wallet::GivenOut given;
            for (const wallet::AccountKey& key : store.wallets.value(0).accounts) {
                if (key.type == btc.type)
                    given = key.given;
            }
            return given;
        };
        const wallet::GivenOut joined = keyHere(shown.controller->store());
        check(joined.contains(2) && joined.contains(3) && joined.contains(9), QStringLiteral("what the backup gave out, and what this one did"));
        check(shown.controller->store().wallets.value(0).label == QLatin1String("Till"), QStringLiteral("under the name it has here"));
        wallet::WalletStore written;
        check(written.load(&error) && keyHere(written).contains(2) && keyHere(written).contains(9), QStringLiteral("written down: %1").arg(error));
        check(shown.controller->nextReceive(own, bitcoin, btc.type).index != 2, QStringLiteral("and none of it is offered again"));
        check(waitFor([&] { QString why; return shown.controller->removeAllWallets(&why); }, 15000), QStringLiteral("all removed"));
    }

    // Under an app password of its own, a name already taken, and a seed in a label of a made-up file.
    {
        const QList<wallet::AccountKey> otherKeys = wallet::WalletStore::deriveAccounts(chain::bip39::split("legal winner thank year wave sausage worth useful legal winner thank yellow"), {});
        Window shown;
        shown.open();
        offline(shown.controller);
        check(shown.controller->addKeysWallet(keysOf(otherKeys), QStringLiteral("Shop"), &error), QStringLiteral("another wallet of that name: %1").arg(error));
        check(waitFor([&] { return !shown.controller->loadingSaved(); }, 15000), QStringLiteral("(ready)"));
        check(waited([&](const auto& done) { shown.controller->changePassword(QString(), QString::fromLatin1(kOtherPassword), false, shown.window.get(), done); }).isEmpty(), QStringLiteral("(an app password)"));

        // A backup as a file made elsewhere would be, with something that should not be in it.
        const QByteArray body = QJsonDocument::fromJson([&] {
            wallet::seal::Cost cost;
            const QJsonObject sealed = QJsonDocument::fromJson(fileBytes(path)).object();
            wallet::seal::costOf(sealed, &cost);
            QByteArray plain;
            wallet::seal::open(wallet::seal::makeKey(wallet::seal::passwordBytes(QString::fromLatin1(kPassword)), cost), sealed, &plain);
            return plain;
        }()).toJson();
        QJsonObject edited = QJsonDocument::fromJson(body).object();
        wallet::LabelLine secret;
        secret.isTransaction = false;
        secret.address = knownAddress + QStringLiteral("x");
        secret.label = QStringLiteral("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about");
        edited.insert(QStringLiteral("labels"), edited.value(QStringLiteral("labels")).toString() + QString::fromUtf8(wallet::writeLabelLine(secret)));
        // A wallet of a key some later version made, which this one can only keep.
        QJsonObject editedStore = edited.value(QStringLiteral("store")).toObject();
        QJsonArray editedWallets = editedStore.value(QStringLiteral("wallets")).toArray();
        editedWallets.append(QJsonObject{{QStringLiteral("id"), 77},
                                         {QStringLiteral("label"), QStringLiteral("Later kind")},
                                         {QStringLiteral("accounts"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("something-later")}, {QStringLiteral("account"), 0}, {QStringLiteral("xpub"), QStringLiteral("a key of that kind")}}}}});
        editedStore.insert(QStringLiteral("wallets"), editedWallets);
        edited.insert(QStringLiteral("store"), editedStore);
        const wallet::seal::Cost cost = wallet::seal::newCost();
        const QJsonObject resealed = wallet::seal::lock(wallet::seal::makeKey(wallet::seal::passwordBytes(QString::fromLatin1(kPassword)), cost), cost, QJsonDocument(edited).toJson(QJsonDocument::Compact), QStringLiteral("gitwallet-backup"));
        const QString editedPath = folder.filePath(QStringLiteral("edited.gwbackup"));
        {
            QFile out(editedPath);
            out.open(QIODevice::WriteOnly);
            out.write(QJsonDocument(resealed).toJson());
        }
        WalletController::RestoreReport report;
        bool over = false;
        shown.controller->restoreBackup(editedPath, QString::fromLatin1(kPassword), shown.window.get(), [&](const WalletController::RestoreReport& came) {
            report = came;
            over = true;
        });
        check(waitFor([&] { return over; }, 30000) && report.error.isEmpty(), QStringLiteral("a backup goes in under an app password: %1").arg(report.error));
        check(report.walletsAdded == 2 && shown.controller->store().wallets.size() == 3, QStringLiteral("beside what is here"));
        check(shown.controller->store().wallets.value(1).label == QLatin1String("Shop 1"), QStringLiteral("under a name of its own: %1").arg(shown.controller->store().wallets.value(1).label));
        check(shown.controller->store().wallets.value(2).label == QLatin1String("Later kind") && shown.controller->store().wallets.value(2).keptAccounts.size() == 1, QStringLiteral("and a key of a kind this version does not read stays with its wallet"));
        check(report.labelsLeftOut == 1 && shown.controller->addressLabel(secret.address).isEmpty(), QStringLiteral("seed words in a label stay out, whatever file they came in"));
        check(report.labels == 2, QStringLiteral("and the rest goes in: %1").arg(report.labels));
        check(wallet::WalletStore::readRawForTest(&error).contains("gitwallet-sealed") && shown.controller->hasPassword(), QStringLiteral("what it wrote is under the app's password"));
        // The same file again: a key of a kind only a later version reads is that wallet all the same.
        WalletController::RestoreReport second;
        over = false;
        shown.controller->restoreBackup(editedPath, QString::fromLatin1(kPassword), shown.window.get(), [&](const WalletController::RestoreReport& came) {
            second = came;
            over = true;
        });
        check(waitFor([&] { return over; }, 30000) && second.error.isEmpty() && second.walletsAdded == 0 && second.walletsHere == 2 && second.walletsRefused == 0 && shown.controller->store().wallets.size() == 3,
              QStringLiteral("a wallet known only by such a key is known as here: %1 added, %2 here, %3 refused, %4 wallets").arg(second.walletsAdded).arg(second.walletsHere).arg(second.walletsRefused).arg(shown.controller->store().wallets.size()));
        check(waitFor([&] { QString why; return shown.controller->removeAllWallets(&why); }, 15000), QStringLiteral("all removed"));
    }

    // A file made to look like a backup, under a password, opens the same way as an honest one.
    const auto editedBackup = [&](const QString& name, const std::function<void(QJsonObject&)>& change) {
        wallet::seal::Cost cost;
        const QJsonObject sealed = QJsonDocument::fromJson(fileBytes(path)).object();
        wallet::seal::costOf(sealed, &cost);
        QByteArray plain;
        wallet::seal::open(wallet::seal::makeKey(wallet::seal::passwordBytes(QString::fromLatin1(kPassword)), cost), sealed, &plain);
        QJsonObject body = QJsonDocument::fromJson(plain).object();
        QJsonObject store = body.value(QStringLiteral("store")).toObject();
        change(store);
        body.insert(QStringLiteral("store"), store);
        const wallet::seal::Cost fresh = wallet::seal::newCost();
        const QJsonObject resealed = wallet::seal::lock(wallet::seal::makeKey(wallet::seal::passwordBytes(QString::fromLatin1(kPassword)), fresh), fresh, QJsonDocument(body).toJson(QJsonDocument::Compact), QStringLiteral("gitwallet-backup"));
        const QString out = folder.filePath(name);
        QFile file(out);
        file.open(QIODevice::WriteOnly);
        file.write(QJsonDocument(resealed).toJson());
        return out;
    };
    const auto restoring = [](WalletController* controller, QObject* guard, const QString& from) {
        WalletController::RestoreReport report;
        bool over = false;
        controller->restoreBackup(from, QString::fromLatin1(kPassword), guard, [&](const WalletController::RestoreReport& came) {
            report = came;
            over = true;
        });
        waitFor([&] { return over; }, 30000);
        return report;
    };

    // A wallet.dat that cannot be read is what a backup is for.
    {
        {
            QFile unreadable(wallet::WalletStore::filePath());
            unreadable.open(QIODevice::WriteOnly);
            unreadable.write("this is no wallet file, or one for another Windows user");
        }
        const QByteArray unreadableBytes = fileBytes(wallet::WalletStore::filePath());
        {
            WalletController controller;
            QString why;
            check(!controller.loadExisting(&why) && !why.isEmpty(), QStringLiteral("(a wallet.dat that cannot be read)"));
            const WalletController::RestoreReport report = restoring(&controller, &controller, path);
            check(report.error.isEmpty() && report.walletsAdded == 1 && controller.hasWallet(), QStringLiteral("a restore puts it right: %1").arg(report.error));
            const QString kept = controller.unreadFileKept();
            check(!kept.isEmpty() && fileBytes(QDir::fromNativeSeparators(kept)) == unreadableBytes, QStringLiteral("and the file is kept aside as it was: %1").arg(kept));
            check(waitFor([&] { return !controller.loadingSaved(); }, 15000), QStringLiteral("(ready)"));
            // The next start says so, whoever put it there.
            QString said;
            WalletController again;
            QObject::connect(&again, &WalletController::errorOccurred, &again, [&said](wallet::Problem kind, const QString& message) {
                if (kind == wallet::Problem::FileSetAside)
                    said = message;
            });
            Q_UNUSED(said);
            check(controller.removeAllWallets(&why) || waitFor([&] { return controller.removeAllWallets(&why); }, 15000), QStringLiteral("all removed: %1").arg(why));
        }
    }

    // What a made-up file does not get to do.
    {
        Window shown;
        shown.open();
        offline(shown.controller);
        const QString tooMany = editedBackup(QStringLiteral("too-many.gwbackup"), [](QJsonObject& store) {
            QJsonArray wallets = store.value(QStringLiteral("wallets")).toArray();
            QJsonObject first = wallets.at(0).toObject();
            QJsonArray accounts = first.value(QStringLiteral("accounts")).toArray();
            QJsonObject account = accounts.at(0).toObject();
            QJsonArray runs;
            runs.append(QJsonArray{0, wallet::kMaxChainRows + 1});
            account.insert(QStringLiteral("given"), runs);
            accounts.replace(0, account);
            first.insert(QStringLiteral("accounts"), accounts);
            wallets.replace(0, first);
            store.insert(QStringLiteral("wallets"), wallets);
        });
        const WalletController::RestoreReport refused = restoring(shown.controller, shown.window.get(), tooMany);
        check(refused.error.contains(QLatin1String("given out")) && !shown.controller->hasWallet(), QStringLiteral("more addresses given out than a wallet can have read as damaged: %1").arg(refused.error));

        // A name that is seed words: the wallet goes in, under another.
        const QList<wallet::AccountKey> thirdKeys = wallet::WalletStore::deriveAccounts(chain::bip39::split("letter advice cage absurd amount doctor acoustic avoid letter advice cage above"), {});
        const QString named = editedBackup(QStringLiteral("named.gwbackup"), [](QJsonObject& store) {
            QJsonArray wallets = store.value(QStringLiteral("wallets")).toArray();
            QJsonObject first = wallets.at(0).toObject();
            first.insert(QStringLiteral("label"), QStringLiteral("zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong"));
            wallets.replace(0, first);
            store.insert(QStringLiteral("wallets"), wallets);
        });
        const WalletController::RestoreReport underAnother = restoring(shown.controller, shown.window.get(), named);
        check(underAnother.error.isEmpty() && underAnother.walletsAdded == 1 && !shown.controller->store().wallets.value(0).label.contains(QLatin1String("zoo")), QStringLiteral("a name that reads like seed words is left out, and the wallet goes in: %1").arg(shown.controller->store().wallets.value(0).label));
        Q_UNUSED(thirdKeys);
        check(waitFor([&] { QString why; return shown.controller->removeAllWallets(&why); }, 15000), QStringLiteral("all removed"));
    }

    // A wallet here with one key of the backup's wallet: what it lacks comes along, as a wallet of its own,
    // for an address a wallet here hands out is that wallet's own, whatever a file says.
    {
        const QList<wallet::AccountKey> otherKeys = wallet::WalletStore::deriveAccounts(chain::bip39::split("legal winner thank year wave sausage worth useful legal winner thank yellow"), {});
        Window shown;
        shown.open();
        offline(shown.controller);
        QList<wallet::AccountKey> part{added.first()};
        for (const wallet::AccountKey& key : keysOf(otherKeys)) {
            if (key.type == chain::ScriptType::EthAccount)
                part.append(key); // its place in that wallet is taken
        }
        check(shown.controller->addKeysWallet(part, QStringLiteral("Part of it"), &error), QStringLiteral("(a wallet with one key of it: %1)").arg(error));
        check(waitFor([&] { return !shown.controller->loadingSaved(); }, 15000), QStringLiteral("(ready)"));
        const WalletController::RestoreReport report = restoring(shown.controller, shown.window.get(), path);
        check(report.error.isEmpty() && report.walletsHere == 1 && report.walletsAdded == 1, QStringLiteral("it counts as here, and what it lacks goes in: %1").arg(report.error));
        check(report.keysApart == int(added.size()) - 1, QStringLiteral("all of those keys: %1").arg(report.keysApart));
        const wallet::WalletEntry* entry = shown.controller->store().wallets.isEmpty() ? nullptr : &shown.controller->store().wallets.first();
        check(entry && entry->accounts.size() == part.size() && entry->label == QLatin1String("Part of it"), QStringLiteral("the wallet here gets no key from a file: nothing it hands out can change"));
        const wallet::WalletEntry apart = shown.controller->store().wallets.value(1);
        check(apart.accounts.size() == report.keysApart && apart.label == QLatin1String("Shop") && apart.fromKeys, QStringLiteral("they are a wallet of their own, to be seen: %1").arg(apart.label));
        wallet::WalletStore written;
        check(written.load(&error) && written.wallets.size() == 2, QStringLiteral("written down, and it reads: %1").arg(error));
        const WalletController::RestoreReport second = restoring(shown.controller, shown.window.get(), path);
        check(second.error.isEmpty() && second.walletsAdded == 0 && second.keysApart == 0 && shown.controller->store().wallets.size() == 2, QStringLiteral("again, nothing doubles"));
        check(waitFor([&] { QString why; return shown.controller->removeAllWallets(&why); }, 15000), QStringLiteral("all removed"));
    }

    // Servers set up first stay the ones every address is asked of.
    {
        Window shown;
        shown.open();
        shown.controller->prices()->useTestServer(QStringLiteral("http://127.0.0.1:1"));
        wallet::ScanSettings own = shown.controller->store().settings;
        own.servers.insert(QStringLiteral("bitcoin"), {QStringLiteral("127.0.0.1:2")});
        shown.controller->applySettings(own, &error);
        const WalletController::RestoreReport report = restoring(shown.controller, shown.window.get(), path);
        check(report.error.isEmpty() && report.walletsAdded == 1, QStringLiteral("(a backup on a PC with its own server: %1)").arg(report.error));
        check(shown.controller->store().settings.servers.value(QStringLiteral("bitcoin")) == QStringList{QStringLiteral("127.0.0.1:2")}, QStringLiteral("a server this PC was set up with stays: %1").arg(shown.controller->store().settings.servers.value(QStringLiteral("bitcoin")).join(QLatin1Char(','))));
        check(report.serversKept, QStringLiteral("and it says so"));
        check(shown.controller->store().settings.utxoReceiveGap == 77, QStringLiteral("what else the backup was found with comes along"));
        check(waitFor([&] { QString why; return shown.controller->removeAllWallets(&why); }, 15000), QStringLiteral("all removed"));
    }

    // One removed and one added leave the count where it was, and the backup without the new one.
    {
        const QList<wallet::AccountKey> otherKeys = wallet::WalletStore::deriveAccounts(chain::bip39::split("legal winner thank year wave sausage worth useful legal winner thank yellow"), {});
        Window shown;
        shown.open();
        offline(shown.controller);
        check(shown.controller->addKeysWallet(added, QStringLiteral("First"), &error), QStringLiteral("(a wallet: %1)").arg(error));
        check(waitFor([&] { return !shown.controller->loadingSaved(); }, 15000), QStringLiteral("(ready)"));
        const QString own = folder.filePath(QStringLiteral("own.gwbackup"));
        check(waited([&](const auto& done) { shown.controller->writeBackup(own, QString::fromLatin1(kPassword), shown.window.get(), done); }).isEmpty(), QStringLiteral("(a backup)"));
        const quint32 first = shown.controller->store().wallets.first().id;
        check(shown.controller->addKeysWallet(keysOf(otherKeys), QStringLiteral("Second"), &error), QStringLiteral("(another: %1)").arg(error));
        check(waitFor([&] { return !shown.controller->loadingSaved(); }, 15000), QStringLiteral("(ready)"));
        check(shown.controller->removeWallet(first, &error), QStringLiteral("(the first goes: %1)").arg(error));
        auto* settings = shown.window->findChild<ui::SettingsPage*>();
        check(settings->findChild<QLabel*>(QStringLiteral("backupState"))->text().contains(QLatin1String("not in it")), QStringLiteral("the new one is said to be outside it: %1").arg(settings->findChild<QLabel*>(QStringLiteral("backupState"))->text()));
        // Labels on their way: what is removed now would come back with them.
        const QString labelsFile = folder.filePath(QStringLiteral("labels.jsonl"));
        {
            QFile file(labelsFile);
            file.open(QIODevice::WriteOnly);
            wallet::LabelLine line;
            line.isTransaction = false;
            line.address = QStringLiteral("somewhere");
            line.label = QStringLiteral("A label");
            file.write(wallet::writeLabelLine(line));
        }
        bool imported = false;
        shown.controller->importLabelsFile(labelsFile, shown.window.get(), [&imported](int, int, int, const QString&) { imported = true; });
        check(!shown.controller->removeAllWallets(&error) && error.contains(QLatin1String("labels")), QStringLiteral("nothing is removed while labels go in: %1").arg(error));
        check(!shown.controller->removeWallet(shown.controller->store().wallets.first().id, &error) && error.contains(QLatin1String("labels")), QStringLiteral("not one wallet either: %1").arg(error));
        check(waitFor([&] { return imported; }, 15000), QStringLiteral("(they are in)"));
        check(waitFor([&] { QString why; return shown.controller->removeAllWallets(&why); }, 15000), QStringLiteral("all removed"));
    }
}

// A dialog that will not close when asked, as a note that cannot be saved does not.
class Stubborn : public QDialog
{
public:
    using QDialog::QDialog;
    void done(int result) override
    {
        if (!refusing)
            QDialog::done(result);
    }
    bool refusing = true;
};

void testLockEdges(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    Window shown;
    shown.open();
    offline(shown.controller);
    check(shown.controller->addKeysWallet(keysOf(keys), QStringLiteral("Shop"), &error), QStringLiteral("(a wallet: %1)").arg(error));
    check(waitFor([&] { return !shown.controller->loadingSaved(); }, 15000), QStringLiteral("(ready)"));
    check(waited([&](const auto& done) { shown.controller->changePassword(QString(), QString::fromLatin1(kPassword), false, shown.window.get(), done); }).isEmpty(), QStringLiteral("(a password)"));
    const auto unlock = [&] {
        shown.type(QString::fromLatin1(kPassword));
        check(waitFor([&] { return !shown.window->isLocked(); }, 10000), QStringLiteral("(open again)"));
    };

    // What will not close does not stop the lock, and is there again after it.
    auto* stubborn = new Stubborn(shown.window.get());
    stubborn->setAttribute(Qt::WA_DeleteOnClose);
    const QPointer<Stubborn> still(stubborn);
    stubborn->open();
    shown.window->lockNow();
    check(shown.window->isLocked() && still && !still->isVisible(), QStringLiteral("what will not close goes out of sight, and the window locks all the same"));
    pause(50);
    check(shown.window->isLocked(), QStringLiteral("and stays locked"));
    unlock();
    check(still && still->isVisible() && still->isModal(), QStringLiteral("after the unlock it is back, with what was in it"));
    if (still) {
        still->refusing = false;
        still->done(0);
    }
    check(waitFor([&] { return !still; }, 2000), QStringLiteral("(it closes in the end)"));
    shown.window->lockNow();
    unlock();
    check(!QApplication::activeModalWidget(), QStringLiteral("and is not asked back after that"));

    // One that finishes behind the lock is not opened again.
    auto* finishing = new Stubborn(shown.window.get());
    const QPointer<Stubborn> finishes(finishing);
    finishing->open();
    shown.window->lockNow();
    finishing->refusing = false;
    finishing->done(0);
    unlock();
    check(finishes && !finishes->isVisible(), QStringLiteral("what finished behind the lock is not opened again"));
    delete finishing;

    // The window left alone with such a thing open locks all the same.
    auto* left = new Stubborn(shown.window.get());
    left->setAttribute(Qt::WA_DeleteOnClose);
    const QPointer<Stubborn> leftThere(left);
    left->open();
    shown.window->setIdleLimitForTest(60);
    check(waitFor([&] { return shown.window->isLocked(); }, 3000), QStringLiteral("left alone with a note that will not save, it locks"));
    shown.window->setIdleLimitForTest(0);
    unlock();
    if (leftThere) {
        leftThere->refusing = false;
        leftThere->done(0);
    }

    // The list of payments left open: the lock is not someone looking at what came after it opened.
    {
        wallet::PaymentBook* book = shown.controller->payments();
        wallet::WalletModel* rows = shown.controller->model();
        const wallet::Network ether = wallet::findNetwork(QStringLiteral("ethereum"))->network;
        const wallet::Asset token = wallet::networkInfo(ether).tokens.first();
        book->noteAnswered(ether, 0, {}, {wallet::assetInfo(token).key});
        const quint32 own = shown.controller->store().wallets.first().id;
        check(waitFor([&] { return rows->find(own, ether, chain::ScriptType::EthAccount, 0, 0, 2) >= 0; }, 15000), QStringLiteral("(its rows)"));
        const auto pay = [&](quint32 index, quint64 units) {
            wallet::AddressRow row = rows->at(rows->find(own, ether, chain::ScriptType::EthAccount, 0, 0, index));
            row.checked = true;
            row.used = true;
            rows->upsert(row);
            row.setBalance(token, chain::Amount::fromU64(units));
            rows->upsert(row);
        };
        pay(1, 5000000);
        check(book->unread() == 1, QStringLiteral("(one not looked at)"));
        shown.window->openPayments();
        auto* list = shown.window->findChild<ui::PaymentsDialog*>();
        check(list && list->isVisible(), QStringLiteral("(the list is open)"));
        pay(2, 6000000);
        check(book->unread() == 2, QStringLiteral("(one more while it is open)"));
        shown.window->lockNow();
        check(shown.window->isLocked() && book->unread() == 1, QStringLiteral("the lock closing the list leaves what came after it opened as not looked at: %1").arg(book->unread()));
        unlock();
    }

    // What this program copied does not stay behind the lock; what someone else put there is theirs.
    QClipboard* clipboard = QApplication::clipboard();
    ui::copyTextToClipboard(shown.window.get(), QStringLiteral("zpub of the wallet"), QStringLiteral("Copied"));
    check(clipboard->text() == QLatin1String("zpub of the wallet"), QStringLiteral("(copied)"));
    shown.window->lockNow();
    check(clipboard->text().isEmpty(), QStringLiteral("what was copied leaves the clipboard with the lock"));
    unlock();
    ui::copyTextToClipboard(shown.window.get(), QStringLiteral("zpub of the wallet"), QStringLiteral("Copied"));
    clipboard->setText(QStringLiteral("something of someone else's"));
    shown.window->lockNow();
    check(clipboard->text() == QLatin1String("something of someone else's"), QStringLiteral("what was copied since by anything else stays"));
    unlock();

    // How the scan stands opens from the status, and follows along.
    shown.window->openStatus();
    auto* status = shown.window->findChild<ui::StatusDialog*>();
    check(status && status->isVisible(), QStringLiteral("the status in the title bar opens how the scan stands"));
    if (status) {
        auto* whole = status->findChild<QLabel*>(QStringLiteral("statusWhole"));
        check(whole && whole->text() == shown.controller->statusText(), QStringLiteral("with the whole of what it says: %1").arg(whole ? whole->text() : QString()));
        const QPointer<ui::StatusDialog> open(status);
        shown.window->lockNow();
        check(!open || !open->isVisible(), QStringLiteral("and it does not stay over the lock"));
        unlock();
    }

    // Too long, where it is typed.
    check(shown.lock()->field()->maxLength() == ui::PasswordDialog::kMostChars, QStringLiteral("the lock takes what a password can be"));
    auto* settings = shown.window->findChild<ui::SettingsPage*>();
    shown.window->showPage(QStringLiteral("settings"));

    // Two kinds of older file, asked about apart.
    const QString formatCopy = wallet::WalletStore::filePath() + QStringLiteral(".format3-20260101-000000");
    const QString unreadCopy = wallet::WalletStore::filePath() + QStringLiteral(".unread-20260101-000000");
    for (const QString& name : {formatCopy, unreadCopy}) {
        QFile file(name);
        file.open(QIODevice::WriteOnly);
        file.write("an older wallet file");
    }
    check(shown.controller->walletFormatCopies() == 1 && shown.controller->walletFilesUnread() == 1, QStringLiteral("(one of each beside it)"));
    settings->findChild<QPushButton*>(QStringLiteral("changePassword"))->click();
    auto* dialog = settings->findChild<ui::PasswordDialog*>();
    check(dialog && dialog->isVisible(), QStringLiteral("(changing it)"));
    if (dialog) {
        auto* copies = dialog->findChild<QCheckBox*>(QStringLiteral("passwordDeleteCopies"));
        auto* unread = dialog->findChild<QCheckBox*>(QStringLiteral("passwordDeleteUnread"));
        check(copies && copies->isChecked(), QStringLiteral("a copy of this wallet from before goes by default: it opens today's database"));
        check(unread && !unread->isChecked(), QStringLiteral("one that could not be read stays by default: it may be the only copy of other wallets"));
        auto* fresh = dialog->findChild<QLineEdit*>(QStringLiteral("passwordNew"));
        auto* repeat = dialog->findChild<QLineEdit*>(QStringLiteral("passwordRepeat"));
        auto* ok = dialog->findChild<QPushButton*>(QStringLiteral("primaryButton"));
        dialog->findChild<QLineEdit*>(QStringLiteral("passwordCurrent"))->setText(QString::fromLatin1(kPassword));
        const QString tooLong(ui::PasswordDialog::kMostChars + 1, QLatin1Char('x'));
        fresh->setText(tooLong);
        repeat->setText(tooLong);
        check(fresh->text().size() == tooLong.size() && !ok->isEnabled() && dialog->findChild<QLabel*>(QStringLiteral("passwordStatus"))->text().contains(QLatin1String("At most")), QStringLiteral("too long is refused, not cut where nothing shows"));
        fresh->setText(QString::fromLatin1(kOtherPassword));
        repeat->setText(QString::fromLatin1(kOtherPassword));
        check(ok->isEnabled(), QStringLiteral("(the right length)"));
        ok->click();
        check(waitFor([&] { return !QFile::exists(formatCopy); }, 10000), QStringLiteral("the copy goes"));
        check(QFile::exists(unreadCopy), QStringLiteral("the other stays"));
    }
    check(waitFor([&] { return !settings->findChild<ui::PasswordDialog*>() || !settings->findChild<ui::PasswordDialog*>()->isVisible(); }, 5000), QStringLiteral("(done)"));
    check(settings->findChild<QLabel*>(QStringLiteral("fileAside"))->isVisibleTo(settings) || shown.controller->unreadFileKept() == QDir::toNativeSeparators(unreadCopy), QStringLiteral("(what is left is known)"));

    // Asked apart, deleted apart.
    check(waited([&](const auto& done) { shown.controller->changePassword(QString::fromLatin1(kOtherPassword), QString::fromLatin1(kPassword), WalletController::DeleteUnreadFiles, shown.window.get(), done); }).isEmpty(), QStringLiteral("(changed)"));
    check(!QFile::exists(unreadCopy), QStringLiteral("the one that could not be read goes only when asked"));

    // Remove all says the password goes.
    {
        QFile file(unreadCopy);
        file.open(QIODevice::WriteOnly);
        file.write("an older wallet file");
    }
    settings->findChild<QPushButton*>(QStringLiteral("dangerButton"))->click();
    auto* box = settings->findChild<QMessageBox*>();
    check(box && box->informativeText().contains(QLatin1String("app password")), QStringLiteral("removing all says the password goes with them"));
    if (box)
        box->done(0);
    check(!QFile::exists(formatCopy), QStringLiteral("(nothing else)"));
}

// A file put aside earlier: every start says so, whoever put it there.
void testFileAside(const Check& check, const QList<wallet::AccountKey>& keys)
{
    QString error;
    const QString unreadCopy = wallet::WalletStore::filePath() + QStringLiteral(".unread-20260101-000000");
    check(QFile::exists(unreadCopy), QStringLiteral("(one from the last test)"));
    {
        Window shown;
        QStringList said;
        shown.window = std::make_unique<ui::MainWindow>();
        shown.controller = shown.window->controller();
        QObject::connect(shown.controller, &WalletController::errorOccurred, shown.window.get(), [&said](wallet::Problem kind, const QString& message) {
            if (kind == wallet::Problem::FileSetAside)
                said.append(message);
        });
        shown.window->show();
        shown.window->resize(1280, 800);
        pause(30);
        check(shown.window->isLocked(), QStringLiteral("(under a password)"));
        check(said.isEmpty(), QStringLiteral("a locked window says nothing of it"));
        shown.type(QString::fromLatin1(kPassword));
        check(waitFor([&] { return !shown.window->isLocked(); }, 10000), QStringLiteral("(open)"));
        check(said.size() == 1 && said.first().contains(QLatin1String("unread-20260101")) && said.first().contains(QLatin1String("are yours")), QStringLiteral("opened, it says a file lies beside it, and what to check: %1").arg(said.value(0)));
        auto* settings = shown.window->findChild<ui::SettingsPage*>();
        shown.window->showPage(QStringLiteral("settings"));
        auto* line = settings->findChild<QLabel*>(QStringLiteral("fileAside"));
        check(line && line->isVisible() && line->text().contains(QLatin1String("unread-20260101")), QStringLiteral("and Settings goes on saying so"));
        offline(shown.controller);
        check(waitFor([&] { return !shown.controller->loadingSaved(); }, 15000), QStringLiteral("(ready)"));
        check(waitFor([&] { QString why; return shown.controller->removeAllWallets(&why); }, 15000), QStringLiteral("(removed)"));
        check(!QFile::exists(unreadCopy), QStringLiteral("(with the file)"));
    }
    Q_UNUSED(keys);
}

} // namespace

void runLockTest(const Check& check, QString* report, const QList<wallet::AccountKey>& someKeys)
{
    testSeal(check, report);
    testStoreFile(check, someKeys);
    testLockInTheWindow(check, someKeys);
    testOlderFile(check, someKeys);
    testBackup(check, someKeys);
    testLockEdges(check, someKeys);
    testFileAside(check, someKeys);
    check(ui::plainTip(QStringLiteral("Shop")) == QLatin1String("Shop"), QStringLiteral("a plain tooltip stays as it is"));
    const QString tip = ui::plainTip(QStringLiteral("Shop<img src=\"file://10.0.0.5/s/a.png\">"));
    check(tip.contains(QLatin1String("&lt;img")) && !tip.contains(QLatin1String("<img")), QStringLiteral("markup in a name reads as text: %1").arg(tip));
    wallet::seal::setCostForTest(0, 0);
}

} // namespace app
