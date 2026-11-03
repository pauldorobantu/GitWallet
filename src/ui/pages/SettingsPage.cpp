#include "ui/pages/SettingsPage.h"

#include "app/WalletController.h"
#include "core/SettingsRepo.h"
#include "net/ElectrumClient.h"
#include "ui/DialogChrome.h"
#include "ui/Feedback.h"
#include "ui/Figures.h"
#include "ui/LanguageManager.h"
#include "ui/MainWindow.h"
#include "ui/PageKit.h"
#include "ui/design/Style.h"
#include "ui/dialogs/PasswordDialog.h"
#include "wallet/Types.h"
#include "wallet/WalletStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>

namespace app::ui {

namespace {

constexpr int kGapMin = 20;
constexpr int kGapMax = 5000;
constexpr int kRefreshMinSeconds = 15;
constexpr int kRefreshMaxSeconds = 3600;
constexpr int kListLines = 5; // visible lines of a server list

QSpinBox* makeSpin(QWidget* parent, int min, int max, int step, const QString& suffix,
                   const QString& tip)
{
    auto* spin = new QSpinBox(parent);
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setSuffix(suffix);
    spin->setAccelerated(true);
    // Typing 5 into a 20–5000 field gives 20, not silently the old value.
    spin->setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
    spin->setToolTip(tip);
    return spin;
}

// One entry per line. The placeholder lists the built-in entries an empty
// field stands for.
QPlainTextEdit* makeListEdit(QWidget* parent, const QString& builtInTitle, const QStringList& builtIn)
{
    auto* edit = new QPlainTextEdit(parent);
    edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    edit->setTabChangesFocus(true);
    edit->setPlaceholderText(builtInTitle + QLatin1Char('\n') + builtIn.join(QLatin1Char('\n')));
    edit->setMaximumWidth(page::kFieldMaxW);
    // The QSS border, padding and font first; then exactly kListLines lines.
    edit->ensurePolished();
    const QMargins frame = edit->contentsMargins();
    const int chrome = frame.top() + frame.bottom() + 2 * qCeil(edit->document()->documentMargin());
    edit->setFixedHeight(QFontMetrics(edit->font()).lineSpacing() * kListLines + chrome);
    return edit;
}

// A muted line under a field (the server in use). Plain text: it shows
// configured host names and URLs.
QLabel* makeCaption(QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setMaximumWidth(page::kFieldMaxW);
    setTone(label, Tone::Muted);
    return label;
}

// The field of a list row: the edit with its caption right under it.
QWidget* withCaption(QWidget* parent, QWidget* field, QLabel* caption)
{
    auto* host = new QWidget(parent);
    auto* column = new QVBoxLayout(host);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(page::kFormVSpacing / 2);
    column->addWidget(field);
    column->addWidget(caption);
    return host;
}

// The trimmed, non-empty lines of a list field, each passed through `accept`
// (which may normalise it), without duplicates. Stops at the first line
// `accept` refuses and reports its 0-based number.
template <typename Accept>
bool readLines(const QPlainTextEdit* edit, Accept accept, QStringList* out, int* badLine)
{
    const QStringList lines = edit->toPlainText().split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        QString value;
        if (!accept(line, &value)) {
            *badLine = int(i);
            return false;
        }
        if (!out->contains(value))
            out->append(value);
    }
    return true;
}

bool acceptElectrum(const QString& line, QString* value)
{
    net::ElectrumServer server;
    if (!net::ElectrumServer::fromString(line, &server))
        return false;
    *value = server.toString();
    return true;
}

bool acceptRpc(const QString& line, QString* value)
{
    const QUrl url(line, QUrl::StrictMode);
    if (!line.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) || !url.isValid()
        || url.host().isEmpty())
        return false;
    *value = line;
    return true;
}

// Puts the cursor on a refused line, selected, so it can be fixed at once.
void selectLine(QPlainTextEdit* edit, int line)
{
    const QTextBlock block = edit->document()->findBlockByNumber(line);
    if (block.isValid()) {
        QTextCursor cursor(block);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        edit->setTextCursor(cursor);
        edit->ensureCursorVisible();
    }
    edit->setFocus();
}

} // namespace

struct SettingsPage::Ui : QObject
{
    using QObject::QObject;

    QFormLayout* scanForm = nullptr;
    QFormLayout* networkForm = nullptr;
    QSpinBox* receiveGap = nullptr;
    QSpinBox* changeGap = nullptr;
    QSpinBox* ethGap = nullptr;
    QSpinBox* ethRefresh = nullptr;
    QList<QPlainTextEdit*> servers; // one per wallet::networks() entry, in that order
    QList<QLabel*> inUse;
    QLabel* walletCount = nullptr;
    QWidget* removeRow = nullptr;
    QPushButton* remove = nullptr;
    QPushButton* exportLabels = nullptr;
    QPushButton* importLabels = nullptr;
    QLabel* passwordState = nullptr;
    QLabel* fileAside = nullptr;
    QPushButton* setPassword = nullptr;
    QPushButton* changePassword = nullptr;
    QPushButton* removePassword = nullptr;
    QPushButton* lockNow = nullptr;
    QComboBox* lockAfter = nullptr;
    QWidget* lockAfterRow = nullptr;
    QLabel* backupState = nullptr;
    QLabel* restoreReport = nullptr;
    QPushButton* makeBackup = nullptr;
    QPushButton* restoreBackup = nullptr;
    // A copy on its way, either way: one at a time.
    void labelsBusy(bool busy)
    {
        exportLabels->setEnabled(!busy);
        importLabels->setEnabled(!busy);
    }
    wallet::ScanSettings filled; // what the form last showed from the store

