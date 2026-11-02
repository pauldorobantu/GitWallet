#include "ui/pages/ReceivePage.h"

#include "app/WalletController.h"
#include "net/PriceFeed.h"
#include "ui/CoinIcon.h"
#include "ui/Feedback.h"
#include "ui/Figures.h"
#include "ui/HoverRowTable.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/QrCode.h"
#include "ui/TableUtil.h"
#include "ui/design/Style.h"
#include "wallet/BulkImport.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>

namespace app::ui {

namespace {

constexpr int kQrSide = 248;      // px on screen, at most; a module stays a whole number of pixels
constexpr int kQrFileScale = 12;  // px a module in a file
constexpr int kListedRows = 200;  // of what was given out, the latest
constexpr int kUpdateMs = 400;    // a scan signals per address
constexpr int kLabelChars = 1024; // as the Addresses page takes them

enum ListColumn { ListAddress, ListLabel, ListPath, ListBalance, ListUsd, ListState, ListColumns };

QString typeName(chain::ScriptType type)
{
    switch (type) {
    case chain::ScriptType::P2WPKH: return trText(QStringLiteral("Native SegWit"));
    case chain::ScriptType::P2SH_P2WPKH: return trText(QStringLiteral("Nested SegWit"));
    case chain::ScriptType::P2PKH: return trText(QStringLiteral("Legacy"));
    case chain::ScriptType::EthAccount: return trText(QStringLiteral("Account"));
    }
    return QString();
}

// What it takes to tell someone to pay here, in the sender's terms.
QString onlyLine(const wallet::NetworkInfo& network)
{
    if (network.tokens.isEmpty())
        return trText(QStringLiteral("Send only %1 on the %2 network to this address. Anything else may be lost."))
            .arg(wallet::assetInfo(network.coin).symbol, network.name);
    QStringList symbols;
    for (const wallet::Asset asset : network.assets)
        symbols << wallet::assetInfo(asset).symbol;
    return trText(QStringLiteral("Send only %1 on the %2 network to this address. Anything else, or another network, may be lost."))
        .arg(symbols.join(QStringLiteral(", ")), network.name);
}

bool validAmount(const QString& text, const wallet::AssetInfo& coin)
{
    static const QRegularExpression shape(QStringLiteral("^[0-9]{1,12}(\\.[0-9]{1,18})?$"));
    if (!shape.match(text).hasMatch())
        return false;
    // Nothing to pay is no amount, and neither is more than there can be.
    const double value = text.toDouble();
    if (value <= 0.0 || (coin.maxSupply > 0.0 && value > coin.maxSupply))
        return false;
    const qsizetype dot = text.indexOf(QLatin1Char('.'));
    return dot < 0 || text.size() - dot - 1 <= coin.decimals;
}

} // namespace

struct ReceivePage::Ui : QObject
{
    using QObject::QObject;

    QWidget* noWallet = nullptr;
    QWidget* body = nullptr;
    QComboBox* wallet = nullptr;
    QComboBox* coin = nullptr;
    QComboBox* type = nullptr;
    QLabel* typeLabel = nullptr;
    QLineEdit* amount = nullptr;
    QLabel* amountLabel = nullptr;
    QLineEdit* label = nullptr;
    QLabel* qr = nullptr;
    QLabel* mark = nullptr;
    QLabel* addressLine = nullptr;
    QLabel* state = nullptr;
    QLabel* only = nullptr;
    QLabel* status = nullptr;
    QLabel* owed = nullptr;
    QPushButton* copy = nullptr;
    QPushButton* save = nullptr;
    QPushButton* next = nullptr;
    HoverRowTableWidget* list = nullptr;
    QLabel* listTitle = nullptr;
    QWidget* filler = nullptr; // keeps the card its own height while there is no list under it
    QTimer* update = nullptr;

