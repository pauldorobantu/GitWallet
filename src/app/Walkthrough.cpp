#include "app/Walkthrough.h"

#include "app/WalletController.h"
#include "ui/MainWindow.h"
#include "ui/dialogs/ImportSeedDialog.h"
#include "ui/pages/AddressesPage.h"
#include "ui/pages/SettingsPage.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTableView>
#include <QTimer>

#include <memory>

namespace app {

namespace {

// The public BIP39 test mnemonic (BIP84 test vectors); nothing else is typed.
const QString kPublicTestMnemonic = QStringLiteral(
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about");

QString totalsText(const WalletController* c)
{
    QStringList parts;
    for (const wallet::AssetInfo& a : wallet::assets())
        parts << a.symbol + QLatin1Char('=')
                     + QString::fromStdString(c->model()->total(a.asset).format(a.decimals, a.decimals));
    return parts.join(QStringLiteral(", "));
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
        if (auto* edit = importDialog() ? importDialog()->findChild<QPlainTextEdit*>() : nullptr)
            edit->setPlainText(text);
    };
    auto importEnabled = [this, importDialog] {
        QWidget* b = findButton(importDialog(), QStringLiteral("Import"));
        return b && b->isEnabled();
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
             expect(findButton(window_, QStringLiteral("Import seed")) != nullptr,
                    QStringLiteral("the Overview offers \"Import seed\""));
         },
         nullptr},
        {QStringLiteral("import-dialog"),
         [this] {
             // Asynchronous click: the dialog runs its own event loop (exec),
             // inside which the rest of the walkthrough keeps going.
             if (QWidget* b = findButton(window_, QStringLiteral("Import seed")))
                 QTimer::singleShot(0, static_cast<QAbstractButton*>(b), &QAbstractButton::click);
         },
         [importDialog] { return importDialog() != nullptr; }, 5000,
         [this, importEnabled] {
             expect(true, QStringLiteral("the import dialog opened"));
             expect(!importEnabled(), QStringLiteral("Import is disabled while the field is empty"));
         },
         importDialog},
        {QStringLiteral("too-few-words"), [typeWords] { typeWords(QStringLiteral("abandon abandon abandon")); },
         nullptr, 3000,
         [this, importEnabled, importDialog] {
             expect(!importEnabled(), QStringLiteral("3 words: Import stays disabled"));
             expect(visibleText(importDialog()).contains(QStringLiteral("Needs 12")),
                    QStringLiteral("3 words: the status asks for 12, 15, 18, 21 or 24 words"));
         },
         importDialog},
        {QStringLiteral("unknown-word"),
         [typeWords] { typeWords(QStringLiteral("abandon ").repeated(11) + QStringLiteral("bitcoinx")); },
         nullptr, 3000,
         [this, importEnabled, importDialog] {
             expect(!importEnabled(), QStringLiteral("an unknown word: Import stays disabled"));
             expect(visibleText(importDialog()).contains(QStringLiteral("not in the BIP39 list")),
                    QStringLiteral("an unknown word: the status names its position"));
         },
         importDialog},
        {QStringLiteral("bad-checksum"), [typeWords, twelveAbandon] { typeWords(twelveAbandon); }, nullptr, 3000,
         [this, importEnabled, importDialog] {
             expect(!importEnabled(), QStringLiteral("valid words, wrong checksum: Import stays disabled"));
             expect(visibleText(importDialog()).contains(QStringLiteral("hecksum"), Qt::CaseInsensitive),
                    QStringLiteral("wrong checksum: the status says so"));
         },
         importDialog},
        {QStringLiteral("valid-seed"), [typeWords] { typeWords(kPublicTestMnemonic); }, nullptr, 3000,
         [this, importEnabled, importDialog] {
             expect(importEnabled(), QStringLiteral("a valid 12-word phrase enables Import"));
             expect(visibleText(importDialog()).contains(QStringLiteral("checksum OK")),
                    QStringLiteral("valid phrase: the status says checksum OK"));
         },
         importDialog},
        {QStringLiteral("imported"),
         [this, importDialog] {
             if (QWidget* b = findButton(importDialog(), QStringLiteral("Import")))
                 QTimer::singleShot(0, static_cast<QAbstractButton*>(b), &QAbstractButton::click);
         },
         [controller, importDialog] { return controller()->hasWallet() && !importDialog(); }, 20000,
         [this, controller] {
             expect(controller()->hasWallet(), QStringLiteral("the wallet exists after Import"));
             expect(QFile::exists(wallet::WalletStore::filePath()),
                    QStringLiteral("wallet.dat was written (DPAPI-encrypted xpubs, no seed)"));
             expect(controller()->store().accounts.size() == 4,
                    QStringLiteral("4 accounts: native / nested SegWit, legacy, Ethereum"));
             QFile f(wallet::WalletStore::filePath());
             const QByteArray raw = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
             expect(!raw.contains("abandon") && !raw.contains("zpub"),
                    QStringLiteral("wallet.dat holds neither the words nor readable xpubs"));
         },
         nullptr},
        {QStringLiteral("scan-finished"), nullptr, [this] { return scanFinished_; }, 300000,
         [this, controller] {
             const wallet::WalletModel* m = controller()->model();
             expect(m->size() > 0, QStringLiteral("addresses were derived: %1").arg(m->size()));
             expect(m->checkedCount() == m->size(),
                    QStringLiteral("every address was checked (%1 of %2)").arg(m->checkedCount()).arg(m->size()));
             expect(controller()->statusText().startsWith(QStringLiteral("Up to date")),
                    QStringLiteral("status: ") + controller()->statusText());
         },
         nullptr},
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
         [this, controller, settingsPage] {
             const QString text = visibleText(settingsPage());
             expect(text.contains(controller()->store().accounts.value(0).xpub),
                    QStringLiteral("Settings shows the account xpubs"));
             expect(findButton(settingsPage(), QStringLiteral("Remove wallet from this PC")) != nullptr,
                    QStringLiteral("Settings offers Remove wallet"));
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
             rowsBeforeReopen_ = controller()->model()->usedCount();
             totalsBeforeReopen_ = totalsText(controller());
             scanFinished_ = false;
             // Close the window like the user would, then start a fresh one:
             // it has to find the wallet in wallet.dat on its own.
             delete window_.data();
             window_ = makeWindow_();
             connect(window_->controller(), &WalletController::scanFinished, this,
                     [this] { scanFinished_ = true; });
         },
         [this, controller] { return scanFinished_ && controller()->hasWallet(); }, 300000,
         [this, controller] {
             expect(controller()->hasWallet(), QStringLiteral("reopened: the wallet loaded from wallet.dat without the seed"));
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
        {QStringLiteral("remove-confirm"),
         [this, settingsPage] {
             window_->showPage(QStringLiteral("settings"));
             if (QWidget* remove = findButton(settingsPage(), QStringLiteral("Remove wallet from this PC")))
                 static_cast<QAbstractButton*>(remove)->click();
         },
         [confirmBox] { return confirmBox() != nullptr; }, 5000,
         [this, confirmBox] {
             expect(confirmBox() && confirmBox()->text().contains(QStringLiteral("Remove the wallet")),
                    QStringLiteral("removing asks for confirmation first"));
         },
         [confirmBox]() -> QWidget* { return confirmBox(); }},
        {QStringLiteral("removed"),
         [this, confirmBox] {
             if (QMessageBox* box = confirmBox())
                 for (QAbstractButton* b : box->buttons())
                     if (b->text() == QStringLiteral("Remove wallet")) b->click();
         },
         [controller] { return !controller()->hasWallet(); }, 10000,
         [this, controller] {
             expect(!controller()->hasWallet(), QStringLiteral("the wallet is gone"));
             expect(!QFile::exists(wallet::WalletStore::filePath()), QStringLiteral("wallet.dat was deleted"));
             expect(controller()->model()->size() == 0, QStringLiteral("no addresses are shown any more"));
             window_->showPage(QStringLiteral("overview"));
         },
         nullptr},
        {QStringLiteral("back-to-empty"), nullptr, nullptr, 3000,
         [this] {
             expect(findButton(window_, QStringLiteral("Import seed")) != nullptr,
                    QStringLiteral("the Overview offers \"Import seed\" again"));
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
