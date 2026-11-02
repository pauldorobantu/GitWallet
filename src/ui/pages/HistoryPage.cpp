#include "ui/pages/HistoryPage.h"

#include "app/WalletController.h"
#include "net/PriceFeed.h"
#include "ui/Csv.h"
#include "ui/Feedback.h"
#include "ui/Figures.h"
#include "ui/HistoryTableModel.h"
#include "ui/HoverRowTable.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/TableUtil.h"
#include "ui/ThemeManager.h"
#include "ui/design/Style.h"
#include "ui/dialogs/TransactionDialog.h"
#include "wallet/History.h"
#include "wallet/Types.h"
#include "wallet/UtxoHistory.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <exception>
#include <memory>

namespace app::ui {

namespace {

constexpr int kSearchPauseMs = 250;
// Past this many transactions a search waits for the typing to stop.
constexpr int kRebuildAbove = 20000;
constexpr int kExportChunk = 2000;
// "Last 24 hours" is that for as long as the window stays open, which can be days.
constexpr int kPeriodAgainMs = 60'000;

enum Period { PeriodAll, PeriodDay, PeriodWeek, PeriodMonth, PeriodYear, PeriodLastYear };

QString grouped(int n)
{
    return figures::formatCount(n);
}

QString amountText(qint64 units, const wallet::AssetInfo& coin)
{
    return figures::formatAmount(chain::Amount::fromU64(quint64(units < 0 ? 0 : units)), coin) + QLatin1Char(' ') + coin.symbol;
}

// A letter typed on the list goes to the search. Left to itself the list would look through half a
// million lines for one that starts with it, on this thread, per key.
class HistoryView : public HoverRowTableView
{
public:
    using HoverRowTableView::HoverRowTableView;
    QLineEdit* search = nullptr;

    void keyboardSearch(const QString& text) override
    {
        // A space means choosing the row.
        if (!search || text.trimmed().isEmpty())
            return;
        search->setFocus();
        search->insert(text);
    }

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        // Past the clipboard's history, as every copy here is. What says there is nothing leaves what
        // the clipboard holds as it is.
        if (event->matches(QKeySequence::Copy) && currentIndex().isValid()) {
            const QString text = HistoryPage::textToCopy(currentIndex());
            if (!text.isEmpty())
                copyTextToClipboard(this, text, trText(QStringLiteral("Copied")));
            event->accept();
            return;
        }
        HoverRowTableView::keyPressEvent(event);
    }
};

} // namespace

// Not the cell as it is drawn: an id with its middle left out names nothing.
QString HistoryPage::textToCopy(const QModelIndex& cell)
{
    const QVariant whole = cell.data(kCopyRole);
    const QString text = (whole.isValid() ? whole.toString() : cell.data().toString()).trimmed();
    return text == figures::dash() ? QString() : text;
}

struct HistoryPage::Ui : QObject
{
    using QObject::QObject;

    HistoryTableModel* model = nullptr;
    HoverRowTableView* view = nullptr;
    QLabel* status = nullptr;
    QLineEdit* search = nullptr;
    QComboBox* wallet = nullptr;
    QComboBox* type = nullptr;
    QComboBox* state = nullptr;
    QComboBox* period = nullptr;
    QLabel* count = nullptr;
    QPushButton* details = nullptr;
    QPushButton* exportCsv = nullptr;
    QWidget* noWallet = nullptr;
    QWidget* waiting = nullptr;
    QWidget* noResults = nullptr;
    QWidget* notRead = nullptr; // a network whose history is not read yet
    QTimer* searchPause = nullptr;
    QTimer* update = nullptr;
    QTimer* periodAgain = nullptr;

    // What a place narrows the page to.
    std::optional<wallet::Network> network;
    QString scripthash;
    QString address;
    bool inPlace = false;

    // Where the reader was, across a change of the list.
    wallet::HistoryBook* topBook = nullptr;
    chain::Hash256 topTx{};
    wallet::HistoryBook* chosenBook = nullptr;
    chain::Hash256 chosenTx{};
    bool hadPlace = false;
    bool placeHeld = false; // read before a book moved everything: the next rebuild goes by that
    int chosenColumn = -1;  // the cell Copy goes by
    int yearOfFilter = 0;   // what "this year" and "last year" were worked out from