    void fill(const wallet::ScanSettings& s)
    {
        filled = s;
        receiveGap->setValue(s.utxoReceiveGap);
        changeGap->setValue(s.utxoChangeGap);
        ethGap->setValue(s.evmGap);
        ethRefresh->setValue(s.evmRefreshSeconds);
        const QList<wallet::NetworkInfo>& list = wallet::networks();
        for (qsizetype i = 0; i < list.size(); ++i)
            servers.at(i)->setPlainText(s.servers.value(list.at(i).id).join(QLatin1Char('\n')));
    }
};

SettingsPage::SettingsPage(WalletController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller)
{
    ui_ = new Ui(this);
    auto* root = new QVBoxLayout(this);
    page::applyPageLayout(root);

    // The three cards outgrow the minimum window height: they scroll, and the
    // action bar stays at the bottom.
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(page::kPageSpacing);

    const page::Section scan = page::makeSection(
        content, QStringLiteral("Scanning"),
        QStringLiteral("GitWallet keeps checking addresses until this many unused ones follow the "
                       "last used one. A seed that skipped long runs of addresses needs larger gaps."));
    ui_->scanForm = page::addSectionForm(scan);
    ui_->receiveGap = makeSpin(scan.card, kGapMin, kGapMax, 10, QStringLiteral(" addresses"),
                               QStringLiteral("Unused receive addresses checked past the last used one"));
    ui_->changeGap = makeSpin(scan.card, kGapMin, kGapMax, 10, QStringLiteral(" addresses"),
                              QStringLiteral("Unused change addresses checked past the last used one"));
    ui_->ethGap = makeSpin(scan.card, kGapMin, kGapMax, 10, QStringLiteral(" addresses"),
                           QStringLiteral("Unused Ethereum addresses checked past the last used one"));
    ui_->ethRefresh = makeSpin(scan.card, kRefreshMinSeconds, kRefreshMaxSeconds, 15, QStringLiteral(" s"),
                               QStringLiteral("How soon a new Ethereum balance shows. A wallet too large to read "
                                              "whole in this time reads its unused next addresses and those holding "
                                              "a balance this often, and the rest as fast as the endpoints allow."));
    ui_->scanForm->addRow(QStringLiteral("Bitcoin receive: look ahead"), ui_->receiveGap);
    ui_->scanForm->addRow(QStringLiteral("Bitcoin change: look ahead"), ui_->changeGap);
    ui_->scanForm->addRow(QStringLiteral("Ethereum: look ahead"), ui_->ethGap);
    ui_->scanForm->addRow(QStringLiteral("Ethereum refresh every"), ui_->ethRefresh);
    column->addWidget(scan.card);

    const page::Section network = page::makeSection(
        content, QStringLiteral("Network"),
        QStringLiteral("One per line; an empty list uses the built-in servers. Every connection uses "
                       "TLS and checks the server's certificate."));
    ui_->networkForm = page::addSectionForm(network);
    for (const wallet::NetworkInfo& info : wallet::networks()) {
        const bool utxo = info.family == wallet::Family::Utxo;
        auto* edit = makeListEdit(network.card,
                                  utxo ? QStringLiteral("Built-in servers (host:port):")
                                       : QStringLiteral("Built-in endpoints (https:// URLs):"),
                                  info.servers);
        auto* caption = makeCaption(network.card);
        ui_->servers.append(edit);
        ui_->inUse.append(caption);
        ui_->networkForm->addRow(utxo ? QStringLiteral("%1 Electrum servers").arg(info.name)
                                      : QStringLiteral("%1 RPC URLs").arg(info.name),
                                 withCaption(network.card, edit, caption));
    }
    column->addWidget(network.card);

    const page::Section walletSection = page::makeSection(
        content, QStringLiteral("Wallets"),
        QStringLiteral("GitWallet keeps only public keys (xpubs). They reveal every address and balance "
                       "but cannot spend. Each wallet's keys are on the Wallets page."));
    ui_->walletCount = new QLabel(walletSection.card);
    setTone(ui_->walletCount, Tone::Muted);
    walletSection.body->addWidget(ui_->walletCount);
    ui_->removeRow = new QWidget(walletSection.card);
    auto* removeLayout = new QHBoxLayout(ui_->removeRow);
    removeLayout->setContentsMargins(0, 0, 0, 0);
    removeLayout->setSpacing(page::kFormHSpacing);
    ui_->remove = new QPushButton(QStringLiteral("Remove all wallets from this PC"), ui_->removeRow);
    ui_->remove->setObjectName(QStringLiteral("removeAllWallets"));
    ui_->remove->setObjectName(QStringLiteral("dangerButton"));
    auto* removeNote = new QLabel(QStringLiteral("Deletes the saved xpubs and settings. Seeds are never "
                                                 "stored, so funds are not affected."),
                                  ui_->removeRow);
    removeNote->setWordWrap(true);
    setTone(removeNote, Tone::Muted);
    removeLayout->addWidget(ui_->remove);
    removeLayout->addWidget(removeNote, 1);
    walletSection.body->addWidget(ui_->removeRow);
    column->addWidget(walletSection.card);

    const page::Section labels = page::makeSection(
        content, QStringLiteral("Labels and notes"),
        QStringLiteral("The labels and notes you give transactions and addresses are on this PC alone, in its "
                       "encrypted database: no server can give them back. Keep a copy elsewhere. The file is in the "
                       "form other wallets read (BIP-329), and it is not encrypted."));
    auto* labelsRow = new QHBoxLayout;
    labelsRow->setSpacing(page::kFormHSpacing);
    auto* exportLabels = new QPushButton(QStringLiteral("Export labels"), labels.card);
    auto* importLabels = new QPushButton(QStringLiteral("Import labels"), labels.card);
    ui_->exportLabels = exportLabels;
    ui_->importLabels = importLabels;
    labelsRow->addWidget(exportLabels);
    labelsRow->addWidget(importLabels);
    labelsRow->addStretch(1);
    labels.body->addLayout(labelsRow);
    column->addWidget(labels.card);
    connect(exportLabels, &QPushButton::clicked, this, &SettingsPage::exportLabels);
    connect(importLabels, &QPushButton::clicked, this, &SettingsPage::importLabels);

    const page::Section alerts = page::makeSection(
        content, QStringLiteral("Payment alerts"),
        QStringLiteral("GitWallet tells you when a payment comes in, and the bell in the title bar lists them. It only knows while it is running: what arrives while it is closed is reported at the next start."));
    auto* alertsOn = new QCheckBox(QStringLiteral("Tell me when a payment comes in"), alerts.card);
    alertsOn->setObjectName(QStringLiteral("paymentAlerts"));
    alertsOn->setChecked(SettingsRepo().get(QStringLiteral("ui.paymentAlerts")) != QLatin1String("0"));
    auto* alertsWindows = new QCheckBox(QStringLiteral("As a Windows notification when this window is not in front"), alerts.card);
    alertsWindows->setObjectName(QStringLiteral("paymentAlertsWindows"));
    alertsWindows->setChecked(SettingsRepo().get(QStringLiteral("ui.paymentAlertsWindows")) != QLatin1String("0"));
    alertsWindows->setEnabled(alertsOn->isChecked());
    alertsWindows->setToolTip(QStringLiteral("With an app password set it names no amount: Windows keeps what it is told, whatever locks here."));
    auto* alertsSound = new QCheckBox(QStringLiteral("With a sound"), alerts.card);
    alertsSound->setObjectName(QStringLiteral("paymentAlertsSound"));
    alertsSound->setChecked(SettingsRepo().get(QStringLiteral("ui.paymentAlertsSound")) == QLatin1String("1"));
    alertsSound->setEnabled(alertsOn->isChecked());
    connect(alertsOn, &QCheckBox::toggled, this, [alertsWindows, alertsSound](bool on) {
        SettingsRepo().set(QStringLiteral("ui.paymentAlerts"), on ? QStringLiteral("1") : QStringLiteral("0"));
        alertsWindows->setEnabled(on);
        alertsSound->setEnabled(on);
    });
    connect(alertsSound, &QCheckBox::toggled, this, [](bool on) {
        SettingsRepo().set(QStringLiteral("ui.paymentAlertsSound"), on ? QStringLiteral("1") : QStringLiteral("0"));
    });
    connect(alertsWindows, &QCheckBox::toggled, this, [](bool on) {
        SettingsRepo().set(QStringLiteral("ui.paymentAlertsWindows"), on ? QStringLiteral("1") : QStringLiteral("0"));
    });
    alerts.body->addWidget(alertsOn);
    alerts.body->addWidget(alertsWindows);
    alerts.body->addWidget(alertsSound);
    column->addWidget(alerts.card);

    const page::Section password = page::makeSection(
        content, QStringLiteral("App password"),
        QStringLiteral("Without one, anyone using this Windows account can open GitWallet and see your wallets, and so can any program running under it. With one, what is saved here cannot be read without it. Seeds are not stored either way."));
    ui_->passwordState = new QLabel(password.card);
    ui_->passwordState->setObjectName(QStringLiteral("passwordState"));
    password.body->addWidget(ui_->passwordState);
    ui_->fileAside = new QLabel(password.card);
    ui_->fileAside->setObjectName(QStringLiteral("fileAside"));
    ui_->fileAside->setTextFormat(Qt::PlainText);
    ui_->fileAside->setWordWrap(true);
    setTone(ui_->fileAside, Tone::Warning);
    password.body->addWidget(ui_->fileAside);
    auto* passwordRow = new QHBoxLayout;
    passwordRow->setSpacing(page::kFormHSpacing);
    ui_->setPassword = new QPushButton(QStringLiteral("Set a password"), password.card);
    ui_->setPassword->setObjectName(QStringLiteral("setPassword"));
    ui_->changePassword = new QPushButton(QStringLiteral("Change it"), password.card);
    ui_->changePassword->setObjectName(QStringLiteral("changePassword"));
    ui_->removePassword = new QPushButton(QStringLiteral("Remove it"), password.card);
    ui_->removePassword->setObjectName(QStringLiteral("removePassword"));
    ui_->lockNow = new QPushButton(QStringLiteral("Lock now"), password.card);
    ui_->lockNow->setObjectName(QStringLiteral("lockNow"));
    passwordRow->addWidget(ui_->setPassword);
    passwordRow->addWidget(ui_->changePassword);
    passwordRow->addWidget(ui_->removePassword);
    passwordRow->addWidget(ui_->lockNow);
    passwordRow->addStretch(1);
    password.body->addLayout(passwordRow);
    ui_->lockAfterRow = new QWidget(password.card);
    auto* lockRow = new QHBoxLayout(ui_->lockAfterRow);
    lockRow->setContentsMargins(0, 0, 0, 0);
    lockRow->setSpacing(page::kFormHSpacing);
    lockRow->addWidget(new QLabel(QStringLiteral("Lock when idle for"), ui_->lockAfterRow));
    ui_->lockAfter = new QComboBox(ui_->lockAfterRow);
    ui_->lockAfter->setObjectName(QStringLiteral("lockAfter"));
    ui_->lockAfter->addItem(QStringLiteral("1 minute"), 1);
    ui_->lockAfter->addItem(QStringLiteral("5 minutes"), 5);
    ui_->lockAfter->addItem(QStringLiteral("15 minutes"), 15);
    ui_->lockAfter->addItem(QStringLiteral("30 minutes"), 30);
    ui_->lockAfter->addItem(QStringLiteral("1 hour"), 60);
    ui_->lockAfter->addItem(QStringLiteral("Never"), 0);
    lockRow->addWidget(ui_->lockAfter);
    lockRow->addStretch(1);
    password.body->addWidget(ui_->lockAfterRow);
    column->addWidget(password.card);
    connect(ui_->setPassword, &QPushButton::clicked, this, [this] { askPassword(int(PasswordDialog::Mode::SetApp)); });
    connect(ui_->changePassword, &QPushButton::clicked, this, [this] { askPassword(int(PasswordDialog::Mode::ChangeApp)); });
    connect(ui_->removePassword, &QPushButton::clicked, this, [this] { askPassword(int(PasswordDialog::Mode::RemoveApp)); });
    connect(ui_->lockNow, &QPushButton::clicked, this, &SettingsPage::lockRequested);
    connect(ui_->lockAfter, &QComboBox::activated, this, [this](int) {
        QString error;
        if (controller_ && !controller_->setLockMinutes(ui_->lockAfter->currentData().toInt(), &error)) {
            Feedback::show(this, Feedback::Type::Error, error);
            updatePassword();
        }
    });

    const page::Section backup = page::makeSection(
        content, QStringLiteral("Backup"),
        QStringLiteral("A single password-protected file to keep elsewhere or take to another PC. It holds your wallets' public keys and names, the addresses given out, the settings and every label and note. It cannot spend, and it does not replace your seeds: keep those written down separately."));
    ui_->backupState = new QLabel(backup.card);
    ui_->backupState->setObjectName(QStringLiteral("backupState"));
    ui_->backupState->setWordWrap(true);
    backup.body->addWidget(ui_->backupState);
    ui_->restoreReport = new QLabel(backup.card);
    ui_->restoreReport->setObjectName(QStringLiteral("restoreReport"));
    ui_->restoreReport->setTextFormat(Qt::PlainText);
    ui_->restoreReport->setWordWrap(true);
    setTone(ui_->restoreReport, Tone::Muted);
    ui_->restoreReport->hide();
    backup.body->addWidget(ui_->restoreReport);
    auto* backupRow = new QHBoxLayout;
    backupRow->setSpacing(page::kFormHSpacing);
    ui_->makeBackup = new QPushButton(QStringLiteral("Make a backup"), backup.card);
    ui_->makeBackup->setObjectName(QStringLiteral("makeBackup"));
    ui_->restoreBackup = new QPushButton(QStringLiteral("Restore from a backup"), backup.card);
    ui_->restoreBackup->setObjectName(QStringLiteral("restoreBackup"));
    backupRow->addWidget(ui_->makeBackup);
    backupRow->addWidget(ui_->restoreBackup);
    backupRow->addStretch(1);
    backup.body->addLayout(backupRow);
    column->addWidget(backup.card);
    connect(ui_->makeBackup, &QPushButton::clicked, this, &SettingsPage::chooseBackupFile);
    connect(ui_->restoreBackup, &QPushButton::clicked, this, &SettingsPage::chooseRestoreFile);

    auto* aboutLine = new QLabel(QStringLiteral("GitWallet %1 · MIT · Icons: Lucide").arg(QCoreApplication::applicationVersion()), content);
    aboutLine->setObjectName(QStringLiteral("aboutLine"));
    aboutLine->setTextFormat(Qt::PlainText);
    setTone(aboutLine, Tone::Muted);
    column->addWidget(aboutLine);

    column->addStretch(1);
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    const page::ActionBar bar = page::makeActionBar(this);
    auto* reset = new QPushButton(QStringLiteral("Reset to defaults"), bar.widget);
    reset->setToolTip(QStringLiteral("Fill in the default values; nothing changes until you save"));
    bar.left->addWidget(reset);
    auto* saveButton = new QPushButton(QStringLiteral("Save"), bar.widget);
    saveButton->setObjectName(QStringLiteral("primaryButton"));
    bar.right->addWidget(saveButton);
    root->addWidget(bar.widget);

    connect(reset, &QPushButton::clicked, this, &SettingsPage::resetToDefaults);
    connect(saveButton, &QPushButton::clicked, this, &SettingsPage::save);
    connect(ui_->remove, &QPushButton::clicked, this, &SettingsPage::confirmRemove);
    if (controller_) {
        // A wallet added, renamed or removed leaves what is typed here alone, unless
        // the stored settings themselves changed (a file loaded, everything removed).
        connect(controller_, &WalletController::walletChanged, this, [this] {
            const wallet::ScanSettings& s = controller_->store().settings;
            if (!(s == ui_->filled))
                ui_->fill(s);
            rebuildWallet();
            updateInUse();
            updatePassword();
            updateBackup();
        });
        connect(controller_, &WalletController::passwordChanged, this, &SettingsPage::updatePassword);
        connect(controller_, &WalletController::backupMade, this, &SettingsPage::updateBackup);
        connect(controller_, &WalletController::statusChanged, this, &SettingsPage::updateInUse);
    }
    // The captions are runtime text (trText) that the language walk cannot re-template.
    connect(LanguageManager::instance(), &LanguageManager::changed, this, &SettingsPage::updateInUse);
    connect(LanguageManager::instance(), &LanguageManager::changed, this, &SettingsPage::updatePassword);
    connect(LanguageManager::instance(), &LanguageManager::changed, this, &SettingsPage::updateBackup);
    load();
}

