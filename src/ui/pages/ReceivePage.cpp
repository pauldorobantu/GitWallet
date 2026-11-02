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

#include <exception>

namespace app::ui {

namespace {

constexpr int kQrSide = 248;      // px on screen, at most; a module stays a whole number of pixels
constexpr int kQrFileScale = 12;  // px a module in a file
constexpr int kListedRows = 200;  // of what was given out, the latest
constexpr int kUpdateMs = 400;    // a scan signals per address
constexpr int kLabelChars = 100;

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
    QLabel* address = nullptr;
    QLabel* state = nullptr;
    QLabel* only = nullptr;
    QLabel* status = nullptr;
    QPushButton* copy = nullptr;
    QPushButton* save = nullptr;
    QPushButton* next = nullptr;
    HoverRowTableWidget* list = nullptr;
    QLabel* listTitle = nullptr;
    QWidget* filler = nullptr; // keeps the card its own height while there is no list under it
    QTimer* update = nullptr;

    // What is on screen. Either the offer, which moves on by itself until it was given out, or
    // one address asked for.
    wallet::ReceiveOffer offer;
    bool given = false;     // it left this screen
    bool asked = false;     // one address, asked for: nothing moves it on
    wallet::AddressRow row; // that address
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
    ui_->address = new QLabel(card);
    ui_->address->setObjectName(QStringLiteral("receiveAddress"));
    ui_->address->setTextFormat(Qt::PlainText);
    ui_->address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ui_->address->setWordWrap(true);
    addressRow->addWidget(ui_->address, 1);
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
        // Until it left this screen it may have been used elsewhere, or only now answered for.
        if (!ui_->asked && !ui_->given)
            offerNext();
        else
            updateShown();
        updateList();
    });
    const auto queue = [this] {
        if (!ui_->update->isActive())
            ui_->update->start();
    };

    connect(ui_->wallet, &QComboBox::currentIndexChanged, this, [this] {
        rebuildCoins();
        ui_->asked = false;
        offerNext();
        updateList();
    });
    connect(ui_->coin, &QComboBox::currentIndexChanged, this, [this] {
        rebuildTypes();
        ui_->asked = false;
        offerNext();
        updateList();
    });
    connect(ui_->type, &QComboBox::currentIndexChanged, this, [this] {
        ui_->asked = false;
        offerNext();
        updateList();
    });
    connect(ui_->amount, &QLineEdit::textChanged, this, &ReceivePage::updateShown);
    // Of one that has left this screen, or was asked for, a label is kept as soon as it is written.
    connect(ui_->label, &QLineEdit::editingFinished, this, [this] {
        if (ui_->given || ui_->asked)
            keepLabel();
    });
    connect(ui_->copy, &QPushButton::clicked, this, [this] {
        const QString address = ui_->address->text();
        if (address.isEmpty())
            return;
        giveOut();
        copyTextToClipboard(ui_->copy, address, trText(QStringLiteral("Address copied")));
    });
    connect(ui_->save, &QPushButton::clicked, this, &ReceivePage::saveQr);
    connect(ui_->next, &QPushButton::clicked, this, &ReceivePage::newAddress);
    connect(list, &QTableWidget::cellClicked, this, [this](int row, int) {
        const int position = ui_->list->item(row, ListAddress)->data(Qt::UserRole).toInt();
        const wallet::WalletModel* model = controller_->model();
        if (position >= 0 && position < model->size())
            showAddress(model->at(position));
    });
    connect(list, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const int position = ui_->list->item(row, ListAddress)->data(Qt::UserRole).toInt();
        const wallet::WalletModel* model = controller_->model();
        if (position >= 0 && position < model->size())
            emit openAddressRequested(model->at(position));
    });
    connect(LanguageManager::instance(), &LanguageManager::changed, this, [this] {
        rebuildTypes();
        updateShown();
        updateList();
    });
    if (controller_) {
        connect(controller_, &WalletController::walletChanged, this, [this] {
            rebuildWallets();
            offerNext();
            updateList();
        });
        connect(controller_, &WalletController::totalsChanged, this, queue);
        connect(controller_, &WalletController::statusChanged, this, queue);
        connect(controller_, &WalletController::labelsChanged, this, queue);
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

void ReceivePage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // What was given out has gone: coming back, the next one is what is wanted.
    if (ui_->given && !ui_->asked)
        ui_->label->clear();
    if (!ui_->asked)
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

void ReceivePage::showFor(quint32 walletId, std::optional<wallet::Asset> asset)
{
    if (walletId != 0) {
        const int index = ui_->wallet->findData(walletId);
        if (index >= 0)
            ui_->wallet->setCurrentIndex(index);
    }
    if (asset) {
        const int index = ui_->coin->findData(int(*asset));
        if (index >= 0)
            ui_->coin->setCurrentIndex(index);
    }
    ui_->asked = false;
    offerNext();
    updateList();
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
    ui_->asked = true;
    ui_->row = row;
    ui_->given = true;
    ui_->offer = {};
    ui_->offer.address = controller_->checkedAddress(row);
    ui_->offer.state = ui_->offer.address.isEmpty() ? wallet::ReceiveOffer::State::Failed : wallet::ReceiveOffer::State::Ready;
    if (ui_->offer.address.isEmpty())
        ui_->offer.why = QStringLiteral("What is kept for this address is not what the key of this wallet gives, so it is not offered.");
    ui_->offer.path = row.path();
    ui_->offer.account = row.account;
    ui_->offer.index = row.index;
    ui_->label->setText(controller_->addressLabel(row.address));
    setStatus(ui_->status, QString(), Tone::Muted);
    updateShown();
    updateList();
}

void ReceivePage::offerNext()
{
    if (!controller_ || ui_->coin->count() == 0 || ui_->type->count() == 0) {
        ui_->offer = {};
        ui_->given = false;
        updateShown();
        return;
    }
    const wallet::ReceiveOffer next = controller_->nextReceive(ui_->wallet->currentData().toUInt(), chosenNetwork(), chosenType());
    if (next.address != ui_->offer.address) {
        ui_->given = false;
        ui_->label->clear();
        setStatus(ui_->status, QString(), Tone::Muted);
    }
    ui_->offer = next;
    updateShown();
}

QString ReceivePage::payload() const
{
    const QString address = ui_->offer.address;
    if (address.isEmpty())
        return {};
    const QString amount = ui_->amount->text().trimmed();
    const wallet::NetworkInfo& network = wallet::networkInfo(chosenNetwork());
    const wallet::AssetInfo& coin = wallet::assetInfo(network.coin);
    if (amount.isEmpty() || network.family != wallet::Family::Utxo || !validAmount(amount, coin))
        return address;
    // BIP 21. The label stays out: whoever scans it has no business reading the name given here.
    return QStringLiteral("%1:%2?amount=%3").arg(network.id, address, amount);
}

QString ReceivePage::shownAddress() const
{
    return ui_->address->text();
}

QString ReceivePage::shownPayload() const
{
    return payload();
}

void ReceivePage::updateShown()
{
    const wallet::ReceiveOffer& offer = ui_->offer;
    const bool ready = offer.state == wallet::ReceiveOffer::State::Ready && !offer.address.isEmpty();
    ui_->copy->setEnabled(ready);
    ui_->save->setEnabled(ready);
    // Moving on from one asked for goes back to what is next; from nothing, there is nothing to skip.
    ui_->next->setEnabled(ready);
    ui_->label->setEnabled(ready);
    ui_->address->setText(ready ? offer.address : QString());
    ui_->mark->setVisible(ready);
    ui_->only->setVisible(ready);
    if (ui_->coin->count() > 0) {
        const wallet::AssetInfo& info = wallet::assetInfo(wallet::Asset(quint16(ui_->coin->currentData().toInt())));
        ui_->mark->setPixmap(coinIcon(info, 24).pixmap(QSize(24, 24)));
        ui_->only->setText(onlyLine(wallet::networkInfo(info.network)));
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
    const QString amount = ui_->amount->text().trimmed();
    if (!amount.isEmpty() && ui_->coin->count() > 0 && ready && !validAmount(amount, wallet::assetInfo(wallet::networkInfo(chosenNetwork()).coin)))
        setStatus(ui_->status, trText(QStringLiteral("That amount cannot go into the QR code: write it like 0.015")), Tone::Warning);
    else if (ui_->status->text().startsWith(trText(QStringLiteral("That amount cannot go into the QR code: write it like 0.015"))))
        setStatus(ui_->status, QString(), Tone::Muted);

    switch (offer.state) {
    case wallet::ReceiveOffer::State::Checking:
        setStatus(ui_->state, trText(QStringLiteral("Looking for the next unused address: a server has to answer for it first.")), Tone::Muted);
        return;
    case wallet::ReceiveOffer::State::NoKey:
        setStatus(ui_->state, ui_->coin->count() == 0 ? trText(QStringLiteral("This wallet has no key to receive on.")) : trText(QStringLiteral("This wallet has no such key.")), Tone::Warning);
        return;
    case wallet::ReceiveOffer::State::Failed:
        setStatus(ui_->state, trText(offer.why), Tone::Error);
        return;
    case wallet::ReceiveOffer::State::Ready:
        break;
    }

    // What has come of it, for one that left this screen; what is known of it, for one that has not.
    const wallet::WalletModel* model = controller_->model();
    const quint32 walletId = ui_->wallet->currentData().toUInt();
    const int position = model->find(walletId, chosenNetwork(), chosenType(), offer.account, 0, offer.index);
    const int asked = ui_->asked ? model->find(ui_->row.wallet, ui_->row.network, ui_->row.type, ui_->row.account, ui_->row.change, ui_->row.index) : -1;
    const int at = ui_->asked ? asked : position;
    QString line = offer.path;
    Tone tone = Tone::Muted;
    if (at >= 0) {
        const wallet::AddressRow& row = model->at(at);
        if (row.state() == wallet::AddressState::Funded) {
            QStringList held;
            row.forEachBalance([&held](wallet::Asset asset, const chain::Amount& value) {
                const wallet::AssetInfo& info = wallet::assetInfo(asset);
                held << figures::formatAmount(value, info) + QLatin1Char(' ') + info.symbol;
            });
            line += QStringLiteral(" · ") + trText(QStringLiteral("Holds %1")).arg(held.join(QStringLiteral(", ")));
            tone = Tone::Success;
        } else if (row.used) {
            line += QStringLiteral(" · ") + trText(QStringLiteral("Has been used, and holds nothing now"));
        } else if (ui_->given) {
            line += QStringLiteral(" · ") + trText(QStringLiteral("Given out, nothing has come in yet"));
            tone = Tone::Accent;
        } else if (controller_->showingSaved()) {
            line += QStringLiteral(" · ") + trText(QStringLiteral("Unused as far as was saved, being checked"));
        } else {
            line += QStringLiteral(" · ") + trText(QStringLiteral("Unused"));
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
        put(ListAddress, row.address)->setData(Qt::UserRole, rows.at(i));
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
        QString state;
        Tone tone = Tone::Muted;
        if (row.state() == wallet::AddressState::Funded) {
            state = trText(QStringLiteral("Received"));
            tone = Tone::Success;
        } else if (row.used) {
            state = trText(QStringLiteral("Used"));
        } else {
            state = trText(row.checked ? QStringLiteral("Waiting") : QStringLiteral("Checking…"));
        }
        put(ListState, state)->setForeground(toneColor(tone));
    }
}

bool ReceivePage::keepLabel()
{
    const QString address = ui_->address->text();
    const QString wanted = ui_->label->text().trimmed();
    if (address.isEmpty() || wanted == controller_->addressLabel(address))
        return true;
    QString error;
    if (controller_->setAddressLabel(address, wanted, &error))
        return true;
    setStatus(ui_->status, error, Tone::Warning);
    // Not kept on screen either, if that is why it was refused.
    if (wallet::looksLikeSecret(wanted))
        ui_->label->clear();
    return false;
}

void ReceivePage::giveOut()
{
    if (ui_->address->text().isEmpty())
        return;
    const bool labelled = keepLabel();
    if (ui_->asked || ui_->given) {
        if (labelled)
            setStatus(ui_->status, QString(), Tone::Muted);
        updateShown();
        return;
    }
    QString error;
    const bool kept = controller_->markGivenOut(ui_->wallet->currentData().toUInt(), chosenNetwork(), chosenType(), ui_->offer.index, &error);
    ui_->given = true;
    if (!kept)
        setStatus(ui_->status, error, Tone::Warning);
    else if (labelled)
        setStatus(ui_->status, QString(), Tone::Muted);
    updateShown();
    updateList();
}

void ReceivePage::newAddress()
{
    giveOut();
    ui_->asked = false;
    ui_->given = false;
    ui_->offer = {};
    ui_->label->clear();
    ui_->amount->clear();
    offerNext();
    updateList();
}

void ReceivePage::saveQr()
{
    const QString text = payload();
    if (text.isEmpty())
        return;
    const QString path = QFileDialog::getSaveFileName(this, trText(QStringLiteral("Save QR")),
                                                      QStringLiteral("receive-%1.png").arg(ui_->address->text().left(12)),
                                                      QStringLiteral("PNG (*.png)"));
    if (path.isEmpty())
        return;
    if (!QrCode::encode(text.toUtf8()).toImage(kQrFileScale).save(path, "PNG")) {
        Feedback::show(ui_->save, Feedback::Type::Error, trText(QStringLiteral("Could not write the file")));
        return;
    }
    giveOut();
    Feedback::show(ui_->save, Feedback::Type::Success, trText(QStringLiteral("QR saved")));
}

} // namespace app::ui