    // An export on its way, a chunk per event-loop turn.
    QVector<HistoryTableModel::Line> exporting;
    std::unique_ptr<QSaveFile> exportFile;
    CsvFormat exportFormat;
    HistoryTableModel::Filter exportFilter;
    int exportAt = 0;
    bool exportWhole = true;
};

HistoryPage::HistoryPage(WalletController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller)
{
    ui_ = new Ui(this);
    auto* root = new QVBoxLayout(this);
    page::applyPageLayout(root);

    ui_->status = new QLabel(this);
    ui_->status->setObjectName(QStringLiteral("historyStatus"));
    ui_->status->setTextFormat(Qt::PlainText);
    setTone(ui_->status, Tone::Muted);
    root->addWidget(ui_->status);

    ui_->model = new HistoryTableModel(controller_, this);
    auto* view = new HistoryView(this);
    ui_->view = view;
    view->setModel(ui_->model);
    page::configureTable(view);
    view->setMouseTracking(true);
    view->setWordWrap(false);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    QHeaderView* header = view->horizontalHeader();
    header->setSectionsClickable(true);
    header->setSortIndicatorClearable(false);
    header->setHighlightSections(false);
    header->setStretchLastSection(false);
    header->setSortIndicator(HistoryTableModel::ColDate, Qt::DescendingOrder);
    view->setSortingEnabled(true);
    installCopyOnClick(view, {HistoryTableModel::ColTx});
    applyColumnWidths();
    root->addWidget(view, 1);

    ui_->noWallet = page::makeEmptyState(this, int(icon::Kind::Traffic), QStringLiteral("No wallet yet"),
                                         QStringLiteral("Add a wallet on the Wallets page, and its transactions show here."));
    ui_->waiting = page::makeEmptyState(this, int(icon::Kind::Traffic), QStringLiteral("No transactions yet"),
                                        QStringLiteral("Every transaction of every address shows here, as servers tell of it and proofs check out."));
    ui_->noResults = page::makeNoResultsState(this);
    ui_->notRead = page::makeEmptyState(this, int(icon::Kind::Traffic), QStringLiteral("Not read yet on this network"),
                                        QStringLiteral("The history of Ethereum, USDT and USDC comes in the next stage. Balances are read already, and an address opens on a block explorer."));
    root->addWidget(ui_->noWallet, 1);
    root->addWidget(ui_->waiting, 1);
    root->addWidget(ui_->noResults, 1);
    root->addWidget(ui_->notRead, 1);
    ui_->notRead->hide();

    const page::ActionBar bar = page::makeActionBar(this);
    ui_->search = page::makeSearch(bar.widget, QStringLiteral("Search id, address, label or note"));
    view->search = ui_->search;
    ui_->wallet = new QComboBox(bar.widget);
    ui_->wallet->setObjectName(QStringLiteral("historyWallet"));
    ui_->wallet->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ui_->wallet->setMaximumWidth(page::kFieldMaxW / 2);
    ui_->type = new QComboBox(bar.widget);
    ui_->type->setObjectName(QStringLiteral("historyType"));
    ui_->type->addItem(QStringLiteral("All types"), -1);
    ui_->type->addItem(QStringLiteral("Received"), int(TxKind::Received));
    ui_->type->addItem(QStringLiteral("Sent"), int(TxKind::Sent));
    ui_->type->addItem(QStringLiteral("Moved"), int(TxKind::Moved));
    ui_->type->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ui_->state = new QComboBox(bar.widget);
    ui_->state->setObjectName(QStringLiteral("historyState"));
    ui_->state->addItem(QStringLiteral("All states"), -1);
    ui_->state->addItem(QStringLiteral("Pending"), int(HistoryTableModel::kStatePending));
    ui_->state->addItem(QStringLiteral("In a block"), int(HistoryTableModel::kStateConfirmed));
    ui_->state->addItem(QStringLiteral("Proof did not hold"), int(HistoryTableModel::kStateProofFailed));
    ui_->state->addItem(QStringLiteral("Replaced or dropped"), int(HistoryTableModel::kStateGone));
    ui_->state->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ui_->period = new QComboBox(bar.widget);
    ui_->period->setObjectName(QStringLiteral("historyPeriod"));
    ui_->period->addItems({QStringLiteral("All time"), QStringLiteral("Last 24 hours"), QStringLiteral("Last 7 days"),
                           QStringLiteral("Last 30 days"), QStringLiteral("This year"), QStringLiteral("Last year")});
    ui_->period->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ui_->count = page::makeBarCount(bar.widget);
    bar.left->addWidget(ui_->search);
    bar.left->addWidget(ui_->wallet);
    bar.left->addWidget(ui_->type);
    bar.left->addWidget(ui_->state);
    bar.left->addWidget(ui_->period);
    page::finishBarLeft(bar, ui_->search, ui_->count);
    ui_->exportCsv = new QPushButton(QStringLiteral("Export CSV"), bar.widget);
    ui_->details = new QPushButton(QStringLiteral("Details"), bar.widget);
    ui_->details->setObjectName(QStringLiteral("primaryButton"));
    bar.right->addWidget(ui_->exportCsv);
    bar.right->addWidget(ui_->details);
    root->addWidget(bar.widget);

    ui_->searchPause = new QTimer(this);
    ui_->searchPause->setSingleShot(true);
    ui_->searchPause->setInterval(kSearchPauseMs);
    connect(ui_->searchPause, &QTimer::timeout, this, &HistoryPage::applyFilter);
    ui_->update = new QTimer(this);
    ui_->update->setSingleShot(true);
    ui_->update->setInterval(200);
    connect(ui_->update, &QTimer::timeout, this, &HistoryPage::updateState);
    ui_->periodAgain = new QTimer(this);
    ui_->periodAgain->setInterval(kPeriodAgainMs);
    connect(ui_->periodAgain, &QTimer::timeout, this, [this] {
        if (periodMoved())
            applyFilter();
    });

    connect(ui_->search, &QLineEdit::textChanged, this, [this] {
        if (ui_->model->totalCount() > kRebuildAbove)
            ui_->searchPause->start();
        else
            applyFilter();
    });
    for (QComboBox* combo : {ui_->wallet, ui_->type, ui_->state, ui_->period})
        connect(combo, &QComboBox::currentIndexChanged, this, &HistoryPage::applyFilter);
    if (controller_) {
        const QList<wallet::UtxoHistory*> histories = controller_->histories();
        for (const wallet::UtxoHistory* history : histories) {
            connect(history->book(), &wallet::HistoryBook::positionsMoved, this, [this] {
                // What it was writing went by positions.
                if (ui_->exportFile)
                    finishExport(false, trText(QStringLiteral("The list changed while it was being exported. Try again.")));
            });
        }
    }
    connect(ui_->model, &HistoryTableModel::aboutToRebuild, this, &HistoryPage::rememberPlace);
    connect(ui_->model, &HistoryTableModel::positionsAboutToMove, this, [this] {
        rememberPlace();
        ui_->placeHeld = true;
    });
    connect(ui_->model, &HistoryTableModel::rebuilt, this, [this] {
        restorePlace();
        if (!ui_->update->isActive())
            ui_->update->start();
    });
    // A double click, or Enter on the row.
    connect(view, &QAbstractItemView::activated, this, [this](const QModelIndex& index) { openRow(index.row()); });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &HistoryPage::updateState);
    connect(ui_->details, &QPushButton::clicked, this, &HistoryPage::openSelected);
    connect(ui_->exportCsv, &QPushButton::clicked, this, &HistoryPage::chooseExportFile);

    connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed, this, &HistoryPage::applyColumnWidths);
    const auto setTips = [this] {
        ui_->type->setToolTip(trText(QStringLiteral("Moved: every output came back to the addresses shown, so only a fee left.")));
        ui_->exportCsv->setToolTip(trText(QStringLiteral("Save the lines shown to a spreadsheet file. It is not encrypted: anyone who reads it "
                                                         "sees these transactions, labels and notes.")));
        ui_->count->setToolTip(trText(QStringLiteral("In and out: what the lines shown came to, of those proven in a block.")));
    };
    setTips();
    connect(LanguageManager::instance(), &LanguageManager::changed, this, [this, setTips] {
        setTips();
        applyColumnWidths();
        updateState();
    });
    if (controller_) {
        connect(controller_, &WalletController::walletChanged, this, [this] {
            rebuildWalletFilter();
            // One address the page was narrowed to, and its wallet is gone.
            if (!ui_->scripthash.isEmpty() && ui_->network && controller_->model()->findByScripthash(*ui_->network, ui_->scripthash) < 0) {
                ui_->scripthash.clear();
                ui_->address.clear();
                applyFilter();
            }
            updateState();
        });
        connect(controller_, &WalletController::historyProgress, this, [this] {
            if (!ui_->update->isActive())
                ui_->update->start();
        });
        connect(controller_, &WalletController::statusChanged, this, [this] {
            if (!ui_->update->isActive())
                ui_->update->start();
        });
    }
    rebuildWalletFilter();
    ui_->model->rebuildNow();
    updateState();
}