void SettingsPage::exportLabels()
{
    if (!controller_)
        return;
    // What is not in memory is not "nothing": it is why it says so.
    const QString blocker = controller_->labelsBlocker();
    if (!blocker.isEmpty()) {
        Feedback::show(this, Feedback::Type::Info, trText(blocker));
        return;
    }
    if (!controller_->hasLabels()) {
        Feedback::show(this, Feedback::Type::Info, trText(QStringLiteral("No label or note to export yet")));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, trText(QStringLiteral("Export labels")), QStringLiteral("gitwallet-labels.jsonl"),
                                                      QStringLiteral("JSON lines (*.jsonl);;All files (*)"));
    if (path.isEmpty())
        return;
    ui_->labelsBusy(true);
    controller_->writeLabelsFile(path, this, [this](bool ok, const QString& error) {
        ui_->labelsBusy(false);
        if (ok)
            Feedback::show(this, Feedback::Type::Success, trText(QStringLiteral("Labels exported")));
        else
            Feedback::show(this, Feedback::Type::Error, trText(QStringLiteral("Could not write the file: %1")).arg(error));
    });
}

void SettingsPage::importLabels()
{
    if (!controller_)
        return;
    const QString blocker = controller_->labelsBlocker();
    if (!blocker.isEmpty()) {
        Feedback::show(this, Feedback::Type::Info, trText(blocker));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, trText(QStringLiteral("Import labels")), QString(), QStringLiteral("JSON lines (*.jsonl);;All files (*)"));
    if (path.isEmpty())
        return;
    ui_->labelsBusy(true);
    controller_->importLabelsFile(path, this, [this](int added, int skipped, int later, const QString& error) {
        ui_->labelsBusy(false);
        if (added < 0) {
            Feedback::show(this, Feedback::Type::Error, trText(error));
            return;
        }
        Feedback::show(this, skipped > 0 ? Feedback::Type::Warning : Feedback::Type::Success,
                       skipped > 0 ? trText(QStringLiteral("%1 labels imported, %2 lines left out")).arg(added).arg(skipped)
                                   : trText(QStringLiteral("%1 labels imported")).arg(added));
        if (later > 0) {
            // More than a toast has room for, and the one chance to say not to throw the file away.
            auto* box = new QMessageBox(QMessageBox::Information, trText(QStringLiteral("Import labels")),
                                        trText(QStringLiteral("%1 lines name a transaction these wallets have not read yet, and look like a private key until they have. Keep the file, and import it again once the history is read. What is still refused then is left out for good."))
                                            .arg(later),
                                        QMessageBox::Ok, this);
            box->setAttribute(Qt::WA_DeleteOnClose);
            applyDialogChrome(box);
            box->open();
        }
    });
}

