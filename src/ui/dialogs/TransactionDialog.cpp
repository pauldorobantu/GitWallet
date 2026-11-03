#include "ui/dialogs/TransactionDialog.h"

#include "app/WalletController.h"
#include "net/PriceFeed.h"
#include "ui/DialogChrome.h"
#include "ui/Feedback.h"
#include "ui/Figures.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/TableUtil.h"
#include "ui/design/Style.h"
#include "wallet/UtxoHistory.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <exception>

namespace app::ui {

namespace {

// How often it looks at the book while that moves: for a person reading, not for a list.
constexpr int kFollowMs = 1000;

QString dateTimeText(qint64 seconds)
{
    if (seconds <= 0)
        return figures::dash();
    static const QLocale english(QLocale::English, QLocale::UnitedStates);
    return english.toString(QDateTime::fromSecsSinceEpoch(seconds).toLocalTime(), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString coinText(qint64 units, const wallet::AssetInfo& coin)
{
    // Every digit: this is where a figure is checked against an explorer.
    const quint64 magnitude = units < 0 ? 0 - quint64(units) : quint64(units);
    return QString::fromStdString(chain::Amount::fromU64(magnitude).format(coin.decimals, coin.decimals)) + QLatin1Char(' ') + coin.symbol;
}

QLabel* valueLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

QTableWidget* sideTable(QWidget* parent)
{
    auto* table = new QTableWidget(0, 2, parent);
    page::configureTable(table);
    table->setHorizontalHeaderLabels({trText(QStringLiteral("Address")), trText(QStringLiteral("Amount"))});
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideMiddle);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->setMinimumHeight(120);
    return table;
}

} // namespace

TransactionDialog::TransactionDialog(WalletController* controller, HistoryTableModel* model, const HistoryTableModel::Line& line, QWidget* parent)
    : QDialog(parent), controller_(controller), model_(model), line_(line)
{
    const wallet::TxRecord& tx = line.book->at(line.position);
    txid_ = tx.txid;
    const wallet::NetworkInfo& network = wallet::networkInfo(line.book->network());
    const wallet::AssetInfo& coin = wallet::assetInfo(network.coin);
    const QString hex = QString::fromStdString(chain::hashHex(tx.txid));

    setObjectName(QStringLiteral("transactionDialog"));
    setWindowTitle(trText(QStringLiteral("Transaction")));
    applyDialogChrome(this);
    auto* column = new QVBoxLayout(this);
    page::applyDialogLayout(column);

    auto* form = new QFormLayout;
    page::applyFormLayout(form);

    kindValue_ = valueLabel(QString(), this);
    form->addRow(trText(QStringLiteral("Type")), kindValue_);

    // How far it is believed, and on what.
    stateValue_ = valueLabel(QString(), this);
    stateValue_->setObjectName(QStringLiteral("txState"));
    form->addRow(trText(QStringLiteral("State")), stateValue_);
    whyValue_ = valueLabel(QString(), this);
    setTone(whyValue_, Tone::Muted);
    form->addRow(QString(), whyValue_);

    dateValue_ = valueLabel(QString(), this);
    form->addRow(trText(QStringLiteral("Date")), dateValue_);

    amountValue_ = valueLabel(QString(), this);
    amountValue_->setObjectName(QStringLiteral("txAmount"));
    form->addRow(trText(QStringLiteral("Amount")), amountValue_);

    usdNowValue_ = valueLabel(QString(), this);
    form->addRow(trText(QStringLiteral("USD now")), usdNowValue_);
    usdThenValue_ = valueLabel(QString(), this);
    form->addRow(trText(QStringLiteral("USD then")), usdThenValue_);

    feeValue_ = valueLabel(figures::dash(), this);
    feeValue_->setObjectName(QStringLiteral("txFee"));
    // Only what an id covers is proven: the witness is a server's word.
    feeValue_->setToolTip(trText(QStringLiteral("The fee rate uses the size a server reports: no proof covers a transaction's witness.")));
    form->addRow(trText(QStringLiteral("Fee")), feeValue_);

    const QStringList wallets = model_->walletsOf(line);
    if (!wallets.isEmpty())
        form->addRow(trText(wallets.size() == 1 ? QStringLiteral("Wallet") : QStringLiteral("Wallets")), valueLabel(wallets.join(QStringLiteral(" · ")), this));

    auto* id = valueLabel(hex, this);
    id->setObjectName(QStringLiteral("txId"));
    id->setMinimumWidth(0);
    form->addRow(trText(QStringLiteral("Transaction")), id);
    auto* idButtons = new QHBoxLayout;
    idButtons->setSpacing(page::kToolbarSpacing);
    auto* copy = new QPushButton(trText(QStringLiteral("Copy")), this);
    connect(copy, &QPushButton::clicked, this, [copy, hex] { copyTextToClipboard(copy, hex, trText(QStringLiteral("Transaction id copied"))); });
    idButtons->addWidget(copy);
    auto* explorer = new QPushButton(trText(QStringLiteral("Open in explorer")), this);
    explorer->setToolTip(trText(QStringLiteral("Opens this transaction on a public block explorer, in your browser")));
    connect(explorer, &QPushButton::clicked, this, [network, hex] {
        QString url = network.txUrl;
        QDesktopServices::openUrl(QUrl(url.replace(QLatin1String("{id}"), hex)));
    });
    idButtons->addWidget(explorer);
    idButtons->addStretch(1);
    form->addRow(QString(), idButtons);

    if (tx.seen == wallet::TxSeen::Replaced && tx.replacedBy != chain::Hash256{})
        form->addRow(trText(QStringLiteral("Replaced by")), valueLabel(QString::fromStdString(chain::hashHex(tx.replacedBy)), this));

    const wallet::TxNote saved = line.book->noteOf(tx.txid);
    labelAtOpen_ = saved.label;
    noteAtOpen_ = saved.note;
    label_ = new QLineEdit(this);
    label_->setObjectName(QStringLiteral("txLabel"));
    // Not shorter than the one there, which a file can have made longer: opening is no edit.
    label_->setMaxLength(qMax(1024, int(saved.label.size())));
    label_->setText(saved.label);
    label_->setPlaceholderText(trText(QStringLiteral("An order, a customer, an invoice")));
    form->addRow(trText(QStringLiteral("Label")), label_);
    note_ = new QPlainTextEdit(saved.note, this);
    note_->setObjectName(QStringLiteral("txNote"));
    note_->setTabChangesFocus(true);
    note_->setFixedHeight(64);
    form->addRow(trText(QStringLiteral("Note")), note_);
    column->addLayout(form);

    reading_ = valueLabel(trText(QStringLiteral("Reading where the coins came from…")), this);
    setTone(reading_, Tone::Muted);
    column->addWidget(reading_);

    auto* sides = new QHBoxLayout;
    sides->setSpacing(page::kFormHSpacing);
    auto* fromColumn = new QVBoxLayout;
    auto* toColumn = new QVBoxLayout;
    fromColumn->addWidget(new QLabel(trText(QStringLiteral("From")), this));
    toColumn->addWidget(new QLabel(trText(QStringLiteral("To")), this));
    from_ = sideTable(this);
    from_->setObjectName(QStringLiteral("txFrom"));
    to_ = sideTable(this);
    to_->setObjectName(QStringLiteral("txTo"));
    fromColumn->addWidget(from_, 1);
    toColumn->addWidget(to_, 1);
    sides->addLayout(fromColumn, 1);
    sides->addLayout(toColumn, 1);
    column->addLayout(sides, 1);

    notKept_ = valueLabel(QString(), this);
    notKept_->setObjectName(QStringLiteral("txNotKept"));
    setTone(notKept_, Tone::Error);
    notKept_->hide();
    column->addWidget(notKept_);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    discard_ = new QPushButton(trText(QStringLiteral("Discard changes")), this);
    discard_->setObjectName(QStringLiteral("txDiscard"));
    discard_->hide();
    buttons->addWidget(discard_);
    auto* close = new QPushButton(trText(QStringLiteral("Close")), this);
    close->setDefault(true);
    buttons->addWidget(close);
    column->addLayout(buttons);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(discard_, &QPushButton::clicked, this, [this] {
        discarding_ = true;
        reject();
    });
    // What it said was of what was there then.
    const auto editing = [this] {
        notKept_->hide();
        discard_->hide();
    };
    connect(label_, &QLineEdit::textEdited, this, editing);
    connect(note_, &QPlainTextEdit::textChanged, this, editing);

    follow_ = new QTimer(this);
    follow_->setSingleShot(true);
    follow_->setInterval(kFollowMs);
    connect(follow_, &QTimer::timeout, this, &TransactionDialog::refresh);
    const auto later = [this] {
        if (!follow_->isActive())
            follow_->start();
    };
    connect(line.book, &wallet::HistoryBook::changed, this, later);
    connect(line.book, &wallet::HistoryBook::positionsMoved, this, later);
    if (controller_) {
        connect(controller_, &WalletController::historyProgress, this, later);
        connect(controller_->prices(), &net::PriceFeed::updated, this, later);
    }
    refresh();

    page::fitDialog(this, page::DialogSize::Wide);

    wallet::UtxoHistory* history = controller_ ? controller_->historyOf(line.book->network()) : nullptr;
    if (history && history->running()) {
        // It waits for a server, and fills in when there is one.
        if (!history->connected())
            reading_->setText(trText(QStringLiteral("No server is connected, so where the coins came from cannot be read.")));
        history->readDetail(txid_, this, [this](const wallet::TxDetail& detail) { showDetail(detail); });
    } else {
        reading_->setText(trText(QStringLiteral("No server is connected, so where the coins came from cannot be read.")));
    }
}

void TransactionDialog::refresh()
{
    const wallet::NetworkInfo& network = wallet::networkInfo(line_.book->network());
    const wallet::AssetInfo& coin = wallet::assetInfo(network.coin);
    HistoryTableModel::Line now;
    if (!model_->lineOf(line_.book, txid_, &now)) {
        // A wallet it belonged to is gone, or what the page is narrowed to.
        stateValue_->setText(trText(QStringLiteral("No longer in the history")));
        setTone(stateValue_, Tone::Muted);
        whyValue_->clear();
        return;
    }
    line_ = now;
    const wallet::TxRecord& tx = line_.book->at(line_.position);
    const qint64 net = line_.received - line_.spent;
    const double whole = double(net < 0 ? -net : net) / 100000000.0;

    kindValue_->setText(model_->kindShown(line_));

    QString state = model_->stateText(line_);
    if (tx.height > 0)
        state += QStringLiteral(" · ") + trText(QStringLiteral("block %1")).arg(QLocale(QLocale::English).toString(tx.height));
    stateValue_->setText(state);
    switch (line_.state) {
    case TxState::Confirmed:
    case TxState::Confirming:
        setTone(stateValue_, Tone::Success);
        whyValue_->setText(trText(QStringLiteral("Proven on this PC: a Merkle proof ties it to this block, and the block's header and work were checked.")));
        break;
    case TxState::Pending:
        setTone(stateValue_, Tone::Warning);
        whyValue_->setText(trText(QStringLiteral("Not in a block yet. It can still be replaced: do not count it as paid.")));
        break;
    case TxState::Unproven:
        setTone(stateValue_, Tone::Warning);
        whyValue_->setText(trText(QStringLiteral("A server says it is in this block. Its proof has not been checked yet.")));
        break;
    case TxState::ProofFailed:
        setTone(stateValue_, Tone::Warning);
        whyValue_->setText(trText(QStringLiteral("A server says it is in this block, and the proof it gave does not hold, or it gives none. Do not count it as paid.")));
        break;
    case TxState::Replaced:
        setTone(stateValue_, Tone::Muted);
        whyValue_->setText(trText(QStringLiteral("Another transaction spends the same coins. This one no longer counts.")));
        break;
    case TxState::Dropped:
        setTone(stateValue_, Tone::Muted);
        whyValue_->setText(trText(QStringLiteral("Servers no longer list it. Unless it comes back, it does not count.")));
        break;
    }

    dateValue_->setText(dateTimeText(tx.time));
    amountValue_->setText((net < 0 ? QStringLiteral("-") : QStringLiteral("+")) + coinText(net, coin));
    const double priceNow = controller_ ? controller_->prices()->usd(coin.asset) : 0.0;
    usdNowValue_->setText(priceNow > 0.0 ? QStringLiteral("%1 · 1 %2 = %3").arg(figures::formatUsd(whole * priceNow), coin.symbol, figures::formatUsd(priceNow)) : figures::dash());
    usdThenValue_->setText(tx.usdThen > 0.0 ? QStringLiteral("%1 · 1 %2 = %3 · %4").arg(figures::formatUsd(whole * tx.usdThen), coin.symbol, figures::formatUsd(tx.usdThen), dateTimeText(tx.usdThenAt))
                                            : trText(QStringLiteral("not known")));
    showFee(detailFee_ >= 0 ? detailFee_ : tx.fee, detailVsize_ > 0 ? detailVsize_ : tx.vsize);
}

void TransactionDialog::showFee(qint64 fee, int vsize)
{
    if (fee < 0) {
        feeValue_->setText(figures::dash());
        return;
    }
    const wallet::AssetInfo& coin = wallet::assetInfo(wallet::networkInfo(line_.book->network()).coin);
    const double priceNow = controller_ ? controller_->prices()->usd(coin.asset) : 0.0;
    const QString rate = vsize > 0 ? QStringLiteral(" · %1 sat/vB").arg(QString::number(double(fee) / vsize, 'f', 1)) : QString();
    const QString usd = priceNow > 0.0 ? QStringLiteral(" · ") + figures::formatUsd(double(fee) / 100000000.0 * priceNow) : QString();
    // Of a payment from someone else, it is theirs.
    const QString whose = line_.spent == 0 ? QStringLiteral(" · ") + trText(QStringLiteral("paid by the sender")) : QString();
    feeValue_->setText(coinText(fee, coin) + rate + usd + whose);
}

void TransactionDialog::addLine(QTableWidget* table, const QString& address, qint64 value, bool ours, bool known)
{
    const wallet::AssetInfo& coin = wallet::assetInfo(wallet::networkInfo(line_.book->network()).coin);
    const int row = table->rowCount();
    table->insertRow(row);
    // What made it could not be read is not the same as a script that has no address.
    QString text = !known ? trText(QStringLiteral("(not read from the server)")) : address.isEmpty() ? trText(QStringLiteral("(no address)")) : address;
    const QString label = controller_ ? controller_->addressLabel(address) : QString();
    if (!label.isEmpty())
        text = QStringLiteral("%1 · %2").arg(label, text);
    auto* where = makeCell(text, !ours);
    where->setToolTip(ours ? address + QLatin1Char('\n') + trText(QStringLiteral("One of your addresses")) : address);
    if (ours)
        where->setForeground(toneColor(Tone::Accent));
    table->setItem(row, 0, where);
    auto* amount = makeCell(known ? coinText(value, coin) : figures::dash(), !ours);
    amount->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    table->setItem(row, 1, amount);
}

void TransactionDialog::showDetail(const wallet::TxDetail& detail)
{
    if (!detail.why.isEmpty()) {
        reading_->setText(trText(detail.why));
        setTone(reading_, Tone::Warning);
        return;
    }
    for (const wallet::TxSideLine& side : detail.inputs)
        addLine(from_, side.address, side.value, side.ours, side.value >= 0);
    for (const wallet::TxSideLine& side : detail.outputs)
        addLine(to_, side.address, side.value, side.ours, side.value >= 0);

    if (detail.coinbase) {
        reading_->setText(trText(QStringLiteral("Newly mined coins: they come from no address.")));
    } else if (detail.inputsUnread > 0) {
        reading_->setText(trText(QStringLiteral("%1 of its inputs could not be read from the server.")).arg(detail.inputsUnread));
        setTone(reading_, Tone::Warning);
    } else if (detail.inputsLeftOut > 0) {
        reading_->setText(trText(QStringLiteral("It spends %1 more inputs than are listed here.")).arg(detail.inputsLeftOut));
    } else {
        reading_->hide();
    }
    detailFee_ = detail.fee;
    detailVsize_ = detail.vsize;
    const int position = line_.book->find(txid_);
    showFee(detail.fee >= 0 ? detail.fee : (position >= 0 ? line_.book->at(position).fee : -1), detail.vsize);
}

bool TransactionDialog::saveNotes()
{
    const wallet::TxNote now{label_->text().trimmed(), note_->toPlainText().trimmed()};
    if (now.label == labelAtOpen_ && now.note == noteAtOpen_)
        return true;
    if (!controller_)
        return false;
    QString error;
    if (controller_->setTxNote(line_.book->network(), txid_, now, &error)) {
        labelAtOpen_ = now.label;
        noteAtOpen_ = now.note;
        return true;
    }
    notKept_->setText(trText(error));
    notKept_->show();
    discard_->show();
    return false;
}

void TransactionDialog::done(int result)
{
    // What was typed exists nowhere else: however this is being closed, it stays open if that cannot be kept.
    if (!discarding_ && !saveNotes())
        return;
    QDialog::done(result);
}

} // namespace app::ui
