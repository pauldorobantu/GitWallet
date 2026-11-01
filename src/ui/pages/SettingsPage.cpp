#include "ui/pages/SettingsPage.h"

#include "app/WalletController.h"
#include "chain/Address.h"
#include "net/ElectrumClient.h"
#include "net/EthRpc.h"
#include "ui/DialogChrome.h"
#include "ui/Feedback.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/TableUtil.h"
#include "ui/ThemeManager.h"
#include "ui/design/Style.h"
#include "wallet/Types.h"
#include "wallet/WalletStore.h"

#include <QAction>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
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

#include <exception>

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

QStringList electrumDefaults()
{
    QStringList out;
    for (const net::ElectrumServer& server : net::defaultElectrumServers())
        out << server.toString();
    return out;
}

// "Native SegWit (bech32)"; accounts past the first are numbered.
QString accountLabel(const wallet::AccountKey& key)
{
    QString label = QString::fromLatin1(chain::scriptTypeName(key.type));
    if (key.account > 0)
        label += QStringLiteral(", account %1").arg(key.account);
    return label;
}

QString accountPath(const wallet::AccountKey& key)
{
    try {
        return QString::fromStdString(chain::accountRootPath(key.type, key.account));
    } catch (const std::exception&) {
        return QString();
    }
}

QIcon copyIcon()
{
    return makeIcon(icon::Kind::Copy, page::accent(), 16);
}