void SettingsPage::load()
{
    ui_->fill(controller_ ? controller_->store().settings : wallet::ScanSettings());
    rebuildWallet();
    updateInUse();
    updatePassword();
    updateBackup();
}

void SettingsPage::updatePassword()
{
    const bool has = controller_ && controller_->hasPassword();
    ui_->passwordState->setText(has ? trText(QStringLiteral("A password is set.")) : trText(QStringLiteral("No password is set.")));
    ui_->setPassword->setVisible(!has);
    ui_->changePassword->setVisible(has);
    ui_->removePassword->setVisible(has);
    ui_->lockNow->setVisible(has);
    ui_->lockAfterRow->setVisible(has);
    if (controller_) {
        const int at = ui_->lockAfter->findData(controller_->lockMinutes());
        if (at >= 0)
            ui_->lockAfter->setCurrentIndex(at);
    }
    // For as long as it is there: a start says so once, and this stays.
    const QString aside = controller_ ? controller_->unreadFileKept() : QString();
    ui_->fileAside->setVisible(!aside.isEmpty());
    if (!aside.isEmpty())
        ui_->fileAside->setText(trText(QStringLiteral("Kept beside the wallet file: %1. No password reaches it. If you did not put it there, check that these wallets are yours.")).arg(aside));
}