HistoryTableModel* HistoryPage::tableModel() const
{
    return ui_->model;
}

void HistoryPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    ui_->model->setWatched(true);
    ui_->periodAgain->start();
    if (periodMoved())
        applyFilter();
    updateState();
}

bool HistoryPage::periodMoved() const
{
    switch (ui_->period->currentIndex()) {
    case PeriodDay:
    case PeriodWeek:
    case PeriodMonth:
        return true;
    case PeriodYear:
    case PeriodLastYear:
        return ui_->yearOfFilter != QDate::currentDate().year();
    default:
        return false;
    }
}

void HistoryPage::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    ui_->model->setWatched(false);
    ui_->periodAgain->stop();
}

void HistoryPage::rebuildWalletFilter()
{
    const quint32 chosen = ui_->wallet->currentData().toUInt();
    const QList<wallet::WalletEntry> wallets = controller_ ? controller_->store().wallets : QList<wallet::WalletEntry>();
    {
        const QSignalBlocker quiet(ui_->wallet);
        ui_->wallet->clear();
        ui_->wallet->addItem(QStringLiteral("All wallets"), quint32(0));
        for (const wallet::WalletEntry& w : wallets)
            ui_->wallet->addItem(w.label, w.id);
        const int keep = ui_->wallet->findData(chosen);
        ui_->wallet->setCurrentIndex(keep >= 0 ? keep : 0);
        ui_->wallet->setProperty("i18nOrig_items", QVariant());
    }
    ui_->wallet->setVisible(wallets.size() > 1 && !ui_->inPlace);
    if (!ui_->inPlace)
        ui_->view->setColumnHidden(HistoryTableModel::ColWallet, wallets.size() < 2);
    if (chosen != ui_->wallet->currentData().toUInt())
        applyFilter();
}