    // What is on screen: all of QR, text, Copy and Save go by these.
    QString address;           // as its key gives it; empty: none
    wallet::AddressRow row;    // what tells it apart in the model
    wallet::ReceiveOffer::State offerState = wallet::ReceiveOffer::State::NoKey;
    QString why;               // why there is none
    bool held = false;         // it stays until told otherwise: asked for, or given out just now
    bool labelTyped = false;   // the field holds what someone typed, not what the address is called
    QString labelProblem;      // why what was typed was not kept
    QString lastCopied;
};

ReceivePage::ReceivePage(WalletController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    ui_ = new Ui(this);
    auto* root = new QVBoxLayout(this);
    page::applyPageLayout(root);

    ui_->noWallet = page::makeEmptyState(this, int(icon::Kind::ImportDown), QStringLiteral("No wallet yet"),
                                         QStringLiteral("Add a wallet on the Wallets page, and its addresses to receive on show here."));
    root->addWidget(ui_->noWallet, 1);

    ui_->body = new QWidget(this);
    auto* column = new QVBoxLayout(ui_->body);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(page::kPageSpacing);
    root->addWidget(ui_->body, 1);

    auto* card = new QFrame(ui_->body);
    card->setObjectName(QStringLiteral("cardPanel"));
    auto* row = new QHBoxLayout(card);
    row->setContentsMargins(page::kSectionPadX, page::kSectionPadY, page::kSectionPadX, page::kSectionPadY);
    row->setSpacing(page::kSectionPadX + 8);

    // Dark on white on either theme: that is what phones read.
    ui_->qr = new QLabel(card);
    ui_->qr->setObjectName(QStringLiteral("receiveQr"));
    ui_->qr->setFixedSize(kQrSide, kQrSide);
    ui_->qr->setAlignment(Qt::AlignCenter);
    row->addWidget(ui_->qr, 0, Qt::AlignTop);

    auto* right = new QVBoxLayout;
    right->setSpacing(page::kFormVSpacing);
    auto* form = new QFormLayout;
    page::applyFormLayout(form);
    ui_->wallet = new QComboBox(card);
    ui_->wallet->setObjectName(QStringLiteral("receiveWallet"));
    ui_->coin = new QComboBox(card);
    ui_->coin->setObjectName(QStringLiteral("receiveCoin"));
    ui_->coin->setIconSize(QSize(18, 18));
    ui_->type = new QComboBox(card);
    ui_->type->setObjectName(QStringLiteral("receiveType"));
    ui_->amount = new QLineEdit(card);
    ui_->amount->setObjectName(QStringLiteral("receiveAmount"));
    ui_->amount->setPlaceholderText(trText(QStringLiteral("Optional: goes into the QR code")));
    ui_->amount->setMaxLength(32);
    ui_->label = new QLineEdit(card);
    ui_->label->setObjectName(QStringLiteral("receiveLabel"));
    ui_->label->setPlaceholderText(trText(QStringLiteral("A customer, an order, a till")));
    ui_->label->setToolTip(trText(QStringLiteral("Only this PC knows it. It does not go into the QR code.")));
    ui_->label->setMaxLength(kLabelChars);
    form->addRow(QStringLiteral("Wallet"), ui_->wallet);
    form->addRow(QStringLiteral("Coin"), ui_->coin);
    ui_->typeLabel = new QLabel(QStringLiteral("Address type"), card);
    form->addRow(ui_->typeLabel, ui_->type);
    ui_->amountLabel = new QLabel(QStringLiteral("Amount"), card);
    form->addRow(ui_->amountLabel, ui_->amount);
    form->addRow(QStringLiteral("Label"), ui_->label);
    page::finishForms({form});
    right->addLayout(form);

    auto* addressRow = new QHBoxLayout;
    addressRow->setSpacing(page::kToolbarSpacing);
    ui_->mark = new QLabel(card);
    ui_->mark->setFixedSize(24, 24);
    addressRow->addWidget(ui_->mark, 0, Qt::AlignVCenter);
    ui_->addressLine = new QLabel(card);
    ui_->addressLine->setObjectName(QStringLiteral("receiveAddress"));
    ui_->addressLine->setTextFormat(Qt::PlainText);
    ui_->addressLine->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ui_->addressLine->setWordWrap(true);
    addressRow->addWidget(ui_->addressLine, 1);
    right->addLayout(addressRow);

    ui_->state = new QLabel(card);
    ui_->state->setObjectName(QStringLiteral("receiveState"));
    ui_->state->setTextFormat(Qt::PlainText);
    ui_->state->setWordWrap(true);
    right->addWidget(ui_->state);
    ui_->only = new QLabel(card);
    ui_->only->setTextFormat(Qt::PlainText);
    ui_->only->setWordWrap(true);
    setTone(ui_->only, Tone::Warning);
    right->addWidget(ui_->only);
    ui_->status = new QLabel(card);
    ui_->status->setObjectName(QStringLiteral("receiveStatus"));
    ui_->status->setTextFormat(Qt::PlainText);
    ui_->status->setWordWrap(true);
    setStatus(ui_->status, QString(), Tone::Muted);
    right->addWidget(ui_->status);
    ui_->owed = new QLabel(card);
    ui_->owed->setObjectName(QStringLiteral("receiveOwed"));
    ui_->owed->setTextFormat(Qt::PlainText);
    ui_->owed->setWordWrap(true);
    setStatus(ui_->owed, QString(), Tone::Warning);
    right->addWidget(ui_->owed);
    right->addStretch(1);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(page::kToolbarSpacing);
    buttons->addStretch(1);
    ui_->save = new QPushButton(QStringLiteral("Save QR"), card);
    ui_->copy = new QPushButton(QStringLiteral("Copy address"), card);
    ui_->next = new QPushButton(QStringLiteral("New address"), card);
    ui_->next->setObjectName(QStringLiteral("primaryButton"));
    ui_->next->setToolTip(trText(QStringLiteral("This one counts as given out, and the next unused one comes up")));
    buttons->addWidget(ui_->save);
    buttons->addWidget(ui_->copy);
    buttons->addWidget(ui_->next);
    right->addLayout(buttons);
    row->addLayout(right, 1);
    card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    column->addWidget(card);

    ui_->listTitle = new QLabel(QStringLiteral("Given out"), ui_->body);
    ui_->listTitle->setObjectName(QStringLiteral("sectionTitle"));
    column->addWidget(ui_->listTitle);
    auto* list = new HoverRowTableWidget(ui_->body);
    ui_->list = list;
    list->setObjectName(QStringLiteral("receiveList"));
    list->setColumnCount(ListColumns);
    list->setHorizontalHeaderLabels({QStringLiteral("Address"), QStringLiteral("Label"), QStringLiteral("Path"), QStringLiteral("Balance"),
                                     QStringLiteral("USD"), QStringLiteral("State")});
    page::configureTable(list);
    list->setMouseTracking(true);
    list->setWordWrap(false);
    list->setTextElideMode(Qt::ElideMiddle);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->horizontalHeader()->setStretchLastSection(false);
    list->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    list->horizontalHeader()->setSectionResizeMode(ListAddress, QHeaderView::Stretch);
    list->horizontalHeader()->setHighlightSections(false);
    column->addWidget(list, 1);
    ui_->filler = new QWidget(ui_->body);
    column->addWidget(ui_->filler, 1);

    ui_->update = new QTimer(this);
    ui_->update->setSingleShot(true);
    ui_->update->setInterval(kUpdateMs);
    connect(ui_->update, &QTimer::timeout, this, [this] {
        if (!isVisible())
            return;
        offerNext();
        updateList();
    });
    const auto queue = [this] {
        if (!ui_->update->isActive())
            ui_->update->start();
    };

    connect(ui_->wallet, &QComboBox::currentIndexChanged, this, [this] {
        rebuildCoins();
        chosenChanged();
    });
    connect(ui_->coin, &QComboBox::currentIndexChanged, this, [this] {
        rebuildTypes();
        chosenChanged();
    });
    connect(ui_->type, &QComboBox::currentIndexChanged, this, &ReceivePage::chosenChanged);
    connect(ui_->amount, &QLineEdit::textChanged, this, &ReceivePage::updateShown);
    connect(ui_->label, &QLineEdit::textEdited, this, [this] {
        ui_->labelTyped = true;
        ui_->labelProblem.clear();
    });
    // Of one that has left this screen, or was asked for, a label is kept as soon as it is written.
    connect(ui_->label, &QLineEdit::editingFinished, this, [this] {
        if (ui_->held) {
            keepLabel();
            updateShown();
        }
    });
    connect(ui_->copy, &QPushButton::clicked, this, [this] {
        const QString address = ui_->address;
        if (address.isEmpty())
            return;
        giveOut();
        ui_->lastCopied = address;
        copyTextToClipboard(ui_->copy, address, trText(QStringLiteral("Address copied")));
    });
    connect(ui_->save, &QPushButton::clicked, this, &ReceivePage::saveQr);
    connect(ui_->next, &QPushButton::clicked, this, &ReceivePage::newAddress);
    const auto rowOfCell = [this](int row) {
        // By what tells it apart, and only if that is still what the line says: rows move.
        const QTableWidgetItem* cell = ui_->list->item(row, ListAddress);
        const QList<wallet::AccountKey> keys = ui_->coin->count() > 0 ? controller_->receiveKeys(ui_->wallet->currentData().toUInt(), chosenNetwork()) : QList<wallet::AccountKey>();
        const auto key = std::find_if(keys.cbegin(), keys.cend(), [this](const wallet::AccountKey& k) { return k.type == chosenType(); });
        if (!cell || key == keys.cend())
            return -1;
        const wallet::WalletModel* model = controller_->model();
        const int position = model->find(ui_->wallet->currentData().toUInt(), chosenNetwork(), chosenType(), key->account, 0, cell->data(Qt::UserRole).toUInt());
        return position >= 0 && model->at(position).address == cell->text() ? position : -1;
    };
    connect(list, &QTableWidget::cellClicked, this, [this, rowOfCell](int row, int) {
        const int position = rowOfCell(row);
        if (position >= 0)
            showAddress(controller_->model()->at(position));
        else
            updateList();
    });
    connect(list, &QTableWidget::cellDoubleClicked, this, [this, rowOfCell](int row, int) {
        const int position = rowOfCell(row);
        if (position >= 0)
            emit openAddressRequested(controller_->model()->at(position));
        else
            updateList();
    });
    connect(LanguageManager::instance(), &LanguageManager::changed, this, [this] {
        rebuildTypes();
        updateShown();
        updateList();
    });
    if (controller_) {
        connect(controller_, &WalletController::walletChanged, this, [this] {
            rebuildWallets();
            // What stays on screen stays only while its wallet does.
            if (ui_->held && !controller_->store().find(ui_->row.wallet)) {
                ui_->held = false;
                resetTyped();
            }
            offerNext();
            updateList();
        });
        connect(controller_, &WalletController::totalsChanged, this, queue);
        connect(controller_, &WalletController::statusChanged, this, queue);
        connect(controller_, &WalletController::labelsChanged, this, [this, queue] {
            loadLabel();
            queue();
        });
        connect(controller_, &WalletController::givenOutChanged, this, queue);
        if (wallet::WalletModel* model = controller_->model()) {
            connect(model, &wallet::WalletModel::rowChanged, this, queue);
            connect(model, &wallet::WalletModel::rowsAppended, this, queue);
            connect(model, &wallet::WalletModel::cleared, this, queue);
            connect(model, &wallet::WalletModel::rowsDropped, this, queue);
        }
    }
    rebuildWallets();
    offerNext();
    updateList();
}

void ReceivePage::rebuildWallets()
{
    const quint32 chosen = ui_->wallet->currentData().toUInt();
    const QList<wallet::WalletEntry> wallets = controller_ ? controller_->store().wallets : QList<wallet::WalletEntry>();
    {
        const QSignalBlocker quiet(ui_->wallet);
        ui_->wallet->clear();
        for (const wallet::WalletEntry& entry : wallets)
            ui_->wallet->addItem(entry.label, entry.id);
        const int keep = ui_->wallet->findData(chosen);
        ui_->wallet->setCurrentIndex(keep >= 0 ? keep : 0);
        ui_->wallet->setProperty("i18nOrig_items", QVariant());
    }
    ui_->noWallet->setVisible(wallets.isEmpty());
    ui_->body->setVisible(!wallets.isEmpty());
    rebuildCoins();
}

void ReceivePage::rebuildCoins()
{
    const int chosen = ui_->coin->currentData().toInt();
    const bool hadChoice = ui_->coin->count() > 0;
    const quint32 walletId = ui_->wallet->currentData().toUInt();
    {
        const QSignalBlocker quiet(ui_->coin);
        ui_->coin->clear();
        for (const wallet::AssetInfo& info : wallet::assets()) {
            if (!controller_ || controller_->receiveKeys(walletId, info.network).isEmpty())
                continue;
            const QString name = info.standard.isEmpty() ? QStringLiteral("%1 (%2)").arg(info.name, info.symbol)
                                                         : QStringLiteral("%1 (%2, %3)").arg(info.name, info.symbol, info.standard);
            ui_->coin->addItem(coinIcon(info, 18), name, int(info.asset));
        }
        const int keep = hadChoice ? ui_->coin->findData(chosen) : 0;
        ui_->coin->setCurrentIndex(keep >= 0 ? keep : 0);
        ui_->coin->setProperty("i18nOrig_items", QVariant());
    }
    rebuildTypes();
}

void ReceivePage::rebuildTypes()
{
    const int chosen = ui_->type->currentData().toInt();
    const bool hadChoice = ui_->type->count() > 0;
    const quint32 walletId = ui_->wallet->currentData().toUInt();
    const QList<wallet::AccountKey> keys = controller_ && ui_->coin->count() > 0 ? controller_->receiveKeys(walletId, chosenNetwork()) : QList<wallet::AccountKey>();
    {
        const QSignalBlocker quiet(ui_->type);
        ui_->type->clear();
        for (const wallet::AccountKey& key : keys)
            ui_->type->addItem(typeName(key.type), int(key.type));
        const int keep = hadChoice ? ui_->type->findData(chosen) : 0;
        ui_->type->setCurrentIndex(keep >= 0 ? keep : 0);
        ui_->type->setProperty("i18nOrig_items", QVariant());
    }
    // One kind of address: nothing to choose, as on Ethereum.
    const bool several = keys.size() > 1;
    ui_->type->setVisible(several);
    ui_->typeLabel->setVisible(several);
    // A URI for an amount on Ethereum is not one wallets agree on, least of all for a token.
    const bool amounts = ui_->coin->count() > 0 && wallet::networkInfo(chosenNetwork()).family == wallet::Family::Utxo;
    ui_->amount->setVisible(amounts);
    ui_->amountLabel->setVisible(amounts);
    if (!amounts)
        ui_->amount->clear();
}

wallet::Network ReceivePage::chosenNetwork() const
{
    return wallet::assetInfo(wallet::Asset(quint16(ui_->coin->currentData().toInt()))).network;
}

chain::ScriptType ReceivePage::chosenType() const
{
    return static_cast<chain::ScriptType>(ui_->type->currentData().toInt());
}

void ReceivePage::chosenChanged()
{
    ui_->held = false;
    ui_->address.clear();
    resetTyped();
    offerNext();
    updateList();
}

void ReceivePage::showFor(quint32 walletId, std::optional<wallet::Asset> asset)
{
    {
        const QSignalBlocker quiet(ui_->wallet);
        const int index = walletId != 0 ? ui_->wallet->findData(walletId) : -1;
        if (index >= 0)
            ui_->wallet->setCurrentIndex(index);
    }
    rebuildCoins();
    {
        const QSignalBlocker quiet(ui_->coin);
        const int index = asset ? ui_->coin->findData(int(*asset)) : -1;
        if (index >= 0)
            ui_->coin->setCurrentIndex(index);
    }
    rebuildTypes();
    chosenChanged();
}

void ReceivePage::showAddress(const wallet::AddressRow& row)
{
    {
        const QSignalBlocker quietWallet(ui_->wallet);
        const int index = ui_->wallet->findData(row.wallet);
        if (index >= 0)
            ui_->wallet->setCurrentIndex(index);
    }
    rebuildCoins();
    {
        const QSignalBlocker quietCoin(ui_->coin);
        const int index = ui_->coin->findData(int(wallet::networkInfo(row.network).coin));
        if (index >= 0)
            ui_->coin->setCurrentIndex(index);
    }
    rebuildTypes();
    {
        const QSignalBlocker quietType(ui_->type);
        const int index = ui_->type->findData(int(row.type));
        if (index >= 0)
            ui_->type->setCurrentIndex(index);
    }
    ui_->held = true;
    ui_->row = row;
    ui_->address = controller_->checkedAddress(row);
    ui_->offerState = ui_->address.isEmpty() ? wallet::ReceiveOffer::State::Failed : wallet::ReceiveOffer::State::Ready;
    ui_->why = ui_->address.isEmpty() ? QStringLiteral("What is kept for this address is not what the key of this wallet gives, so it is not offered.") : QString();
    resetTyped();
    updateShown();
    updateList();
}

void ReceivePage::resetTyped()
{
    ui_->amount->clear();
    ui_->labelTyped = false;
    ui_->labelProblem.clear();
    loadLabel();
}

void ReceivePage::loadLabel()
{
    if (ui_->labelTyped)
        return;
    const QString kept = controller_ && !ui_->address.isEmpty() ? controller_->addressLabel(ui_->address) : QString();
    if (ui_->label->text() != kept)
        ui_->label->setText(kept);
}

void ReceivePage::offerNext()
{
    if (ui_->held) {
        updateShown();
        return;
    }
    if (!controller_ || ui_->coin->count() == 0 || ui_->type->count() == 0) {
        ui_->address.clear();
        ui_->offerState = wallet::ReceiveOffer::State::NoKey;
        ui_->why.clear();
        loadLabel();
        updateShown();
        return;
    }
    const quint32 walletId = ui_->wallet->currentData().toUInt();
    const wallet::ReceiveOffer next = controller_->nextReceive(walletId, chosenNetwork(), chosenType());
    ui_->offerState = next.state;
    ui_->why = next.why;
    if (next.state == wallet::ReceiveOffer::State::Ready) {
        const wallet::WalletModel* model = controller_->model();
        ui_->row = model->at(model->find(walletId, chosenNetwork(), chosenType(), next.account, 0, next.index));
    }
    // What was typed is for the payment, so it goes along if the address moves on by itself.
    if (next.address != ui_->address) {
        ui_->address = next.address;
        ui_->labelProblem.clear();
        loadLabel();
    }
    updateShown();
}

int ReceivePage::shownPosition() const
{
    const wallet::AddressRow& row = ui_->row;
    return ui_->address.isEmpty() || !controller_ ? -1 : controller_->model()->find(row.wallet, row.network, row.type, row.account, row.change, row.index);
}

QString ReceivePage::payload() const
{
    if (ui_->address.isEmpty())
        return {};
    const QString amount = ui_->amount->text().trimmed();
    const wallet::NetworkInfo& network = wallet::networkInfo(ui_->row.network);
    const wallet::AssetInfo& coin = wallet::assetInfo(network.coin);
    if (amount.isEmpty() || network.family != wallet::Family::Utxo || !validAmount(amount, coin))
        return ui_->address;
    // BIP 21. The label stays out: whoever scans it has no business reading the name given here.
    return QStringLiteral("%1:%2?amount=%3").arg(network.id, ui_->address, amount);
}

QString ReceivePage::shownAddress() const
{
    return ui_->address;
}

QString ReceivePage::shownPayload() const
{
    return payload();
}

QString ReceivePage::lastCopied() const
{
    return ui_->lastCopied;
}

void ReceivePage::updateShown()
{
    const bool ready = !ui_->address.isEmpty();
    ui_->copy->setEnabled(ready);
    ui_->save->setEnabled(ready);
    ui_->next->setEnabled(ready);
    ui_->label->setEnabled(ready);
    ui_->addressLine->setText(ui_->address);
    ui_->mark->setVisible(ready);
    ui_->only->setVisible(ready);
    if (ready) {
        const wallet::NetworkInfo& network = wallet::networkInfo(ui_->row.network);
        ui_->mark->setPixmap(coinIcon(wallet::assetInfo(network.coin), 24).pixmap(QSize(24, 24)));
        ui_->only->setText(onlyLine(network));
    }

    const QString text = payload();
    if (text.isEmpty()) {
        ui_->qr->clear();
    } else {
        const QrCode code = QrCode::encode(text.toUtf8());
        const int modules = code.size() + 8;
        const qreal ratio = devicePixelRatioF();
        const int scale = qMax(1, int(kQrSide * ratio) / modules);
        QImage image = code.toImage(scale);
        image.setDevicePixelRatio(ratio);
        ui_->qr->setPixmap(QPixmap::fromImage(image));
    }

    // What typed input came to, and whether what was given out is in the file.
    const QString amount = ui_->amount->text().trimmed();
    QString problem = ui_->labelProblem;
    if (problem.isEmpty() && ready && !amount.isEmpty() && !validAmount(amount, wallet::assetInfo(wallet::networkInfo(ui_->row.network).coin)))
        problem = trText(QStringLiteral("That amount cannot go into the QR code: write it like 0.015"));
    setStatus(ui_->status, problem, Tone::Warning);
    setStatus(ui_->owed, controller_ && controller_->givenOutOwed() ? trText(QStringLiteral("An address given out could not be written down yet, so another start may offer it again. This is tried again every half a minute.")) : QString(), Tone::Warning);

    switch (ui_->offerState) {
    case wallet::ReceiveOffer::State::Checking:
        setStatus(ui_->state, trText(QStringLiteral("Looking for the next unused address: a server has to answer for it first.")), Tone::Muted);
        return;
    case wallet::ReceiveOffer::State::NoKey:
        setStatus(ui_->state, ui_->coin->count() == 0 ? trText(QStringLiteral("This wallet has no key to receive on.")) : trText(QStringLiteral("This wallet has no such key.")), Tone::Warning);
        return;
    case wallet::ReceiveOffer::State::Failed:
        setStatus(ui_->state, trText(ui_->why), Tone::Error);
        return;
    case wallet::ReceiveOffer::State::Ready:
        break;
    }

    // What is known of it, and no more than that.
    const int at = shownPosition();
    QString line = ui_->row.path();
    Tone tone = Tone::Muted;
    if (at >= 0) {
        const wallet::AddressRow& row = controller_->model()->at(at);
        const bool given = controller_->isGivenOut(row);
        const auto say = [&line](const QString& what) { line += QStringLiteral(" · ") + what; };
        if (row.state() == wallet::AddressState::Funded) {
            QStringList held;
            row.forEachBalance([&held, &row](wallet::Asset asset, const chain::Amount& value) {
                const wallet::AssetInfo& info = wallet::assetInfo(asset);
                const QString amountText = figures::formatAmount(value, info) + QLatin1Char(' ') + info.symbol;
                held << (row.checked ? amountText : figures::atLeast(amountText));
            });
            say(trText(QStringLiteral("Holds %1")).arg(held.join(QStringLiteral(", "))));
            tone = Tone::Success;
        } else if (row.used) {
            say(row.checked ? trText(QStringLiteral("Has been used, and holds nothing now")) : trText(QStringLiteral("Has been used; what it holds is being read")));
            tone = row.checked ? Tone::Muted : Tone::Accent;
        } else if (!row.checked) {
            say(given ? trText(QStringLiteral("Given out; no server has answered for it yet")) : trText(QStringLiteral("No server has answered for it yet")));
        } else if (controller_->showingSaved()) {
            say(given ? trText(QStringLiteral("Given out; nothing had come in as far as was saved, being checked")) : trText(QStringLiteral("Unused as far as was saved, being checked")));
        } else {
            say(given ? trText(QStringLiteral("Given out, nothing has come in yet")) : trText(QStringLiteral("Unused")));
            tone = given ? Tone::Accent : Tone::Muted;
        }
    }
    setStatus(ui_->state, line, tone);
}

void ReceivePage::updateList()
{
    if (!controller_ || ui_->coin->count() == 0 || ui_->type->count() == 0) {
        ui_->list->setRowCount(0);
        ui_->list->hide();
        ui_->listTitle->hide();
        ui_->filler->show();
        return;
    }
    const wallet::WalletModel* model = controller_->model();
    const QList<int> rows = controller_->givenOutRows(ui_->wallet->currentData().toUInt(), chosenNetwork(), chosenType(), kListedRows);
    ui_->list->setVisible(!rows.isEmpty());
    ui_->listTitle->setVisible(!rows.isEmpty());
    ui_->filler->setVisible(rows.isEmpty());
    ui_->list->setRowCount(int(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        const wallet::AddressRow& row = model->at(rows.at(i));
        const auto put = [&](int column, const QString& text, Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter) {
            QTableWidgetItem* cell = ui_->list->item(i, column);
            if (!cell) {
                cell = makeCell(text);
                ui_->list->setItem(i, column, cell);
            } else if (cell->text() != text) {
                cell->setText(text);
            }
            cell->setTextAlignment(alignment);
            return cell;
        };
        put(ListAddress, row.address)->setData(Qt::UserRole, row.index);
        put(ListLabel, controller_->addressLabel(row.address));
        put(ListPath, row.path());
        QStringList held;
        double usd = 0.0;
        bool priced = true;
        try {
            row.forEachBalance([&](wallet::Asset asset, const chain::Amount& value) {
                const wallet::AssetInfo& info = wallet::assetInfo(asset);
                held << figures::formatAmount(value, info) + QLatin1Char(' ') + info.symbol;
                const double price = controller_->prices()->usd(asset);
                priced = priced && price > 0.0;
                usd += value.toDouble(info.decimals) * price;
            });
        } catch (const std::exception&) {
            priced = false;
        }
        const Qt::Alignment numbers = Qt::AlignRight | Qt::AlignVCenter;
        put(ListBalance, held.isEmpty() ? (row.checked ? QStringLiteral("0") : figures::dash()) : held.join(QStringLiteral(" · ")), numbers);
        put(ListUsd, row.checked && priced ? figures::formatUsd(usd) : figures::dash(), numbers);
        // No more than is known of it: a figure that was only saved says so.
        QString state;
        Tone tone = Tone::Muted;
        if (!row.checked) {
            state = trText(QStringLiteral("Checking…"));
        } else if (controller_->showingSaved()) {
            state = trText(QStringLiteral("Saved, being checked"));
        } else if (row.state() == wallet::AddressState::Funded) {
            state = trText(QStringLiteral("Received"));
            tone = Tone::Success;
        } else if (row.used) {
            state = trText(QStringLiteral("Used"));
        } else {
            state = trText(QStringLiteral("Waiting"));
        }
        put(ListState, state)->setForeground(toneColor(tone));
    }
}

bool ReceivePage::keepLabel()
{
    if (!ui_->labelTyped || ui_->address.isEmpty())
        return true;
    const QString wanted = ui_->label->text().trimmed();
    QString error;
    if (wanted == controller_->addressLabel(ui_->address) || controller_->setAddressLabel(ui_->address, wanted, &error)) {
        ui_->labelTyped = false;
        ui_->labelProblem.clear();
        return true;
    }
    ui_->labelProblem = error;
    // Not kept on screen either, if that is why it was refused.
    if (wallet::looksLikeSecret(wanted)) {
        ui_->label->clear();
        ui_->labelTyped = false;
    }
    return false;
}

bool ReceivePage::giveOut()
{
    if (ui_->address.isEmpty())
        return true;
    const bool labelled = keepLabel();
    // As it stands now: one that has had a payment meanwhile has no need of a mark.
    const int at = shownPosition();
    if (at >= 0) {
        QString error;
        controller_->markGivenOut(controller_->model()->at(at), &error); // what fails stays on screen, and is tried again
    }
    ui_->held = true;
    updateShown();
    updateList();
    return labelled;
}

void ReceivePage::newAddress()
{
    // A label refused stays in its field, with why: moving on would lose it.
    if (!giveOut())
        return;
    ui_->held = false;
    ui_->address.clear();
    resetTyped();
    offerNext();
    updateList();
}

bool ReceivePage::saveQrAs(const QString& path)
{
    const QString text = payload();
    if (text.isEmpty() || !QrCode::encode(text.toUtf8()).toImage(kQrFileScale).save(path, "PNG"))
        return false;
    giveOut();
    return true;
}

void ReceivePage::saveQr()
{
    if (ui_->address.isEmpty())
        return;
    // Nothing moves what is on screen while the file is being chosen: it is what goes in the file.
    const bool wasHeld = ui_->held;
    ui_->held = true;
    const QString path = QFileDialog::getSaveFileName(this, trText(QStringLiteral("Save QR")),
                                                      QStringLiteral("receive-%1.png").arg(ui_->address.left(12)),
                                                      QStringLiteral("PNG (*.png)"));
    if (path.isEmpty()) {
        ui_->held = wasHeld;
        offerNext();
        return;
    }
    if (saveQrAs(path)) {
        Feedback::show(ui_->save, Feedback::Type::Success, trText(QStringLiteral("QR saved")));
    } else {
        ui_->held = wasHeld;
        offerNext();
        Feedback::show(ui_->save, Feedback::Type::Error, trText(QStringLiteral("Could not write the file")));
    }
}

} // namespace app::ui