void SettingsPage::askPassword(int mode)
{
    if (!controller_)
        return;
    const auto chosen = PasswordDialog::Mode(mode);
    PasswordDialog::OldFiles oldFiles;
    oldFiles.formatCopies = controller_->walletFormatCopies();
    oldFiles.unread = controller_->walletFilesUnread();
    auto* dialog = new PasswordDialog(chosen, oldFiles, QString(), [this](const PasswordDialog::Filled& filled, const PasswordDialog::Finish& finish) {
        const int deleteOld = (filled.deleteFormatCopies ? WalletController::DeleteFormatCopies : 0) | (filled.deleteUnreadFiles ? WalletController::DeleteUnreadFiles : 0);
        controller_->changePassword(filled.current, filled.fresh, deleteOld, this, finish);
    }, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, chosen] {
        Feedback::show(this, Feedback::Type::Success,
                       chosen == PasswordDialog::Mode::RemoveApp ? trText(QStringLiteral("The password was removed")) : trText(QStringLiteral("The password is set")));
    });
    dialog->open();
}

void SettingsPage::updateBackup()
{
    const wallet::WalletStore* store = controller_ ? &controller_->store() : nullptr;
    if (!store || store->lastBackupMs <= 0) {
        ui_->backupState->setText(trText(QStringLiteral("No backup made on this PC yet.")));
    } else {
        QString line = trText(QStringLiteral("Last backup: %1.")).arg(QDateTime::fromMSecsSinceEpoch(store->lastBackupMs).toString(QStringLiteral("d MMM yyyy HH:mm")));
        // By which wallets, not how many: one removed and one added come to the same count.
        int since = int(store->wallets.size()) - store->lastBackupWallets;
        if (store->lastBackupNextWallet > 0)
            since = int(std::count_if(store->wallets.cbegin(), store->wallets.cend(), [store](const wallet::WalletEntry& entry) { return entry.id >= store->lastBackupNextWallet; }));
        if (since > 0)
            line += QLatin1Char(' ') + figures::counted(since, QStringLiteral("%1 wallet added since is not in it."), QStringLiteral("%1 wallets added since are not in it."));
        ui_->backupState->setText(line);
    }
}