// What a search or a filter left behind has no business narrowing "all of it".
void HistoryPage::resetFilters()
{
    const QSignalBlocker quietWallet(ui_->wallet);
    const QSignalBlocker quietType(ui_->type);
    const QSignalBlocker quietState(ui_->state);
    const QSignalBlocker quietPeriod(ui_->period);
    const QSignalBlocker quietSearch(ui_->search);
    ui_->wallet->setCurrentIndex(0);
    ui_->type->setCurrentIndex(0);
    ui_->state->setCurrentIndex(0);
    ui_->period->setCurrentIndex(PeriodAll);
    ui_->search->clear();
}

void HistoryPage::showAll()
{
    ui_->scripthash.clear();
    ui_->address.clear();
    ui_->network.reset();
    resetFilters();
    applyFilter();
}

void HistoryPage::showWallet(quint32 id)
{
    ui_->scripthash.clear();
    ui_->address.clear();
    ui_->network.reset();
    resetFilters();
    {
        const QSignalBlocker quiet(ui_->wallet);
        const int index = ui_->wallet->findData(id);
        ui_->wallet->setCurrentIndex(index >= 0 ? index : 0);
    }
    applyFilter();
}

void HistoryPage::showPlace(quint32 wallet, std::optional<wallet::Network> network, const QString& scripthash, const QString& address)
{
    ui_->inPlace = true;
    ui_->network = network;
    ui_->scripthash = scripthash;
    ui_->address = address;
    resetFilters();
    {
        const QSignalBlocker quiet(ui_->wallet);
        const int index = ui_->wallet->findData(wallet);
        ui_->wallet->setCurrentIndex(index >= 0 ? index : 0);
    }
    ui_->wallet->setVisible(false);
    ui_->view->setColumnHidden(HistoryTableModel::ColWallet, wallet != 0 || (controller_ && controller_->store().wallets.size() < 2));
    applyFilter();
}

void HistoryPage::clearPlace()
{
    if (!ui_->inPlace)
        return;
    ui_->inPlace = false;
    showAll();
    rebuildWalletFilter();
}

