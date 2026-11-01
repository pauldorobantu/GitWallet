#include "app/Walkthrough.h"

#include "app/WalletController.h"
#include "chain/Bip39.h"
#include "ui/MainWindow.h"
#include "ui/dialogs/ImportSeedDialog.h"
#include "ui/pages/AddressesPage.h"
#include "ui/pages/SettingsPage.h"
#include "ui/pages/WalletsPage.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTableView>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace app {

namespace {

// Public BIP39 test vectors; nothing else is typed.
const QString kPublicTestMnemonic = QStringLiteral(
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about");
const QString kSecondTestMnemonic = QStringLiteral(
    "legal winner thank year wave sausage worth useful legal winner thank yellow");
const char* const kThirdTestMnemonic =
    "letter advice cage absurd amount doctor acoustic avoid letter advice cage above";

const QString kFirstName = QStringLiteral("Online shop");
const QString kSecondName = QStringLiteral("Warehouse");
const QString kSecondNewName = QStringLiteral("Warehouse EU");
const QString kKeysName = QStringLiteral("Partner keys");

QString totalsText(const WalletController* c)
{
    QStringList parts;
    for (const wallet::AssetInfo& a : wallet::assets())
        parts << a.symbol + QLatin1Char('=')
                     + QString::fromStdString(c->model()->total(a.asset).format(a.decimals, a.decimals));
    return parts.join(QStringLiteral(", "));
}

// The used addresses a fresh scan finds at these gaps. Walking each chain up from
// index 0, it stops where more unused addresses than the gap follow the last used
// one: rows a larger gap found earlier stay in the model but not in the next scan.
int usedWithinGaps(const WalletController* c)
{
    const wallet::WalletModel* m = c->model();
    const wallet::ScanSettings& s = c->store().settings;
    QHash<QString, QList<quint32>> chains;
    QHash<QString, int> gaps;
    for (int i = 0; i < m->size(); ++i) {
        const wallet::AddressRow& r = m->at(i);
        if (!r.used)
            continue;
        const QString key = QStringLiteral("%1/%2/%3/%4").arg(r.wallet).arg(int(r.type)).arg(r.account).arg(r.change);
        chains[key].append(r.index);
        gaps[key] = !r.isBitcoin() ? s.ethGap : r.change == 0 ? s.btcReceiveGap : s.btcChangeGap;
    }
    int found = 0;
    for (auto it = chains.begin(); it != chains.end(); ++it) {
        std::sort(it->begin(), it->end());
        qint64 last = -1;
        for (const quint32 index : std::as_const(*it)) {
            if (qint64(index) > last + gaps.value(it.key()))
                break;
            ++found;
            last = index;
        }
    }
    return found;
}

template <typename T>
T* visibleChild(QWidget* root)
{
    if (!root) return nullptr;
    for (T* w : root->findChildren<T*>())
        if (w->isVisible()) return w;
    return nullptr;
}

} // namespace

Walkthrough::Walkthrough(WindowFactory makeWindow, const QString& outDir, QObject* parent)
    : QObject(parent), makeWindow_(std::move(makeWindow)), outDir_(outDir)
{
    QDir().mkpath(outDir_);
    poll_ = new QTimer(this);
    poll_->setInterval(100);
}

void Walkthrough::start()
{
    window_ = makeWindow_();
    connect(window_->controller(), &WalletController::scanFinished, this, [this] { scanFinished_ = true; });
    buildSteps();
    // Let the first paint and the (absent) wallet load happen first.
    QTimer::singleShot(1500, this, [this] { runStep(0); });
}

void Walkthrough::expect(bool ok, const QString& what)
{
    currentChecks_.append(QJsonObject{{QStringLiteral("ok"), ok}, {QStringLiteral("check"), what}});
    if (!ok) ++failures_;
}

QWidget* Walkthrough::findButton(QWidget* root, const QString& text) const
{
    if (!root) return nullptr;
    for (QAbstractButton* b : root->findChildren<QAbstractButton*>())
        if (b->isVisible() && b->text() == text) return b;
    return nullptr;
}

QString Walkthrough::visibleText(QWidget* root) const
{
    QStringList texts;
    if (!root) return {};
    for (QLabel* l : root->findChildren<QLabel*>())
        if (l->isVisible()) texts << l->text();
    for (QAbstractButton* b : root->findChildren<QAbstractButton*>())
        if (b->isVisible()) texts << b->text();
    for (QLineEdit* e : root->findChildren<QLineEdit*>())
        if (e->isVisible()) texts << e->text();
    return texts.join(QLatin1Char('\n'));
}

void Walkthrough::buildSteps()
{
    auto controller = [this] { return window_->controller(); };
    auto importDialog = [this]() -> QWidget* {
        auto* d = window_ ? window_->findChild<ui::ImportSeedDialog*>() : nullptr;
        return (d && d->isVisible()) ? d : nullptr;
    };
    auto typeWords = [importDialog](const QString& text) {
        if (auto* edit = importDialog() ? importDialog()->findChild<QPlainTextEdit*>(QStringLiteral("seedWords")) : nullptr)
            edit->setPlainText(text);
    };
    auto typeName = [importDialog](const QString& text) {
        if (auto* edit = importDialog() ? importDialog()->findChild<QLineEdit*>(QStringLiteral("walletName")) : nullptr)
            edit->setText(text);
    };
    auto typeKeys = [importDialog](const QString& text) {
        if (auto* source = importDialog() ? importDialog()->findChild<QComboBox*>(QStringLiteral("walletSource")) : nullptr)
            source->setCurrentIndex(1);
        if (auto* edit = importDialog() ? importDialog()->findChild<QPlainTextEdit*>(QStringLiteral("publicKeys")) : nullptr)
            edit->setPlainText(text);
    };
    auto importEnabled = [this, importDialog] {
        QWidget* b = findButton(importDialog(), QStringLiteral("Add wallet"));
        return b && b->isEnabled();
    };
    // Asynchronous click: the dialog runs its own event loop (exec), inside
    // which the rest of the walkthrough keeps going.
    auto clickLater = [](QWidget* button) {
        if (button)
            QTimer::singleShot(0, static_cast<QAbstractButton*>(button), &QAbstractButton::click);
    };
    auto walletsPage = [this]() -> QWidget* { return window_->findChild<ui::WalletsPage*>(); };
    auto walletsTable = [walletsPage]() -> QTableView* {
        return walletsPage() ? walletsPage()->findChild<QTableView*>() : nullptr;
    };
    auto selectWallet = [walletsTable](const QString& name) {
        QTableView* t = walletsTable();
        for (int row = 0; t && row < t->model()->rowCount(); ++row) {
            if (t->model()->index(row, 0).data().toString() == name)
                t->selectRow(row);
        }
    };
    auto openAddDialog = [this, walletsPage, clickLater] {
        window_->showPage(QStringLiteral("wallets"));
        clickLater(findButton(walletsPage(), QStringLiteral("Add wallet")));
    };
    auto namedDialog = [this](const char* name) -> QWidget* {
        auto* d = window_ ? window_->findChild<QDialog*>(QLatin1String(name)) : nullptr;
        return (d && d->isVisible()) ? d : nullptr;
    };
    auto storeHas = [controller](const QString& name) {
        const QList<wallet::WalletEntry>& list = controller()->store().wallets;
        return std::any_of(list.cbegin(), list.cend(), [&name](const wallet::WalletEntry& w) { return w.label == name; });
    };
    auto table = [this]() -> QTableView* {
        auto* page = window_->findChild<ui::AddressesPage*>();
        return page ? page->findChild<QTableView*>() : nullptr;
    };
    auto tableRows = [table] { return table() && table()->model() ? table()->model()->rowCount() : -1; };
    auto settingsPage = [this]() -> QWidget* { return window_->findChild<ui::SettingsPage*>(); };
    auto confirmBox = [this]() -> QMessageBox* {
        for (QMessageBox* box : window_->findChildren<QMessageBox*>())
            if (box->isVisible()) return box;
        return nullptr;
    };

    const QString twelveAbandon = QStringLiteral("abandon ").repeated(12).trimmed();

    steps_ = {
        {QStringLiteral("empty-state"), nullptr, nullptr, 5000,
         [this, controller] {
             expect(!controller()->hasWallet(), QStringLiteral("first start: no wallet"));
             expect(findButton(window_, QStringLiteral("Add wallet")) != nullptr,
                    QStringLiteral("the Overview offers \"Add wallet\""));
         },
         nullptr},
        {QStringLiteral("import-dialog"),
         [this, clickLater] { clickLater(findButton(window_, QStringLiteral("Add wallet"))); },
         [importDialog] { return importDialog() != nullptr; }, 5000,
         [this, importEnabled] {
             expect(true, QStringLiteral("the add-wallet dialog opened"));
             expect(!importEnabled(), QStringLiteral("Add wallet is disabled while the field is empty"));
         },
         importDialog},
        {QStringLiteral("too-few-words"), [typeWords] { typeWords(QStringLiteral("abandon abandon abandon")); },
         nullptr, 3000,
         [this, importEnabled, importDialog] {
             expect(!importEnabled(), QStringLiteral("3 words: Add wallet stays disabled"));
             expect(visibleText(importDialog()).contains(QStringLiteral("Needs 12")),
                    QStringLiteral("3 words: the status asks for 12, 15, 18, 21 or 24 words"));
         },
         importDialog},
        {QStringLiteral("unknown-word"),
         [typeWords] { typeWords(QStringLiteral("abandon ").repeated(11) + QStringLiteral("bitcoinx")); },
         nullptr, 3000,
         [this, importEnabled, importDialog] {
             expect(!importEnabled(), QStringLiteral("an unknown word: Add wallet stays disabled"));
             expect(visibleText(importDialog()).contains(QStringLiteral("not in the BIP39 list")),
                    QStringLiteral("an unknown word: the status names its position"));
         },
         importDialog},
        {QStringLiteral("bad-checksum"), [typeWords, twelveAbandon] { typeWords(twelveAbandon); }, nullptr, 3000,
         [this, importEnabled, importDialog] {
             expect(!importEnabled(), QStringLiteral("valid words, wrong checksum: Add wallet stays disabled"));
             expect(visibleText(importDialog()).contains(QStringLiteral("hecksum"), Qt::CaseInsensitive),
                    QStringLiteral("wrong checksum: the status says so"));
         },
         importDialog},
        {QStringLiteral("valid-seed"), [typeWords] { typeWords(kPublicTestMnemonic); }, nullptr, 3000,
         [this, importEnabled, importDialog] {
             expect(importEnabled(), QStringLiteral("a valid 12-word phrase enables Add wallet"));
             expect(visibleText(importDialog()).contains(QStringLiteral("checksum OK")),
                    QStringLiteral("valid phrase: the status says checksum OK"));
         },
         importDialog},
        {QStringLiteral("imported"),
         [this, importDialog, typeName, clickLater] {
             typeName(kFirstName);
             clickLater(findButton(importDialog(), QStringLiteral("Add wallet")));
         },
         [controller, importDialog] { return controller()->hasWallet() && !importDialog(); }, 20000,
         [this, controller, storeHas] {
             expect(controller()->hasWallet(), QStringLiteral("the wallet exists after Add wallet"));
             expect(storeHas(kFirstName), QStringLiteral("it goes by the name typed"));
             expect(QFile::exists(wallet::WalletStore::filePath()),
                    QStringLiteral("wallet.dat was written (DPAPI-encrypted xpubs, no seed)"));
             expect(controller()->store().allAccounts().size() == 4,
                    QStringLiteral("4 accounts: native / nested SegWit, legacy, Ethereum"));
             QFile f(wallet::WalletStore::filePath());
             const QByteArray raw = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
             expect(!raw.contains("abandon") && !raw.contains("zpub"),
                    QStringLiteral("wallet.dat holds neither the words nor readable xpubs"));
         },
         nullptr},
        // scanFinished() says both initial scans are through; the status is
        // final once nothing else holds a balance back (the connection, a
        // balance fetched again, an Ethereum pass that failed).
        {QStringLiteral("scan-finished"), nullptr,
         [this, controller] { return scanFinished_ && controller()->balancesComplete(); }, 300000,
         [this, controller] {
             const wallet::WalletModel* m = controller()->model();
             expect(m->size() > 0, QStringLiteral("addresses were derived: %1").arg(m->size()));
             expect(m->checkedCount() == m->size(),
                    QStringLiteral("every address was checked (%1 of %2)").arg(m->checkedCount()).arg(m->size()));
             const QString status = controller()->statusText();
             expect(controller()->balancesComplete() && status.startsWith(QStringLiteral("Up to date")),
                    QStringLiteral("status: ") + status);
             expect(controller()->ethBalancesAt().isValid(),
                    QStringLiteral("the Ethereum balances are dated: %1")
                        .arg(controller()->ethBalancesAt().toString(QStringLiteral("HH:mm:ss"))));
         },
         nullptr},
        {QStringLiteral("second-dialog"), openAddDialog, [importDialog] { return importDialog() != nullptr; }, 5000,
         [this] { expect(true, QStringLiteral("the Wallets page opens the add-wallet dialog")); }, importDialog},
        {QStringLiteral("second-added"),
         [this, importDialog, typeName, typeWords, clickLater] {
             typeName(kSecondName);
             typeWords(kSecondTestMnemonic);
             clickLater(findButton(importDialog(), QStringLiteral("Add wallet")));
         },
         [controller, importDialog] { return controller()->store().wallets.size() == 2 && !importDialog(); }, 20000,
         [this, storeHas, controller] {
             expect(storeHas(kFirstName) && storeHas(kSecondName), QStringLiteral("a second seed joins the first"));
             expect(controller()->store().allAccounts().size() == 8, QStringLiteral("8 accounts now"));
         },
         nullptr},
        {QStringLiteral("duplicate-dialog"), openAddDialog, [importDialog] { return importDialog() != nullptr; }, 5000,
         nullptr, importDialog},
        {QStringLiteral("duplicate-refused"),
         [this, importDialog, typeWords, clickLater] {
             typeWords(kPublicTestMnemonic);
             clickLater(findButton(importDialog(), QStringLiteral("Add wallet")));
         },
         [this, importDialog] {
             return importDialog() && visibleText(importDialog()).contains(QStringLiteral("already has these keys"));
         },
         5000,
         [this, controller, importDialog] {
             expect(controller()->store().wallets.size() == 2, QStringLiteral("the same seed again is refused"));
             expect(visibleText(importDialog()).contains(kFirstName), QStringLiteral("the refusal names the wallet"));
         },
         importDialog},
        {QStringLiteral("duplicate-cancelled"),
         [this, importDialog, clickLater] { clickLater(findButton(importDialog(), QStringLiteral("Cancel"))); },
         [importDialog] { return !importDialog(); }, 3000, nullptr, nullptr},
        {QStringLiteral("keys-dialog"), openAddDialog, [importDialog] { return importDialog() != nullptr; }, 5000,
         nullptr, importDialog},
        {QStringLiteral("bad-key"), [typeKeys] { typeKeys(QStringLiteral("zpub-not-a-real-key")); }, nullptr, 3000,
         [this, importEnabled, importDialog] {
             expect(!importEnabled(), QStringLiteral("a bad public key: Add wallet stays disabled"));
             expect(visibleText(importDialog()).contains(QStringLiteral("Key 1 is not")),
                    QStringLiteral("a bad public key: the status names it by number"));
         },
         importDialog},
        {QStringLiteral("keys-typed"),
         [typeKeys, typeName] {
             typeName(kKeysName);
             const QList<wallet::AccountKey> derived =
                 wallet::WalletStore::deriveAccounts(chain::bip39::split(kThirdTestMnemonic), {});
             typeKeys(derived.value(0).xpub);
         },
         nullptr, 3000,
         [this, importEnabled, importDialog] {
             expect(importEnabled(), QStringLiteral("a valid zpub enables Add wallet"));
             expect(visibleText(importDialog()).contains(QStringLiteral("1 account key")),
                    QStringLiteral("a valid zpub: the status says so"));
         },
         importDialog},
        {QStringLiteral("keys-added"),
         [this, importDialog, clickLater] {
             scanFinished_ = false; // the restart this add causes reports anew
             clickLater(findButton(importDialog(), QStringLiteral("Add wallet")));
         },
         [controller, importDialog] { return controller()->store().wallets.size() == 3 && !importDialog(); }, 20000,
         [this, controller] {
             const wallet::WalletEntry last = controller()->store().wallets.last();
             expect(last.label == kKeysName && last.fromKeys && last.accounts.size() == 1,
                    QStringLiteral("public keys make a wallet of their own, marked as such"));
         },
         nullptr},
        {QStringLiteral("all-scanned"), [this] { window_->showPage(QStringLiteral("wallets")); },
         [this, controller] { return scanFinished_ && controller()->balancesComplete(); }, 600000,
         [this, controller, walletsTable] {
             const wallet::WalletModel* m = controller()->model();
             const QList<wallet::WalletEntry>& list = controller()->store().wallets;
             expect(controller()->balancesComplete(), QStringLiteral("status: ") + controller()->statusText());
             expect(walletsTable() && walletsTable()->model()->rowCount() == 3,
                    QStringLiteral("the Wallets page lists all 3"));
             int rows = 0;
             bool everyFilled = true;
             for (const wallet::WalletEntry& w : list) {
                 const wallet::WalletCounts counts = m->counts(w.id);
                 rows += counts.rows;
                 everyFilled = everyFilled && counts.rows > 0 && counts.checked == counts.rows;
             }
             expect(rows == m->size(), QStringLiteral("the wallets' rows add up to all %1 addresses").arg(m->size()));
             expect(everyFilled, QStringLiteral("every wallet has addresses, all checked"));
             for (const wallet::AssetInfo& a : wallet::assets()) {
                 chain::Amount sum;
                 for (const wallet::WalletEntry& w : list)
                     sum += m->total(a.asset, w.id);
                 expect(sum == m->total(a.asset),
                        QStringLiteral("%1: the wallets' totals add up to the overall total").arg(a.symbol));
             }
         },
         nullptr},
        {QStringLiteral("addresses-of-wallet"),
         [this, selectWallet, walletsPage, clickLater] {
             selectWallet(kSecondName);
             clickLater(findButton(walletsPage(), QStringLiteral("View addresses")));
         },
         nullptr, 3000,
         [this, controller, tableRows] {
             const QList<wallet::WalletEntry>& list = controller()->store().wallets;
             const quint32 id = list.value(1).id;
             expect(tableRows() == controller()->model()->counts(id).rows,
                    QStringLiteral("View addresses narrows the table to that wallet (%1 rows)").arg(tableRows()));
         },
         nullptr},
        {QStringLiteral("export-csv"),
         [this] {
             QString error;
             if (auto* page = window_->findChild<ui::AddressesPage*>())
                 page->exportCsv(outDir_ + QStringLiteral("/addresses.csv"), &error);
         },
         nullptr, 3000,
         [this, tableRows] {
             QFile f(outDir_ + QStringLiteral("/addresses.csv"));
             const QByteArray raw = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
             const QList<QByteArray> lines = raw.trimmed().split('\n');
             expect(raw.startsWith("\xEF\xBB\xBFwallet,chain,type,path,address,state,BTC,ETH,USDT,USDC,USD"),
                    QStringLiteral("the CSV starts with a BOM and its header"));
             expect(lines.size() == tableRows() + 1,
                    QStringLiteral("the CSV has one line per row shown (%1)").arg(lines.size() - 1));
             const bool onlyThatWallet = std::all_of(lines.cbegin() + 1, lines.cend(), [](const QByteArray& line) {
                 return line.startsWith(kSecondName.toUtf8() + ',');
             });
             expect(onlyThatWallet, QStringLiteral("and only the wallet the table is narrowed to"));
             auto* page = window_->findChild<ui::AddressesPage*>();
             if (page)
                 page->showWallet(0);
         },
         nullptr},
        {QStringLiteral("rename-dialog"),
         [this, selectWallet, walletsPage, clickLater] {
             window_->showPage(QStringLiteral("wallets"));
             selectWallet(kSecondName);
             clickLater(findButton(walletsPage(), QStringLiteral("Rename")));
         },
         [namedDialog] { return namedDialog("renameWalletDialog") != nullptr; }, 5000, nullptr,
         [namedDialog]() -> QWidget* { return namedDialog("renameWalletDialog"); }},
        {QStringLiteral("renamed"),
         [this, namedDialog, clickLater] {
             QWidget* dialog = namedDialog("renameWalletDialog");
             if (auto* edit = dialog ? dialog->findChild<QLineEdit*>() : nullptr)
                 edit->setText(kSecondNewName);
             clickLater(findButton(dialog, QStringLiteral("Rename")));
         },
         [storeHas] { return storeHas(kSecondNewName); }, 5000,
         [this, storeHas, walletsTable] {
             expect(storeHas(kSecondNewName) && !storeHas(kSecondName), QStringLiteral("the new name is stored"));
             bool shown = false;
             for (int row = 0; walletsTable() && row < walletsTable()->model()->rowCount(); ++row)
                 shown = shown || walletsTable()->model()->index(row, 0).data().toString() == kSecondNewName;
             expect(shown, QStringLiteral("and shown on the Wallets page"));
         },
         nullptr},
        {QStringLiteral("public-keys"),
         [this, selectWallet, walletsPage, clickLater] {
             selectWallet(kFirstName);
             clickLater(findButton(walletsPage(), QStringLiteral("Public keys")));
         },
         [namedDialog] { return namedDialog("walletKeysDialog") != nullptr; }, 5000,
         [this, controller, namedDialog] {
             expect(visibleText(namedDialog("walletKeysDialog")).contains(controller()->store().wallets.value(0).accounts.value(0).xpub),
                    QStringLiteral("the wallet's public keys are on show"));
         },
         [namedDialog]() -> QWidget* { return namedDialog("walletKeysDialog"); }},
        {QStringLiteral("public-keys-closed"),
         [this, namedDialog, clickLater] {
             clickLater(findButton(namedDialog("walletKeysDialog"), QStringLiteral("Close")));
         },
         [namedDialog] { return namedDialog("walletKeysDialog") == nullptr; }, 3000, nullptr, nullptr},
        {QStringLiteral("addresses"), [this] { window_->showPage(QStringLiteral("addresses")); }, nullptr, 3000,
         [this, controller, tableRows] {
             expect(tableRows() == controller()->model()->size(),
                    QStringLiteral("the table lists every address (%1)").arg(tableRows()));
         },
         nullptr},
        {QStringLiteral("search"),
         [this] {
             auto* page = window_->findChild<ui::AddressesPage*>();
             if (auto* search = page ? page->findChild<QLineEdit*>(QStringLiteral("mimSearch")) : nullptr)
                 search->setText(QStringLiteral("bc1q"));
         },
         nullptr, 3000,
         [this, controller, tableRows] {
             int expected = 0;
             const wallet::WalletModel* m = controller()->model();
             for (int i = 0; i < m->size(); ++i)
                 if (m->at(i).address.contains(QStringLiteral("bc1q"), Qt::CaseInsensitive)
                     || m->at(i).path.contains(QStringLiteral("bc1q"), Qt::CaseInsensitive))
                     ++expected;
             expect(tableRows() == expected,
                    QStringLiteral("search \"bc1q\": %1 rows, expected %2").arg(tableRows()).arg(expected));
         },
         nullptr},
        {QStringLiteral("funded-filter"),
         [this] {
             auto* page = window_->findChild<ui::AddressesPage*>();
             if (auto* search = page ? page->findChild<QLineEdit*>(QStringLiteral("mimSearch")) : nullptr)
                 search->clear();
             for (QComboBox* combo : page ? page->findChildren<QComboBox*>() : QList<QComboBox*>())
                 if (combo->findText(QStringLiteral("Funded")) >= 0)
                     combo->setCurrentText(QStringLiteral("Funded"));
         },
         nullptr, 3000,
         [this, controller, tableRows] {
             expect(tableRows() == controller()->model()->fundedCount(),
                    QStringLiteral("filter Funded: %1 rows, model says %2 funded")
                        .arg(tableRows())
                        .arg(controller()->model()->fundedCount()));
         },
         nullptr},
        {QStringLiteral("settings"),
         [this] {
             auto* page = window_->findChild<ui::AddressesPage*>();
             for (QComboBox* combo : page ? page->findChildren<QComboBox*>() : QList<QComboBox*>())
                 combo->setCurrentIndex(0);
             window_->showPage(QStringLiteral("settings"));
         },
         nullptr, 3000,
         [this, settingsPage] {
             const QString text = visibleText(settingsPage());
             expect(text.contains(QStringLiteral("3 wallets")), QStringLiteral("Settings says how many wallets"));
             expect(findButton(settingsPage(), QStringLiteral("Remove all wallets from this PC")) != nullptr,
                    QStringLiteral("Settings offers to remove them all"));
         },
         nullptr},
        {QStringLiteral("settings-saved"),
         [this, settingsPage] {
             QList<QSpinBox*> spins = settingsPage()->findChildren<QSpinBox*>();
             if (!spins.isEmpty()) spins.first()->setValue(100); // Bitcoin receive gap
             if (QWidget* save = findButton(settingsPage(), QStringLiteral("Save")))
                 static_cast<QAbstractButton*>(save)->click();
         },
         [controller] { return controller()->store().settings.btcReceiveGap == 100; }, 5000,
         [this, controller] {
             expect(controller()->store().settings.btcReceiveGap == 100,
                    QStringLiteral("Bitcoin receive gap saved as 100"));
         },
         nullptr},
        {QStringLiteral("reopened"),
         [this, controller] {
             // Before the gap came down to 100 a second seed's address at index
             // 150 was found: the reopened window's scan rightly stops short of it.
             rowsBeforeReopen_ = usedWithinGaps(controller());
             totalsBeforeReopen_ = totalsText(controller());
             scanFinished_ = false;
             // Close the window like the user would, then start a fresh one:
             // it has to find the wallet in wallet.dat on its own.
             delete window_.data();
             window_ = makeWindow_();
             connect(window_->controller(), &WalletController::scanFinished, this,
                     [this] { scanFinished_ = true; });
         },
         // The totals are compared once the reopened window's are final too.
         [this, controller] {
             return scanFinished_ && controller()->hasWallet() && controller()->balancesComplete();
         },
         300000,
         [this, controller, storeHas] {
             expect(controller()->store().wallets.size() == 3 && storeHas(kFirstName) && storeHas(kSecondNewName)
                        && storeHas(kKeysName),
                    QStringLiteral("reopened: all 3 wallets loaded from wallet.dat, with their names, without a seed"));
             expect(controller()->balancesComplete(),
                    QStringLiteral("reopened: status ") + controller()->statusText());
             expect(controller()->store().settings.btcReceiveGap == 100,
                    QStringLiteral("reopened: the saved gap (100) persisted"));
             expect(controller()->model()->usedCount() == rowsBeforeReopen_,
                    QStringLiteral("reopened: same used addresses (%1 vs %2)")
                        .arg(controller()->model()->usedCount())
                        .arg(rowsBeforeReopen_));
             expect(totalsText(controller()) == totalsBeforeReopen_,
                    QStringLiteral("reopened: same totals (%1)").arg(totalsText(controller())));
         },
         nullptr},
        {QStringLiteral("remove-one-confirm"),
         [this, selectWallet, walletsPage] {
             window_->showPage(QStringLiteral("wallets"));
             selectWallet(kSecondNewName);
             if (QWidget* remove = findButton(walletsPage(), QStringLiteral("Remove")))
                 static_cast<QAbstractButton*>(remove)->click();
         },
         [confirmBox] { return confirmBox() != nullptr; }, 5000,
         [this, confirmBox] {
             expect(confirmBox() && confirmBox()->text().contains(kSecondNewName),
                    QStringLiteral("removing one wallet asks first, by name"));
         },
         [confirmBox]() -> QWidget* { return confirmBox(); }},
        {QStringLiteral("removed-one"),
         [this, confirmBox, controller] {
             removedWallet_ = controller()->store().wallets.value(1).id;
             scanFinished_ = false;
             if (QMessageBox* box = confirmBox())
                 for (QAbstractButton* b : box->buttons())
                     if (b->text() == QStringLiteral("Remove wallet")) b->click();
         },
         [controller] { return controller()->store().wallets.size() == 2; }, 10000,
         [this, controller, storeHas, walletsTable] {
             const wallet::WalletModel* m = controller()->model();
             expect(!storeHas(kSecondNewName) && storeHas(kFirstName) && storeHas(kKeysName),
                    QStringLiteral("only that wallet is gone"));
             expect(m->counts(removedWallet_).rows == 0, QStringLiteral("its addresses are gone"));
             expect(m->counts(controller()->store().wallets.value(0).id).rows > 0
                        && m->counts(controller()->store().wallets.value(1).id).rows > 0,
                    QStringLiteral("the others keep theirs"));
             expect(QFile::exists(wallet::WalletStore::filePath()), QStringLiteral("wallet.dat stays"));
             expect(walletsTable() && walletsTable()->model()->rowCount() == 2,
                    QStringLiteral("the Wallets page lists the other 2"));
         },
         nullptr},
        {QStringLiteral("others-still-followed"), nullptr,
         [this, controller] { return scanFinished_ && controller()->balancesComplete(); }, 300000,
         [this, controller] {
             expect(controller()->balancesComplete(),
                    QStringLiteral("after removing one, status: ") + controller()->statusText());
         },
         nullptr},
        {QStringLiteral("remove-all-confirm"),
         [this, settingsPage] {
             window_->showPage(QStringLiteral("settings"));
             if (QWidget* remove = findButton(settingsPage(), QStringLiteral("Remove all wallets from this PC")))
                 static_cast<QAbstractButton*>(remove)->click();
         },
         [confirmBox] { return confirmBox() != nullptr; }, 5000,
         [this, confirmBox] {
             expect(confirmBox() && confirmBox()->text().contains(QStringLiteral("Remove every wallet")),
                    QStringLiteral("removing them all asks for confirmation first"));
         },
         [confirmBox]() -> QWidget* { return confirmBox(); }},
        {QStringLiteral("removed"),
         [this, confirmBox] {
             if (QMessageBox* box = confirmBox())
                 for (QAbstractButton* b : box->buttons())
                     if (b->text() == QStringLiteral("Remove all wallets")) b->click();
         },
         [controller] { return !controller()->hasWallet(); }, 10000,
         [this, controller] {
             expect(!controller()->hasWallet(), QStringLiteral("every wallet is gone"));
             expect(!QFile::exists(wallet::WalletStore::filePath()), QStringLiteral("wallet.dat was deleted"));
             expect(controller()->model()->size() == 0, QStringLiteral("no addresses are shown any more"));
             window_->showPage(QStringLiteral("overview"));
         },
         nullptr},
        {QStringLiteral("back-to-empty"), nullptr, nullptr, 3000,
         [this] {
             expect(findButton(window_, QStringLiteral("Add wallet")) != nullptr,
                    QStringLiteral("the Overview offers \"Add wallet\" again"));
         },
         nullptr},
    };
}

void Walkthrough::runStep(int index)
{
    if (index >= steps_.size()) {
        finish();
        return;
    }
    currentChecks_ = QJsonArray();
    Step& step = steps_[index];
    if (step.act) step.act();

    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    disconnect(poll_, nullptr, this, nullptr);
    connect(poll_, &QTimer::timeout, this, [this, index, clock] {
        const Step& s = steps_[index];
        const bool ready = !s.ready || s.ready();
        const bool late = clock->elapsed() > s.timeoutMs;
        if (!ready && !late) return;
        poll_->stop();
        finishStep(index, !ready);
    });
    poll_->start();
}

void Walkthrough::finishStep(int index, bool timedOut)
{
    // Let the UI settle (coalesced repaints, layouts) before checking and photographing.
    QTimer::singleShot(500, this, [this, index, timedOut] {
        const Step& step = steps_[index];
        if (timedOut) expect(false, QStringLiteral("timed out waiting for the app to react"));
        if (step.check) step.check();
        QWidget* target = step.shot ? step.shot() : window_.data();
        const QString file = QStringLiteral("%1-%2.png").arg(index + 1, 2, 10, QLatin1Char('0')).arg(step.name);
        if (target) target->grab().save(outDir_ + QLatin1Char('/') + file);
        results_.append(QJsonObject{{QStringLiteral("step"), index + 1},
                                    {QStringLiteral("name"), step.name},
                                    {QStringLiteral("screenshot"), target ? file : QString()},
                                    {QStringLiteral("checks"), currentChecks_}});
        runStep(index + 1);
    });
}

void Walkthrough::finish()
{
    const QJsonObject summary{{QStringLiteral("failures"), failures_}, {QStringLiteral("steps"), results_}};
    QFile f(outDir_ + QStringLiteral("/walkthrough.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(summary).toJson(QJsonDocument::Indented));
    QCoreApplication::exit(failures_ == 0 ? 0 : 1);
}

} // namespace app