void SettingsPage::chooseBackupFile()
{
    if (!controller_)
        return;
    const QString blocker = controller_->backupBlocker();
    if (!blocker.isEmpty()) {
        Feedback::show(this, Feedback::Type::Info, blocker);
        return;
    }
    const QString name = QStringLiteral("GitWallet-backup-%1.gwbackup").arg(QDate::currentDate().toString(Qt::ISODate));
    const QString path = QFileDialog::getSaveFileName(this, trText(QStringLiteral("Make a backup")), name, QStringLiteral("GitWallet backup (*.gwbackup)"));
    if (!path.isEmpty())
        makeBackupAt(path);
}

void SettingsPage::makeBackupAt(const QString& path)
{
    auto* dialog = new PasswordDialog(PasswordDialog::Mode::MakeBackup, {}, QDir::toNativeSeparators(path), [this, path](const PasswordDialog::Filled& filled, const PasswordDialog::Finish& finish) {
        controller_->writeBackup(path, filled.fresh, this, finish);
    }, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this] { Feedback::show(this, Feedback::Type::Success, trText(QStringLiteral("Backup made, and read back"))); });
    dialog->open();
}

void SettingsPage::chooseRestoreFile()
{
    if (!controller_)
        return;
    const QString path = QFileDialog::getOpenFileName(this, trText(QStringLiteral("Restore from a backup")), QString(), QStringLiteral("GitWallet backup (*.gwbackup);;All files (*)"));
    if (!path.isEmpty())
        restoreBackupFrom(path);
}