void HistoryPage::applyFilter()
{
    ui_->searchPause->stop();
    HistoryTableModel::Filter filter;
    filter.wallet = ui_->wallet->currentData().toUInt();
    filter.scripthash = ui_->scripthash;
    filter.network = ui_->network ? int(*ui_->network) : -1;
    filter.kind = ui_->type->currentData().toInt();
    filter.state = ui_->state->currentData().toInt();
    filter.query = ui_->search->text().trimmed();
    const QDateTime now = QDateTime::currentDateTime();
    ui_->yearOfFilter = now.date().year();
    switch (ui_->period->currentIndex()) {
    case PeriodDay:
        filter.from = now.addDays(-1).toSecsSinceEpoch();
        break;
    case PeriodWeek:
        filter.from = now.addDays(-7).toSecsSinceEpoch();
        break;
    case PeriodMonth:
        filter.from = now.addDays(-30).toSecsSinceEpoch();
        break;
    case PeriodYear:
        filter.from = QDate(now.date().year(), 1, 1).startOfDay().toSecsSinceEpoch();
        break;
    case PeriodLastYear:
        filter.from = QDate(now.date().year() - 1, 1, 1).startOfDay().toSecsSinceEpoch();
        filter.until = QDate(now.date().year(), 1, 1).startOfDay().toSecsSinceEpoch();
        break;
    default:
        break;
    }
    ui_->model->setFilter(filter);
    updateState();
}

void HistoryPage::updateState()
{
    ui_->update->stop();
    const bool hasWallet = controller_ && controller_->hasWallet();
    for (QWidget* control : std::initializer_list<QWidget*>{ui_->search, ui_->wallet, ui_->type, ui_->state, ui_->period})
        control->setEnabled(hasWallet);

    const int shown = ui_->model->rowCount();
    const int total = ui_->model->totalCount();
    const QModelIndexList chosen = ui_->view->selectionModel()->selectedRows();
    ui_->details->setEnabled(!chosen.isEmpty());
    const bool exporting = ui_->exportFile != nullptr;
    // Not part of what the database kept, with nothing to say so.
    ui_->exportCsv->setEnabled(shown > 0 && !exporting && !(controller_ && controller_->loadingSaved()));

    // What the lines shown came to, in the only coin there is so far.
    QString line;
    if (hasWallet) {
        // Of the same moment: what comes in between two rebuilds is no filter.
        const int all = ui_->model->totalAtRebuild();
        line = shown == all ? trText(all == 1 ? QStringLiteral("%1 transaction") : QStringLiteral("%1 transactions")).arg(grouped(all))
                            : trText(QStringLiteral("%1 of %2 transactions")).arg(grouped(shown), grouped(all));
        const wallet::NetworkInfo* utxo = wallet::firstOf(wallet::Family::Utxo);
        if (utxo && shown > 0) {
            const wallet::AssetInfo& coin = wallet::assetInfo(utxo->coin);
            try {
                line += QStringLiteral(" · %1 %2 · %3 %4")
                            .arg(trText(QStringLiteral("in")), amountText(ui_->model->receivedShown(), coin),
                                 trText(QStringLiteral("out")), amountText(ui_->model->spentShown(), coin));
            } catch (const std::exception&) {
            }
        }
    }
    ui_->count->setText(line);
    ui_->count->setVisible(hasWallet);

    // How far the reading has got, and what is not as it should be.
    wallet::HistoryProgress sum;
    bool connected = false;
    if (controller_) {
        const QList<wallet::UtxoHistory*> histories = controller_->histories();
        for (const wallet::UtxoHistory* history : histories) {
            const wallet::HistoryProgress one = history->progress();
            sum.addressesLeft += one.addressesLeft;
            sum.transactionsLeft += one.transactionsLeft;
            sum.proofsLeft += one.proofsLeft;
            sum.refused += one.refused;
            sum.differing += one.differing;
            sum.unproven += one.unproven;
            connected = connected || !history->running() || history->connected();
        }
    }
    QStringList said;
    Tone tone = Tone::Muted;
    if (sum.working()) {
        said << trText(QStringLiteral("Reading the history: %1 addresses, %2 transactions and %3 proofs to go"))
                    .arg(grouped(sum.addressesLeft), grouped(sum.transactionsLeft), grouped(sum.proofsLeft));
        if (!connected)
            said << trText(QStringLiteral("waiting for a server"));
    } else if (hasWallet && total > 0 && sum.refused == 0 && sum.unproven == 0 && sum.differing == 0 && controller_->figuresCurrent()) {
        // Only of what the servers have said: while they have not, or not of every address, it would
        // claim what nobody knows yet.
        said << trText(QStringLiteral("Every transaction is read, and what is in a block has its proof checked on this PC"));
    }
    if (sum.refused > 0) {
        said << trText(QStringLiteral("%1 addresses could not be read whole from this server")).arg(grouped(sum.refused));
        tone = Tone::Warning;
    }
    if (sum.unproven > 0) {
        said << trText(QStringLiteral("%1 proofs did not check out")).arg(grouped(sum.unproven));
        tone = Tone::Warning;
    }
    if (sum.differing > 0) {
        said << trText(QStringLiteral("%1 addresses do not add up to their balance")).arg(grouped(sum.differing));
        tone = Tone::Warning;
    }
    ui_->status->setText(said.join(QStringLiteral(" · ")));
    setTone(ui_->status, tone);
    ui_->status->setVisible(!said.isEmpty());

    // A network with no book: nothing below could say anything of it.
    const bool notRead = ui_->network && controller_ && !controller_->historyOf(*ui_->network);
    ui_->notRead->setVisible(notRead);
    for (QWidget* control : std::initializer_list<QWidget*>{ui_->search, ui_->type, ui_->state, ui_->period, ui_->count})
        control->setVisible(!notRead && (control != ui_->count || hasWallet));
    ui_->exportCsv->setVisible(!notRead);
    ui_->details->setVisible(!notRead);
    if (notRead) {
        ui_->status->hide();
        ui_->view->hide();
        ui_->noWallet->hide();
        ui_->waiting->hide();
        ui_->noResults->hide();
        return;
    }
    QWidget* empty = hasWallet ? ui_->waiting : ui_->noWallet;
    (hasWallet ? ui_->noWallet : ui_->waiting)->hide();
    const HistoryTableModel::Filter& filter = ui_->model->filter();
    const bool narrowed = filter.wallet != 0 || filter.kind >= 0 || filter.state >= 0 || filter.from > 0;
    page::showListState(ui_->view, empty, ui_->noResults, shown, hasWallet ? filter.query : QString(), hasWallet && narrowed && total > 0);
}

