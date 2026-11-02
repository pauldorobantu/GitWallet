#include "ui/pages/SettingsPage.h"

#include "app/WalletController.h"
#include "net/ElectrumClient.h"
#include "ui/DialogChrome.h"
#include "ui/Feedback.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/design/Style.h"
#include "wallet/Types.h"
#include "wallet/WalletStore.h"

#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
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
    ui_->scanForm->addRow(QStringLiteral("Bitcoin receive gap"), ui_->receiveGap);
    ui_->scanForm->addRow(QStringLiteral("Bitcoin change gap"), ui_->changeGap);
    ui_->scanForm->addRow(QStringLiteral("Ethereum gap"), ui_->ethGap);
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
        });
        connect(controller_, &WalletController::statusChanged, this, &SettingsPage::updateInUse);
    }
    // The captions are runtime text (trText) that the language walk cannot re-template.
    connect(LanguageManager::instance(), &LanguageManager::changed, this, &SettingsPage::updateInUse);
    load();
}

void SettingsPage::load()
{
    ui_->fill(controller_ ? controller_->store().settings : wallet::ScanSettings());
    rebuildWallet();
    updateInUse();
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
        text = left.atMost ? QStringLiteral("These gaps can leave out up to %1 addresses that have been used.").arg(left.used)
               : left.used == 1
                   ? QStringLiteral("These gaps leave out 1 address that has been used.")
                   : QStringLiteral("These gaps leave out %1 addresses that have been used.").arg(left.used);
        detail = QStringLiteral("Holding a balance now: %1 of %2. GitWallet would stop showing and watching these "
                                "addresses, and their balances would leave the totals, until the gaps change "
                                "again. Nothing changes on the blockchain.")
                     .arg(left.funded)
                     .arg(left.used);
    } else if (left.overLimit > 0) {
        text = QStringLiteral("These gaps ask for about %1 more addresses than GitWallet follows.").arg(left.overLimit);
        detail = QStringLiteral("The scan would stop at the limit, so no total would read as whole, until the gaps are "
                                "lowered or wallets removed. Nothing changes on the blockchain.");
    } else {
        text = QStringLiteral("Not every address has answered, so what smaller gaps leave out is not known.");
        detail = QStringLiteral("GitWallet could stop showing and watching addresses it has not read, and their "
                                "balances would not count in the totals, until the gaps are raised again. Nothing "
                                "changes on the blockchain.");
    }
    if (left.atMost)
        detail += QLatin1Char(' ') + QStringLiteral("A scan stops at a limit that these gaps go past.");
    if (left.used > 0 && unknown)
        detail += QLatin1Char(' ') + QStringLiteral("Not every address has answered, so there may be more.");
    auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Changing the gaps"), text, QMessageBox::NoButton, this);
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
    if (!controller_->removeAllWallets(&error)) {
        Feedback::show(this, Feedback::Type::Error,
                       error.isEmpty() ? trText(QStringLiteral("Could not remove the wallets")) : error);
        return;
    }
    load(); // walletChanged reloads as well; this keeps the page right either way
    Feedback::show(this, Feedback::Type::Success,
                   trText(QStringLiteral("All wallets removed from this PC")));
}

} // namespace app::ui