void SettingsPage::restoreBackupFrom(const QString& path)
{
    auto* dialog = new PasswordDialog(PasswordDialog::Mode::OpenBackup, {}, QDir::toNativeSeparators(path), [this, path](const PasswordDialog::Filled& filled, const PasswordDialog::Finish& finish) {
        controller_->restoreBackup(path, filled.current, this, [this, finish](const WalletController::RestoreReport& report) {
            finish(report.error);
            if (!report.error.isEmpty())
                return;
            QStringList lines;
            lines << trText(QStringLiteral("Wallets added: %1")).arg(report.walletsAdded);
            if (report.walletsHere > 0)
                lines << trText(QStringLiteral("Already on this PC: %1")).arg(report.walletsHere);
            if (report.keysApart > 0)
                lines << trText(QStringLiteral("Keys a wallet here lacks, added as a wallet of their own: %1. Check they are yours: nothing can show they come from the same seed.")).arg(report.keysApart);
            if (report.walletsRefused > 0) {
                lines << trText(QStringLiteral("Not added: %1")).arg(report.walletsRefused);
                // A backup cannot be listed anywhere else, so this is the only place to learn which.
                const int shown = std::min(int(report.refused.size()), 10);
                lines += report.refused.mid(0, shown);
                if (report.refused.size() > shown)
                    lines << trText(QStringLiteral("and %1 more")).arg(report.refused.size() - shown);
            }
            lines << trText(QStringLiteral("Addresses given out that this PC did not know of: %1")).arg(report.givenOut);
            lines << trText(QStringLiteral("Labels and notes: %1")).arg(report.labels);
            if (report.labelsKept > 0)
                lines << trText(QStringLiteral("Kept as they are here, where the backup words them differently: %1")).arg(report.labelsKept);
            if (report.labelsLeftOut > 0)
                lines << trText(QStringLiteral("Labels and notes left out: %1")).arg(report.labelsLeftOut);
            if (!report.labelsWhy.isEmpty())
                lines << trText(QStringLiteral("Labels and notes did not come along: %1")).arg(report.labelsWhy);
            if (report.serversKept)
                lines << trText(QStringLiteral("This PC's servers stay in use: the backup's were not taken."));
            if (report.gapsRose)
                lines << trText(QStringLiteral("This PC now looks as far ahead as the backup did: receive %1, change %2, Ethereum %3.")).arg(report.backupReceiveGap).arg(report.backupChangeGap).arg(report.backupEvmGap);
            if (report.gapsKept)
                lines << trText(QStringLiteral("The backup looks further ahead (receive %1, change %2, Ethereum %3) than GitWallet can follow with these wallets, so this PC's settings stay.")).arg(report.backupReceiveGap).arg(report.backupChangeGap).arg(report.backupEvmGap);
            auto* box = new QMessageBox(QMessageBox::Information, trText(QStringLiteral("Restored")), lines.join(QLatin1Char('\n')), QMessageBox::Ok, this);
            box->setObjectName(QStringLiteral("restoredBox"));
            box->setTextFormat(Qt::PlainText); // names out of a file
            box->setInformativeText(trText(QStringLiteral("Balances and transactions are read again from the servers: that can take a while.")));
            box->setAttribute(Qt::WA_DeleteOnClose);
            applyDialogChrome(box);
            // The box can be gone before anyone reads it: a lock closes it. This stays for as long as the window.
            ui_->restoreReport->setText(trText(QStringLiteral("Last restore: %1")).arg(lines.join(QStringLiteral(" · "))));
            ui_->restoreReport->show();
            // Its counts are nothing to show over a lock.
            auto* host = qobject_cast<MainWindow*>(window());
            if (host && host->isLocked())
                connect(host, &MainWindow::unlocked, box, [box] { box->open(); }, Qt::SingleShotConnection);
            else
                box->open();
        });
    }, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void SettingsPage::rebuildWallet()
{
    const int wallets = controller_ ? int(controller_->store().wallets.size()) : 0;
    ui_->walletCount->setText(wallets == 0 ? trText(QStringLiteral("No wallet on this PC yet."))
                              : wallets == 1
                                  ? trText(QStringLiteral("1 wallet on this PC."))
                                  : trText(QStringLiteral("%1 wallets on this PC.")).arg(wallets));
    // Older files beside wallet.dat hold keys too, with or without a wallet left.
    ui_->removeRow->setVisible(wallets > 0 || (controller_ && controller_->walletFilesKeptAside() > 0));
    // Labels of every card share one column.
    page::finishForms({ui_->scanForm, ui_->networkForm});
}

void SettingsPage::updateInUse()
{
    const bool active = controller_ && controller_->hasWallet();
    const QList<wallet::NetworkInfo>& list = wallet::networks();
    for (qsizetype i = 0; i < list.size(); ++i) {
        const QString server = active ? controller_->networkStatus(list.at(i).network).server : QString();
        ui_->inUse.at(i)->setText(server.isEmpty() ? trText(QStringLiteral("No server in use"))
                                                   : trText(QStringLiteral("In use: %1")).arg(server));
    }
}

void SettingsPage::resetToDefaults()
{
    const wallet::ScanSettings fromStore = ui_->filled;
    ui_->fill(wallet::ScanSettings());
    ui_->filled = fromStore; // not from the store: a rename elsewhere leaves them on show
    Feedback::show(this, Feedback::Type::Info,
                   trText(QStringLiteral("Defaults filled in; Save applies them")));
}

void SettingsPage::save()
{
    if (!controller_)
        return;
    QMap<QString, QStringList> servers;
    const QList<wallet::NetworkInfo>& list = wallet::networks();
    for (qsizetype i = 0; i < list.size(); ++i) {
        const wallet::NetworkInfo& info = list.at(i);
        const bool utxo = info.family == wallet::Family::Utxo;
        QStringList lines;
        int bad = -1;
        const bool read = utxo ? readLines(ui_->servers.at(i), acceptElectrum, &lines, &bad)
                               : readLines(ui_->servers.at(i), acceptRpc, &lines, &bad);
        if (!read) {
            selectLine(ui_->servers.at(i), bad);
            Feedback::show(this, Feedback::Type::Error,
                           (utxo ? trText(QStringLiteral("%1 Electrum servers, line %2: use host:port"))
                                 : trText(QStringLiteral("%1 RPC URLs, line %2: use an https:// URL")))
                               .arg(info.name)
                               .arg(bad + 1));
            return;
        }
        if (!lines.isEmpty())
            servers.insert(info.id, lines);
    }

    // Start from the stored settings so fields this page does not show survive.
    wallet::ScanSettings settings = controller_->store().settings;
    settings.utxoReceiveGap = ui_->receiveGap->value();
    settings.utxoChangeGap = ui_->changeGap->value();
    settings.evmGap = ui_->ethGap->value();
    settings.evmRefreshSeconds = ui_->ethRefresh->value();
    for (const wallet::NetworkInfo& info : wallet::networks())
        settings.servers.remove(info.id);
    for (auto it = servers.cbegin(); it != servers.cend(); ++it)
        settings.servers.insert(it.key(), it.value());

    askThenApply(settings, -1);
}

void SettingsPage::askThenApply(const wallet::ScanSettings& settings, int accepted)
{
    // Smaller gaps can hide addresses that have history, coins included: the next
    // scan stops before them, and only the user can say that is what they want. The
    // rows only know what has answered: until all have, what is left out is not known.
    const WalletController::GapImpact left = controller_->gapImpact(settings);
    const bool smaller = WalletController::gapsFall(controller_->store().settings, settings);
    const bool unknown = smaller && controller_->hasWallet() && !controller_->balancesComplete();
    const bool agreed = accepted >= 0 && left.used <= accepted;
    if (agreed || (left.used == 0 && left.overLimit == 0 && !unknown)) {
        apply(settings);
        return;
    }
    QString text;
    QString detail;
    if (left.used > 0) {
        text = left.atMost ? QStringLiteral("This can leave out up to %1 addresses that have been used.").arg(left.used)
               : left.used == 1
                   ? QStringLiteral("This leaves out 1 address that has been used.")
                   : QStringLiteral("This leaves out %1 addresses that have been used.").arg(left.used);
        detail = QStringLiteral("Holding a balance now: %1 of %2. GitWallet would stop showing and watching these "
                                "addresses, and their balances would leave the totals, until the gaps change "
                                "again. Nothing changes on the blockchain.")
                     .arg(left.funded)
                     .arg(left.used);
    } else if (left.overLimit > 0) {
        text = QStringLiteral("This asks for about %1 more addresses than GitWallet follows.").arg(left.overLimit);
        detail = QStringLiteral("The scan would stop at the limit, so no total would read as whole, until the gaps are "
                                "lowered or wallets removed. Nothing changes on the blockchain.");
    } else {
        text = QStringLiteral("Not every address has answered, so what this leaves out is not known.");
        detail = QStringLiteral("GitWallet could stop showing and watching addresses it has not read, and their "
                                "balances would not count in the totals, until the gaps are raised again. Nothing "
                                "changes on the blockchain.");
    }
    if (left.atMost)
        detail += QLatin1Char(' ') + QStringLiteral("A scan stops at a limit that this goes past.");
    if (left.used > 0 && unknown)
        detail += QLatin1Char(' ') + QStringLiteral("Not every address has answered, so there may be more.");
    auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Looking less far ahead"), text, QMessageBox::NoButton, this);
    box->setInformativeText(detail);
    QPushButton* saveAnyway = box->addButton(QStringLiteral("Save anyway"), QMessageBox::DestructiveRole);
    saveAnyway->setObjectName(QStringLiteral("dangerButton"));
    QPushButton* cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    box->setAttribute(Qt::WA_DeleteOnClose);
    applyDialogChrome(box);
    // open(), not exec(): no nested event loop while the scanners keep running. They
    // can find more behind the box, and that is asked again.
    const int shown = left.used;
    connect(box, &QDialog::finished, this, [this, box, saveAnyway, settings, shown] {
        if (box->clickedButton() == saveAnyway)
            askThenApply(settings, shown);
    });
    box->open();
}

void SettingsPage::apply(const wallet::ScanSettings& settings)
{
    // The controller words its failures as full sentences; shown as they are.
    QString error;
    if (!controller_->applySettings(settings, &error)) {
        Feedback::show(this, Feedback::Type::Error,
                       error.isEmpty() ? trText(QStringLiteral("Could not save the settings")) : error);
        return;
    }
    ui_->fill(controller_->store().settings); // as stored: normalised, without duplicates
    updateInUse();
    Feedback::show(this, Feedback::Type::Success, trText(QStringLiteral("Settings saved")));
}

void SettingsPage::confirmRemove()
{
    if (!controller_ || (!controller_->hasWallet() && controller_->walletFilesKeptAside() == 0))
        return;
    auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Remove all wallets"),
                                QStringLiteral("Remove every wallet from this PC?"), QMessageBox::NoButton,
                                this);
    QString detail = QStringLiteral("GitWallet deletes wallet.dat (the saved xpubs and settings) and its database of "
                                    "balances, and stops scanning. "
                                    "Seeds were never stored, so nothing changes on the blockchain: adding a seed "
                                    "again shows the same addresses.");
    const QList<wallet::WalletEntry>& wallets = controller_->store().wallets;
    if (std::any_of(wallets.cbegin(), wallets.cend(), [](const wallet::WalletEntry& w) { return w.fromKeys; })) {
        detail += QLatin1Char(' ')
                  + QStringLiteral("Wallets added from public keys have no seed on this PC: to add them back you need "
                                   "those keys, which Public keys on the Wallets page shows now.");
    }
    const int aside = controller_->walletFilesKeptAside();
    if (aside > 0) {
        detail += QLatin1Char(' ')
                  + (aside == 1 ? QStringLiteral("It also deletes the older wallet file kept aside, which may hold "
                                                 "other wallets.")
                                : QStringLiteral("It also deletes the %1 older wallet files kept aside, which may hold "
                                                 "other wallets.")
                                      .arg(aside));
    }
    const int labels = controller_->labelCount();
    if (labels > 0) {
        detail += QLatin1Char(' ')
                  + QStringLiteral("Its %1 labels and notes go too, for good: only this PC has them, and no seed brings them "
                                   "back. Keep a copy first, under Labels and notes here in Settings.")
                        .arg(labels);
    } else if (!controller_->keepingSaved() || controller_->loadingSaved()) {
        // Nothing to count, which is no reason to say nothing: the file goes all the same.
        detail += QLatin1Char(' ')
                  + QStringLiteral("Its database could not be read just now: whatever labels and notes it holds go too, for good.");
    }
    const int waiting = controller_->givenOutWaiting();
    if (waiting > 0) {
        detail += QLatin1Char(' ')
                  + trText(QStringLiteral("%1 of the addresses they gave out have had nothing come in yet. If those seeds are added again, they "
                                          "will be offered to someone else: only this PC knows they were given out."))
                        .arg(waiting);
    }
    const int databases = controller_->databasesKeptAside();
    if (databases > 0) {
        detail += QLatin1Char(' ')
                  + QStringLiteral("It also deletes %1 older databases kept aside, which may hold the only copy of other "
                                   "labels and notes.")
                        .arg(databases);
    }
    if (controller_->hasPassword()) {
        detail += QLatin1Char(' ')
                  + trText(QStringLiteral("The app password goes with them: what you add next is under none until you set one again."));
    }
    box->setInformativeText(detail);
    QPushButton* remove = box->addButton(QStringLiteral("Remove all wallets"), QMessageBox::DestructiveRole);
    remove->setObjectName(QStringLiteral("dangerButton"));
    QPushButton* cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    box->setAttribute(Qt::WA_DeleteOnClose);
    applyDialogChrome(box);
    // open(), not exec(): no nested event loop while the scanners keep running.
    connect(box, &QDialog::finished, this, [this, box, remove] {
        if (box->clickedButton() == remove)
            removeWallet();
    });
    box->open();
}

void SettingsPage::removeWallet()
{
    if (!controller_)
        return;
    QString error;
    const bool hadPassword = controller_->hasPassword();
    if (!controller_->removeAllWallets(&error)) {
        Feedback::show(this, Feedback::Type::Error,
                       error.isEmpty() ? trText(QStringLiteral("Could not remove the wallets")) : error);
        return;
    }
    load(); // walletChanged reloads as well; this keeps the page right either way
    Feedback::show(this, Feedback::Type::Success,
                   hadPassword ? trText(QStringLiteral("All wallets removed from this PC, and the app password with them"))
                               : trText(QStringLiteral("All wallets removed from this PC")));
}

} // namespace app::ui