void HistoryPage::rememberPlace()
{
    if (ui_->placeHeld)
        return;
    QTableView* view = ui_->view;
    ui_->hadPlace = false;
    ui_->chosenBook = nullptr;
    ui_->chosenColumn = view->currentIndex().column();
    const QModelIndexList chosen = view->selectionModel()->selectedRows();
    if (!chosen.isEmpty()) {
        if (const HistoryTableModel::Line* line = ui_->model->lineAt(chosen.first().row())) {
            ui_->chosenBook = line->book;
            ui_->chosenTx = line->book->at(line->position).txid;
        }
    }
    if (view->verticalScrollBar()->value() > view->verticalScrollBar()->minimum()) {
        if (const HistoryTableModel::Line* line = ui_->model->lineAt(view->rowAt(0))) {
            ui_->topBook = line->book;
            ui_->topTx = line->book->at(line->position).txid;
            ui_->hadPlace = true;
        }
    }
}

void HistoryPage::restorePlace()
{
    ui_->placeHeld = false;
    QTableView* view = ui_->view;
    if (ui_->chosenBook) {
        const int row = ui_->model->rowOf(ui_->chosenBook, ui_->chosenTx);
        if (row >= 0) {
            // Chosen is all it is: the reader stays where the reader is.
            view->setAutoScroll(false);
            view->selectRow(row);
            // It moves the current cell to the row's first column, and Copy goes by that.
            if (ui_->chosenColumn >= 0)
                view->selectionModel()->setCurrentIndex(ui_->model->index(row, ui_->chosenColumn), QItemSelectionModel::NoUpdate);
            view->setAutoScroll(true);
        }
    }
    if (ui_->hadPlace) {
        const int row = ui_->model->rowOf(ui_->topBook, ui_->topTx);
        if (row >= 0) {
            // Down to that line, and no further along: it also scrolls sideways to its column. With
            // the list as long as it is now: its scroll bar is otherwise as long as it was.
            const int sideways = view->horizontalScrollBar()->value();
            view->doItemsLayout();
            view->scrollTo(ui_->model->index(row, 0), QAbstractItemView::PositionAtTop);
            view->horizontalScrollBar()->setValue(sideways);
        }
    }
}