// A read-only xpub with a copy button at its end (a line-edit action, as
// the search field carries its magnifier).
QLineEdit* makeXpubField(QWidget* parent, const wallet::AccountKey& key)
{
    auto* edit = new QLineEdit(key.xpub, parent);
    edit->setReadOnly(true);
    edit->setCursorPosition(0); // the prefix (xpub / ypub / zpub) stays visible
    edit->setToolTip(QStringLiteral("%1\n%2").arg(accountPath(key), key.xpub));
    QAction* copy = edit->addAction(copyIcon(), QLineEdit::TrailingPosition);
    copy->setToolTip(QStringLiteral("Copy"));
    const QString xpub = key.xpub;
    QObject::connect(copy, &QAction::triggered, edit, [edit, xpub] { copyTextToClipboard(edit, xpub); });
    QObject::connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed, copy,
                     [copy] { copy->setIcon(copyIcon()); });
    return edit;
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
    QFormLayout* walletForm = nullptr;
    QSpinBox* receiveGap = nullptr;
    QSpinBox* changeGap = nullptr;
    QSpinBox* ethGap = nullptr;
    QSpinBox* ethRefresh = nullptr;
    QPlainTextEdit* electrum = nullptr;
    QPlainTextEdit* ethRpc = nullptr;
    QLabel* electrumInUse = nullptr;
    QLabel* ethInUse = nullptr;
    QFrame* walletCard = nullptr;
    QLabel* noWallet = nullptr;
    QWidget* removeRow = nullptr;
    QPushButton* remove = nullptr;

    void fill(const wallet::ScanSettings& s) const
    {
        receiveGap->setValue(s.btcReceiveGap);
        changeGap->setValue(s.btcChangeGap);
        ethGap->setValue(s.ethGap);
        ethRefresh->setValue(s.ethRefreshSeconds);
        electrum->setPlainText(s.electrumServers.join(QLatin1Char('\n')));
        ethRpc->setPlainText(s.ethEndpoints.join(QLatin1Char('\n')));
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
                               QStringLiteral("How often every known Ethereum address is read again"));
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
    ui_->electrum = makeListEdit(network.card, QStringLiteral("Built-in servers (host:port):"),
                                 electrumDefaults());
    ui_->electrumInUse = makeCaption(network.card);
    ui_->networkForm->addRow(QStringLiteral("Electrum servers"),
                             withCaption(network.card, ui_->electrum, ui_->electrumInUse));
    ui_->ethRpc = makeListEdit(network.card, QStringLiteral("Built-in endpoints (https:// URLs):"),
                               net::defaultEthEndpoints());
    ui_->ethInUse = makeCaption(network.card);
    ui_->networkForm->addRow(QStringLiteral("Ethereum RPC URLs"),
                             withCaption(network.card, ui_->ethRpc, ui_->ethInUse));
    column->addWidget(network.card);

    const page::Section walletSection = page::makeSection(
        content, QStringLiteral("Wallet"),
        QStringLiteral("The account public keys (xpubs) GitWallet watches. They reveal every address "
                       "and balance but cannot spend."));
    ui_->walletCard = walletSection.card;
    ui_->noWallet = new QLabel(QStringLiteral("No wallet on this PC yet. Import a seed on the Overview page."),
                               walletSection.card);
    setTone(ui_->noWallet, Tone::Muted);
    walletSection.body->addWidget(ui_->noWallet);
    ui_->walletForm = page::addSectionForm(walletSection);
    ui_->removeRow = new QWidget(walletSection.card);
    auto* removeLayout = new QHBoxLayout(ui_->removeRow);
    removeLayout->setContentsMargins(0, 0, 0, 0);
    removeLayout->setSpacing(page::kFormHSpacing);
    ui_->remove = new QPushButton(QStringLiteral("Remove wallet from this PC"), ui_->removeRow);
    ui_->remove->setObjectName(QStringLiteral("dangerButton"));
    auto* removeNote = new QLabel(QStringLiteral("Deletes the saved xpubs and settings. The seed is never "
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
        connect(controller_, &WalletController::walletChanged, this, &SettingsPage::load);
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
    QFormLayout* form = ui_->walletForm;
    while (form->rowCount() > 0)
        form->removeRow(0); // deletes the row's label and field
    const bool hasWallet = controller_ && controller_->hasWallet();
    if (hasWallet) {
        for (const wallet::AccountKey& key : controller_->store().accounts)
            form->addRow(accountLabel(key), makeXpubField(ui_->walletCard, key));
    }
    ui_->noWallet->setVisible(!hasWallet);
    ui_->removeRow->setVisible(hasWallet);
    // Labels of every card share one column, also after the rows changed.
    page::finishForms({ui_->scanForm, ui_->networkForm, ui_->walletForm});
}

void SettingsPage::updateInUse()
{
    const bool active = controller_ && controller_->hasWallet();
    const QString server = active ? controller_->electrumServer() : QString();
    const QString endpoint = active ? controller_->ethEndpoint() : QString();
    ui_->electrumInUse->setText(server.isEmpty() ? trText(QStringLiteral("No server connected"))
                                                 : trText(QStringLiteral("In use: %1")).arg(server));
    ui_->ethInUse->setText(endpoint.isEmpty() ? trText(QStringLiteral("No endpoint in use"))
                                              : trText(QStringLiteral("In use: %1")).arg(endpoint));
}

void SettingsPage::resetToDefaults()
{
    ui_->fill(wallet::ScanSettings());
    Feedback::show(this, Feedback::Type::Info,
                   trText(QStringLiteral("Defaults filled in; Save applies them")));
}

void SettingsPage::save()
{
    if (!controller_)
        return;
    QStringList servers;
    QStringList endpoints;
    int bad = -1;
    if (!readLines(ui_->electrum, acceptElectrum, &servers, &bad)) {
        selectLine(ui_->electrum, bad);
        Feedback::show(this, Feedback::Type::Error,
                       trText(QStringLiteral("Electrum servers, line %1: use host:port")).arg(bad + 1));
        return;
    }
    if (!readLines(ui_->ethRpc, acceptRpc, &endpoints, &bad)) {
        selectLine(ui_->ethRpc, bad);
        Feedback::show(this, Feedback::Type::Error,
                       trText(QStringLiteral("Ethereum RPC URLs, line %1: use an https:// URL"))
                           .arg(bad + 1));
        return;
    }

    // Start from the stored settings so fields this page does not show survive.
    wallet::ScanSettings settings = controller_->store().settings;
    settings.btcReceiveGap = ui_->receiveGap->value();
    settings.btcChangeGap = ui_->changeGap->value();
    settings.ethGap = ui_->ethGap->value();
    settings.ethRefreshSeconds = ui_->ethRefresh->value();
    settings.electrumServers = servers;
    settings.ethEndpoints = endpoints;

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
    if (!controller_ || !controller_->hasWallet())
        return;
    auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Remove wallet"),
                                QStringLiteral("Remove the wallet from this PC?"), QMessageBox::NoButton,
                                this);
    box->setInformativeText(
        QStringLiteral("GitWallet deletes wallet.dat (the saved xpubs and settings) and stops scanning. "
                       "The seed was never stored, so nothing changes on the blockchain: importing the "
                       "seed again shows the same addresses."));
    QPushButton* remove = box->addButton(QStringLiteral("Remove wallet"), QMessageBox::DestructiveRole);
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
    if (!controller_->removeWallet(&error)) {
        Feedback::show(this, Feedback::Type::Error,
                       error.isEmpty() ? trText(QStringLiteral("Could not remove the wallet")) : error);
        return;
    }
    load(); // walletChanged reloads as well; this keeps the page right either way
    Feedback::show(this, Feedback::Type::Success,
                   trText(QStringLiteral("Wallet removed from this PC")));
}

} // namespace app::ui