void HistoryPage::openSelected()
{
    const QModelIndexList chosen = ui_->view->selectionModel()->selectedRows();
    if (!chosen.isEmpty())
        openRow(chosen.first().row());
}

void HistoryPage::openRow(int row)
{
    const HistoryTableModel::Line* line = ui_->model->lineAt(row);
    if (!line)
        return;
    auto* dialog = new TransactionDialog(controller_, ui_->model, *line, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void HistoryPage::applyColumnWidths()
{
    QTableView* view = ui_->view;
    QHeaderView* header = view->horizontalHeader();
    view->ensurePolished();
    header->ensurePolished();
    const QFontMetrics cell(view->font());
    const QFontMetrics head(header->font());
    const QStyle* style = view->style();
    const int textMargin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, view) + 1;
    const int cellPad = 2 * (page::kCellPadX + textMargin);
    const int headPad = 2 * page::kCellPadX + textMargin + style->pixelMetric(QStyle::PM_HeaderMarkSize, nullptr, header)
                        + 2 * style->pixelMetric(QStyle::PM_HeaderMargin, nullptr, header);
    const auto fit = [&](int column, const QStringList& samples) {
        int width = head.horizontalAdvance(ui_->model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString()) + headPad;
        for (const QString& sample : samples)
            width = qMax(width, cell.horizontalAdvance(sample) + cellPad + 2 * cell.averageCharWidth());
        header->setSectionResizeMode(column, QHeaderView::Interactive);
        header->resizeSection(column, width);
    };
    fit(HistoryTableModel::ColDate, {QStringLiteral("2026-12-31 23:59")});
    fit(HistoryTableModel::ColType, {trText(QStringLiteral("Received")), trText(QStringLiteral("Moved"))});
    fit(HistoryTableModel::ColWallet, {QStringLiteral("A wallet name")});
    fit(HistoryTableModel::ColAmount, {QStringLiteral("-1,234.12345678 BTC")});
    fit(HistoryTableModel::ColUsdNow, {QStringLiteral("$1,234,567.00")});
    fit(HistoryTableModel::ColUsdThen, {QStringLiteral("$1,234,567.00")});
    fit(HistoryTableModel::ColFee, {QStringLiteral("0.00012345 BTC")});
    fit(HistoryTableModel::ColState, {trText(QStringLiteral("In a block, proof on its way")), trText(QStringLiteral("In a block, proof did not hold")), trText(QStringLiteral("5 confirmations"))});
    fit(HistoryTableModel::ColTx, {QStringLiteral("00000000…00000000")});
    header->setSectionResizeMode(HistoryTableModel::ColLabel, QHeaderView::Stretch);
}

// ----------------------------------------------------------------- export ----

QString HistoryPage::headerLine() const
{
    QStringList titles{QStringLiteral("date"), QStringLiteral("type"), QStringLiteral("wallet"), QStringLiteral("amount"), QStringLiteral("coin"),
                       QStringLiteral("USD now"), QStringLiteral("USD then"), QStringLiteral("price then as of"), QStringLiteral("fee"),
                       QStringLiteral("state"), QStringLiteral("block"), QStringLiteral("label"), QStringLiteral("note"),
                       QStringLiteral("transaction"), QStringLiteral("addresses")};
    return titles.join(ui_->exportFormat.separator);
}

bool HistoryPage::writeLines(int from, int to, QString* error)
{
    const CsvFormat& format = ui_->exportFormat;
    QString text;
    for (int i = from; i < to; ++i) {
        const HistoryTableModel::ExportLine e = ui_->model->exportLine(ui_->exporting.at(i), ui_->exportFilter);
        const QStringList fields{csvField(e.date, format), csvField(e.kind, format), csvField(e.wallet, format),
                                 csvNumber(e.amount, format), csvField(e.coin, format), csvNumber(e.usdNow, format),
                                 csvNumber(e.usdThen, format), csvField(e.priceThenAt, format), csvNumber(e.fee, format),
                                 csvField(e.state, format), csvField(e.height, format), csvField(e.label, format),
                                 csvField(e.note, format), csvField(e.txid, format), csvField(e.addresses, format)};
        text += fields.join(format.separator);
        text += QStringLiteral("\r\n");
    }
    const QByteArray body = text.toUtf8();
    if (ui_->exportFile->write(body) != body.size()) {
        *error = trText(QStringLiteral("Could not write the file: %1")).arg(ui_->exportFile->errorString());
        return false;
    }
    return true;
}

bool HistoryPage::exportCsvNow(const QString& path, QString* error)
{
    QSaveFile file(path);
    ui_->exportFormat = csvFormat();
    const QByteArray head = QByteArray("\xEF\xBB\xBF") + headerLine().toUtf8() + "\r\n";
    if (!file.open(QIODevice::WriteOnly) || file.write(head) != head.size()) {
        *error = trText(QStringLiteral("Could not write the file: %1")).arg(file.errorString());
        return false;
    }
    const QVector<HistoryTableModel::Line> lines = ui_->model->lines();
    const HistoryTableModel::Filter filter = ui_->model->filter();
    const CsvFormat& format = ui_->exportFormat;
    for (const HistoryTableModel::Line& line : lines) {
        const HistoryTableModel::ExportLine e = ui_->model->exportLine(line, filter);
        const QStringList fields{csvField(e.date, format), csvField(e.kind, format), csvField(e.wallet, format),
                                 csvNumber(e.amount, format), csvField(e.coin, format), csvNumber(e.usdNow, format),
                                 csvNumber(e.usdThen, format), csvField(e.priceThenAt, format), csvNumber(e.fee, format),
                                 csvField(e.state, format), csvField(e.height, format), csvField(e.label, format),
                                 csvField(e.note, format), csvField(e.txid, format), csvField(e.addresses, format)};
        const QByteArray body = (fields.join(format.separator) + QStringLiteral("\r\n")).toUtf8();
        if (file.write(body) != body.size()) {
            *error = trText(QStringLiteral("Could not write the file: %1")).arg(file.errorString());
            return false;
        }
    }
    if (!file.commit()) {
        *error = trText(QStringLiteral("Could not write the file: %1")).arg(file.errorString());
        return false;
    }
    return true;
}

void HistoryPage::chooseExportFile()
{
    if (ui_->exportFile)
        return;
    const QString path = QFileDialog::getSaveFileName(this, trText(QStringLiteral("Export transactions")), QStringLiteral("gitwallet-transactions.csv"),
                                                      QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty())
        return;
    ui_->exportFormat = csvFormat();
    ui_->exportFile = std::make_unique<QSaveFile>(path);
    const QByteArray head = QByteArray("\xEF\xBB\xBF") + headerLine().toUtf8() + "\r\n";
    if (!ui_->exportFile->open(QIODevice::WriteOnly) || ui_->exportFile->write(head) != head.size()) {
        finishExport(false, trText(QStringLiteral("Could not write the file: %1")).arg(ui_->exportFile->errorString()));
        return;
    }
    ui_->exporting = ui_->model->lines();
    ui_->exportFilter = ui_->model->filter();
    ui_->exportAt = 0;
    updateState();
    QTimer::singleShot(0, this, &HistoryPage::exportStep);
}

void HistoryPage::exportStep()
{
    if (!ui_->exportFile)
        return;
    const int to = qMin(ui_->exportAt + kExportChunk, int(ui_->exporting.size()));
    QString error;
    if (!writeLines(ui_->exportAt, to, &error)) {
        finishExport(false, error);
        return;
    }
    ui_->exportAt = to;
    ui_->exportCsv->setText(trText(QStringLiteral("Exporting %1%")).arg(100 * qint64(to) / qMax(qsizetype(1), ui_->exporting.size())));
    if (to >= ui_->exporting.size()) {
        const bool ok = ui_->exportFile->commit();
        finishExport(ok, ok ? QString() : trText(QStringLiteral("Could not write the file: %1")).arg(ui_->exportFile->errorString()));
        return;
    }
    QTimer::singleShot(0, this, &HistoryPage::exportStep);
}

void HistoryPage::finishExport(bool ok, const QString& error)
{
    ui_->exportCsv->setText(trText(QStringLiteral("Export CSV")));
    ui_->exportFile.reset();
    ui_->exporting.clear();
    updateState();
    if (ok)
        Feedback::show(ui_->exportCsv, Feedback::Type::Success, trText(QStringLiteral("Transactions exported")));
    else
        Feedback::show(this, Feedback::Type::Error, error);
}

} // namespace app::ui
