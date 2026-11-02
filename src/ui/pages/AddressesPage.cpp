#include "ui/pages/AddressesPage.h"

#include "app/WalletController.h"
#include "net/PriceFeed.h"
#include "ui/AddressTableModel.h"
#include "ui/Csv.h"
#include "ui/DialogChrome.h"
#include "ui/Feedback.h"
#include "ui/HoverRowTable.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/TableUtil.h"
#include "ui/ThemeManager.h"
#include "ui/design/Style.h"
#include "wallet/Types.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFileDialog>
#include <QFontMetrics>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>

namespace app::ui {

namespace {

// What the chain filter holds when it shows every network.
constexpr int kAllChains = -1;
// Combo indices of the state filter.
enum StateFilter { StateAll, StateFunded, StateUsed };

// Refresh calls CoinGecko and the public RPC endpoints, which rate-limit;
// the same pause as the Overview page's button.
constexpr int kRefreshCooldownMs = 3000;
// Past this many rows a change of filter rebuilds the list and search waits for the
// typing to stop: each costs too much to do row by row or key by key.
constexpr int kRebuildAbove = 50000;

// Narrows the table to rows whose address or path contains the search text,
// on one chain, in one state. Sorting is the base class's, on the model's
// SortRole (numeric for path, balance, USD and state).
class AddressFilter : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setQuery(const QString& query)
    {
        if (query == query_)
            return;
        change([&] {
            query_ = query;
            static const QRegularExpression pathLike(QStringLiteral("^[mM0-9'/]*$"));
            pathQuery_ = pathLike.match(query).hasMatch();
        });
    }
    void setChain(int chain)
    {
        if (chain != chain_)
            change([&] { chain_ = chain; });
    }
    void setState(int state)
    {
        if (state != state_)
            change([&] { state_ = state; });
    }
    void setWallet(quint32 wallet)
    {
        if (wallet != wallet_)
            change([&] { wallet_ = wallet; });
    }
    // With the state Funded, it is this asset the row has to hold.
    void setHeld(std::optional<wallet::Asset> asset, const wallet::WalletModel* model)
    {
        if (asset != held_) {
            change([&] {
                held_ = asset;
                model_ = model;
            });
        }
    }
    // A combo narrows the list (the search text is reported separately).
    bool filtered() const { return wallet_ != 0 || chain_ != kAllChains || state_ != StateAll; }
    // The search text or a combo hides rows; otherwise every row passes.
    bool narrowed() const { return filtered() || !query_.isEmpty(); }
    bool accepts(int sourceRow) const { return filterAcceptsRow(sourceRow, QModelIndex()); }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        const QAbstractItemModel* source = sourceModel();
        if (!source)
            return false;
        const QModelIndex first = source->index(sourceRow, 0, sourceParent);
        if (wallet_ != 0 && first.data(AddressTableModel::WalletRole).toUInt() != wallet_)
            return false;
        if (chain_ != kAllChains && first.data(AddressTableModel::NetworkRole).toInt() != chain_)
            return false;
        if (state_ != StateAll) {
            const auto state =
                static_cast<wallet::AddressState>(first.data(AddressTableModel::StateRole).toInt());
            // Used includes Funded: an address holding coins has received them.
            const bool funded = state == wallet::AddressState::Funded;
            if (state_ == StateFunded ? !funded : !(funded || state == wallet::AddressState::Used))
                return false;
            if (state_ == StateFunded && held_ && model_ && sourceRow < model_->size() && model_->at(sourceRow).amount(*held_).isZero())
                return false;
        }
        if (query_.isEmpty())
            return true;
        if (first.data(AddressTableModel::AddressRole).toString().contains(query_, Qt::CaseInsensitive))
            return true;
        if (source->index(sourceRow, AddressTableModel::ColLabel, sourceParent).data().toString().contains(query_, Qt::CaseInsensitive))
            return true;
        // A path only has so many kinds of character, and building one costs more than the search.
        return pathQuery_
               && source->index(sourceRow, AddressTableModel::ColPath, sourceParent).data().toString().contains(query_, Qt::CaseInsensitive);
    }

private:
    template <typename Apply>
    void change(Apply apply)
    {
        // Row by row the filter pays for every stretch of rows it keeps or drops, each
        // as long as the rows after it: tens of seconds for a query that keeps every third
        // of a large wallet's rows. Thrown away, it reads them all once and sorts once.
        const QAbstractItemModel* source = sourceModel();
        if (source && source->rowCount() > kRebuildAbove) {
            apply();
            invalidate();
            return;
        }
        beginFilterChange();
        apply();
        endFilterChange(Direction::Rows);
    }

    bool pathQuery_ = true;
    QString query_;
    quint32 wallet_ = 0; // 0: every wallet
    int chain_ = kAllChains;
    int state_ = StateAll;
    std::optional<wallet::Asset> held_;
    const wallet::WalletModel* model_ = nullptr;
};

// The row's amount of `info` in whole units, every digit kept; empty for an asset
// of another network, and for a zero that may only mean "no answer yet".
QString csvAmount(const wallet::AddressRow& row, const wallet::AssetInfo& info, const CsvFormat& format)
{
    if (info.network != row.network)
        return QString();
    const chain::Amount amount = row.amount(info.asset);
    if (amount.isZero() && !row.checked)
        return QString();
    return csvNumber(QString::fromStdString(amount.format(info.decimals, info.decimals)), format);
}

// A column of the export: the symbol, with its network where two networks share it.
QString csvTitle(const wallet::AssetInfo& info)
{
    const QList<wallet::AssetInfo>& all = wallet::assets();
    const auto same = std::count_if(all.cbegin(), all.cend(), [&info](const wallet::AssetInfo& other) {
        return other.symbol == info.symbol;
    });
    return same > 1 ? QStringLiteral("%1 %2").arg(info.symbol, wallet::networkInfo(info.network).name) : info.symbol;
}

// "500 addresses · 3 funded", or "12 of 500 addresses · 1 funded" while a
// filter hides some.
QString countText(int shown, int total, int funded)
{
    // English-first like the rest of the UI: "12,345" whatever the Windows format.
    static const QLocale english(QLocale::English, QLocale::UnitedStates);
    const QString noun = trText(total == 1 ? QStringLiteral("address") : QStringLiteral("addresses"));
    const QString count = shown == total ? english.toString(total)
                                         : trText(QStringLiteral("%1 of %2"))
                                               .arg(english.toString(shown), english.toString(total));
    return trText(QStringLiteral("%1 %2 · %3 funded")).arg(count, noun, english.toString(funded));
}

// Funded rows among the first `rows` of the wallet model, the ones the table
// has announced: the model's own count, less the rows appended since (they
// reach the table on the next event-loop turn). No walk over every row.
int fundedAmong(const wallet::WalletModel* model, int rows)
{
    if (!model)
        return 0;
    int funded = model->fundedCount();
    for (int i = rows; i < model->size(); ++i) {
        if (model->at(i).state() == wallet::AddressState::Funded)
            --funded;
    }
    return funded;
}

// The glyph is a pixmap in the text colour of the theme it was drawn for.
QIcon refreshIcon()
{
    return makeIcon(icon::Kind::Refresh, page::text(), 15);
}

} // namespace

// How long search waits for the next key, on a list this large.
constexpr int kSearchPauseMs = 250;

struct AddressesPage::Ui : QObject
{
    using QObject::QObject;

    AddressTableModel* model = nullptr;
    AddressFilter* filter = nullptr;
    HoverRowTableView* view = nullptr;
    QLineEdit* search = nullptr;
    QComboBox* wallet = nullptr;
    QComboBox* chain = nullptr;
    QComboBox* state = nullptr;
    QLabel* count = nullptr;
    QPushButton* exportCsv = nullptr;
    QPushButton* refresh = nullptr;
    QPushButton* history = nullptr;
    QPushButton* label = nullptr;
    QWidget* noWallet = nullptr; // first run: no wallet added yet
    QWidget* waiting = nullptr;  // a wallet whose first addresses are not derived yet
    QWidget* noResults = nullptr;
    QTimer* update = nullptr;    // batches recounts while a scan streams rows in
    QTimer* searchPause = nullptr; // on a large list, the query waits for the typing to stop
    QTimer* cooldown = nullptr;  // Refresh stays disabled while it runs
    bool inPlace = false;        // the wallet and chain come from the place, and their combos are gone
};

AddressesPage::AddressesPage(WalletController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller)
{
    ui_ = new Ui(this);
    auto* root = new QVBoxLayout(this);
    page::applyPageLayout(root);

    ui_->model = new AddressTableModel(controller_, this);
    ui_->filter = new AddressFilter(this);
    ui_->filter->setSourceModel(ui_->model);
    ui_->filter->setSortRole(AddressTableModel::SortRole);
    ui_->filter->setSortCaseSensitivity(Qt::CaseInsensitive);

    auto* view = new HoverRowTableView(this);
    ui_->view = view;
    view->setModel(ui_->filter);
    page::configureTable(view);
    view->setMouseTracking(true); // the hover band follows the pointer
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideMiddle); // a cut address keeps both ends
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    QHeaderView* header = view->horizontalHeader();
    // configureTable() makes headers inert; this list sorts on a header click (a third
    // click returns to the order the rows came in). Path order by default: each chain's
    // addresses together, by index.
    header->setSectionsClickable(true);
    header->setSortIndicatorClearable(true);
    header->setHighlightSections(false);
    header->setStretchLastSection(false);
    header->setSortIndicator(AddressTableModel::ColPath, Qt::AscendingOrder);
    view->setSortingEnabled(true);
    installCopyOnClick(view, {AddressTableModel::ColAddress});
    applyColumnWidths();
    root->addWidget(view, 1);

    ui_->noWallet = page::makeEmptyState(this, int(icon::Kind::ListRows), QStringLiteral("No wallet yet"),
                                         QStringLiteral("Add a wallet on the Wallets page."));
    ui_->waiting = page::makeEmptyState(this, int(icon::Kind::Refresh), QStringLiteral("Scanning addresses"),
                                        QStringLiteral("Addresses appear here as GitWallet derives and checks them."));
    ui_->noResults = page::makeNoResultsState(this);
    root->addWidget(ui_->noWallet, 1);
    root->addWidget(ui_->waiting, 1);
    root->addWidget(ui_->noResults, 1);

    const page::ActionBar bar = page::makeActionBar(this);
    ui_->search = page::makeSearch(bar.widget, QStringLiteral("Search address, label or path"));
    ui_->wallet = new QComboBox(bar.widget);
    ui_->wallet->setObjectName(QStringLiteral("walletFilter"));
    ui_->wallet->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ui_->wallet->setMaximumWidth(page::kFieldMaxW / 2); // a long name is cut, not the toolbar
    ui_->chain = new QComboBox(bar.widget);
    ui_->chain->addItem(QStringLiteral("All chains"), kAllChains);
    for (const wallet::NetworkInfo& network : wallet::networks())
        ui_->chain->addItem(network.name, int(network.network));
    ui_->chain->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ui_->state = new QComboBox(bar.widget);
    ui_->state->setObjectName(QStringLiteral("stateFilter"));
    ui_->state->addItems({QStringLiteral("All states"), QStringLiteral("Funded"), QStringLiteral("Used")});
    ui_->state->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ui_->state->setToolTip(QStringLiteral("Funded: holds a balance now. Used: has transactions or a balance."));
    ui_->count = page::makeBarCount(bar.widget);
    bar.left->addWidget(ui_->search);
    bar.left->addWidget(ui_->wallet);
    bar.left->addWidget(ui_->chain);
    bar.left->addWidget(ui_->state);
    page::finishBarLeft(bar, ui_->search, ui_->count);

    ui_->exportCsv = new QPushButton(QStringLiteral("Export CSV"), bar.widget);
    ui_->exportCsv->setToolTip(QStringLiteral("Save the rows shown to a spreadsheet file. It is not encrypted: "
                                              "anyone who reads it sees these addresses and balances."));
    bar.right->addWidget(ui_->exportCsv);
    ui_->refresh = new QPushButton(QStringLiteral("Refresh"), bar.widget);
    ui_->refresh->setToolTip(QStringLiteral("Fetch prices and read the balances that are not followed live, now"));
    page::setLabeledIcon(ui_->refresh, refreshIcon());
    bar.right->addWidget(ui_->refresh);
    ui_->label = new QPushButton(QStringLiteral("Label"), bar.widget);
    ui_->label->setToolTip(trText(QStringLiteral("A name for this address that only this PC knows")));
    bar.right->addWidget(ui_->label);
    ui_->history = new QPushButton(QStringLiteral("History"), bar.widget);
    ui_->history->setObjectName(QStringLiteral("primaryButton"));
    ui_->history->setToolTip(trText(QStringLiteral("Every transaction of this address")));
    bar.right->addWidget(ui_->history);
    root->addWidget(bar.widget);

    ui_->update = new QTimer(this);
    ui_->update->setSingleShot(true);
    ui_->update->setInterval(100);
    connect(ui_->update, &QTimer::timeout, this, &AddressesPage::updateState);
    ui_->cooldown = new QTimer(this);
    ui_->cooldown->setSingleShot(true);
    ui_->cooldown->setInterval(kRefreshCooldownMs);
    connect(ui_->cooldown, &QTimer::timeout, this, &AddressesPage::updateState);

    ui_->searchPause = new QTimer(this);
    ui_->searchPause->setSingleShot(true);
    ui_->searchPause->setInterval(kSearchPauseMs);
    connect(ui_->searchPause, &QTimer::timeout, this, [this] {
        ui_->filter->setQuery(ui_->search->text().trimmed());
        updateState();
    });
    connect(ui_->search, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (ui_->model->rowCount() > kRebuildAbove) {
            ui_->searchPause->start();
            return;
        }
        ui_->searchPause->stop();
        ui_->filter->setQuery(text.trimmed());
        updateState();
    });
    connect(ui_->wallet, &QComboBox::currentIndexChanged, this, [this](int) {
        ui_->filter->setWallet(ui_->wallet->currentData().toUInt());
        updateState();
    });
    connect(ui_->chain, &QComboBox::currentIndexChanged, this, [this](int) {
        ui_->filter->setChain(ui_->chain->currentData().toInt());
        updateState();
    });
    connect(ui_->state, &QComboBox::currentIndexChanged, this, [this](int index) {
        ui_->filter->setState(index);
        updateState();
    });
    connect(ui_->refresh, &QPushButton::clicked, this, [this] {
        if (!controller_)
            return;
        controller_->refresh();
        Feedback::show(ui_->refresh, Feedback::Type::Info,
                       trText(QStringLiteral("Refreshing prices and balances")));
        ui_->cooldown->start();
        ui_->refresh->setEnabled(false);
    });
    connect(ui_->exportCsv, &QPushButton::clicked, this, &AddressesPage::chooseExportFile);
    connect(view, &QAbstractItemView::doubleClicked, this, &AddressesPage::openAddressOf);
    connect(ui_->history, &QPushButton::clicked, this, [this] {
        const QModelIndexList rows = ui_->view->selectionModel()->selectedRows();
        if (!rows.isEmpty())
            openHistoryOf(rows.first());
    });
    connect(ui_->label, &QPushButton::clicked, this, &AddressesPage::editLabelOfSelected);
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &AddressesPage::scheduleUpdate);

    // Scan results arrive as inserts and single-row changes, hundreds a second:
    // recount in batches.
    connect(ui_->filter, &QAbstractItemModel::rowsInserted, this, &AddressesPage::scheduleUpdate);
    connect(ui_->filter, &QAbstractItemModel::rowsRemoved, this, &AddressesPage::scheduleUpdate);
    connect(ui_->filter, &QAbstractItemModel::dataChanged, this, &AddressesPage::scheduleUpdate);
    connect(ui_->filter, &QAbstractItemModel::modelReset, this, &AddressesPage::updateState);
    // Connected after setModel(), so the view has already taken the insert.
    connect(ui_->filter, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, int first, int last) { keepTopRow(first, last); });

    connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed, this, [this] {
        page::setLabeledIcon(ui_->refresh, refreshIcon());
        applyColumnWidths(); // the font size may have changed
    });
    // Header titles and the count are runtime text (trText).
    connect(LanguageManager::instance(), &LanguageManager::changed, this, [this] {
        applyColumnWidths();
        updateState();
    });
    if (controller_) {
        connect(controller_, &WalletController::walletChanged, this, [this] {
            rebuildWalletFilter();
            updateState();
        });
        // The count says when the figures are saved ones, and export waits for the load.
        connect(controller_, &WalletController::statusChanged, this, &AddressesPage::scheduleUpdate);
    }
    rebuildWalletFilter();
    updateState();
}

void AddressesPage::rebuildWalletFilter()
{
    const quint32 chosen = ui_->wallet->currentData().toUInt();
    const QList<wallet::WalletEntry> wallets =
        controller_ ? controller_->store().wallets : QList<wallet::WalletEntry>();
    {
        const QSignalBlocker quiet(ui_->wallet);
        ui_->wallet->clear();
        ui_->wallet->addItem(QStringLiteral("All wallets"), quint32(0));
        for (const wallet::WalletEntry& w : wallets)
            ui_->wallet->addItem(w.label, w.id);
        const int keep = ui_->wallet->findData(chosen);
        ui_->wallet->setCurrentIndex(keep >= 0 ? keep : 0); // a wallet removed: back to all
        // LanguageManager keeps a combo's first texts and writes them back by
        // position; these are new, and it reads them afresh.
        ui_->wallet->setProperty("i18nOrig_items", QVariant());
    }
    ui_->filter->setWallet(ui_->wallet->currentData().toUInt());
    ui_->wallet->setVisible(wallets.size() > 1 && !ui_->inPlace);
    if (!ui_->inPlace)
        ui_->view->setColumnHidden(AddressTableModel::ColWallet, wallets.size() < 2);
}

void AddressesPage::showWallet(quint32 id)
{
    const int index = ui_->wallet->findData(id);
    ui_->wallet->setCurrentIndex(index >= 0 ? index : 0);
}

void AddressesPage::showPlace(quint32 wallet, std::optional<wallet::Asset> asset)
{
    ui_->inPlace = true;
    ui_->filter->setHeld(asset, controller_ ? controller_->model() : nullptr);
    {
        const QSignalBlocker quietWallet(ui_->wallet);
        const QSignalBlocker quietChain(ui_->chain);
        const QSignalBlocker quietState(ui_->state);
        const QSignalBlocker quietSearch(ui_->search);
        const int walletAt = ui_->wallet->findData(wallet);
        ui_->wallet->setCurrentIndex(walletAt >= 0 ? walletAt : 0);
        const int chainAt = asset ? ui_->chain->findData(int(wallet::assetInfo(*asset).network)) : 0;
        ui_->chain->setCurrentIndex(chainAt >= 0 ? chainAt : 0);
        ui_->state->setCurrentIndex(asset ? StateFunded : StateAll);
        ui_->search->clear();
    }
    ui_->filter->setQuery(QString());
    ui_->filter->setWallet(wallet);
    ui_->filter->setChain(ui_->chain->currentData().toInt());
    ui_->filter->setState(ui_->state->currentIndex());
    ui_->wallet->setVisible(false);
    ui_->chain->setVisible(!asset.has_value());
    ui_->view->setColumnHidden(AddressTableModel::ColWallet, wallet != 0 || (controller_ && controller_->store().wallets.size() < 2));
    ui_->view->setColumnHidden(AddressTableModel::ColChain, asset.has_value());
    updateState();
}

void AddressesPage::clearPlace()
{
    if (!ui_->inPlace)
        return;
    ui_->inPlace = false;
    ui_->filter->setHeld(std::nullopt, nullptr);
    {
        const QSignalBlocker quietWallet(ui_->wallet);
        const QSignalBlocker quietChain(ui_->chain);
        const QSignalBlocker quietState(ui_->state);
        const QSignalBlocker quietSearch(ui_->search);
        ui_->wallet->setCurrentIndex(0);
        ui_->chain->setCurrentIndex(0);
        ui_->state->setCurrentIndex(StateAll);
        ui_->search->clear();
    }
    ui_->filter->setQuery(QString());
    ui_->filter->setWallet(0);
    ui_->filter->setChain(kAllChains);
    ui_->filter->setState(StateAll);
    ui_->chain->setVisible(true);
    ui_->view->setColumnHidden(AddressTableModel::ColChain, false);
    rebuildWalletFilter();
    updateState();
}

void AddressesPage::openHistoryOf(const QModelIndex& index)
{
    const QModelIndex source = ui_->filter->mapToSource(index);
    const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
    if (!model || !source.isValid() || source.row() >= model->size())
        return;
    emit showHistoryRequested(model->at(source.row()));
}

void AddressesPage::openAddressOf(const QModelIndex& index)
{
    const QModelIndex source = ui_->filter->mapToSource(index);
    const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
    if (!model || !source.isValid() || source.row() >= model->size())
        return;
    emit openAddressRequested(model->at(source.row()));
}

void AddressesPage::editLabelOfSelected()
{
    const QModelIndexList rows = ui_->view->selectionModel()->selectedRows();
    const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
    if (rows.isEmpty() || !model)
        return;
    const QModelIndex source = ui_->filter->mapToSource(rows.first());
    if (!source.isValid() || source.row() >= model->size())
        return;
    editLabel(model->at(source.row()).address);
}

void AddressesPage::editLabel(const QString& address)
{
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("addressLabelDialog"));
    dialog->setWindowTitle(trText(QStringLiteral("Label")));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    applyDialogChrome(dialog);
    auto* column = new QVBoxLayout(dialog);
    page::applyDialogLayout(column);
    auto* where = new QLabel(address, dialog);
    where->setTextFormat(Qt::PlainText);
    where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    column->addWidget(where);
    auto* name = new QLineEdit(controller_->addressLabel(address), dialog);
    name->setObjectName(QStringLiteral("addressLabel"));
    name->setMaxLength(1024);
    name->setPlaceholderText(trText(QStringLiteral("A customer, an order, a till")));
    name->selectAll();
    column->addWidget(name);
    auto* status = new QLabel(dialog);
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    setStatus(status, QString(), Tone::Muted);
    column->addWidget(status);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(page::kToolbarSpacing);
    buttons->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), dialog);
    auto* save = new QPushButton(QStringLiteral("Save"), dialog);
    save->setObjectName(QStringLiteral("primaryButton"));
    save->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(save);
    column->addLayout(buttons);
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, dialog, [this, dialog, name, status, address] {
        QString error;
        if (controller_->setAddressLabel(address, name->text(), &error)) {
            dialog->accept();
            return;
        }
        setStatus(status, error, Tone::Error);
    });
    page::fitDialog(dialog, page::DialogSize::Compact);
    dialog->open();
    name->setFocus();
}

void AddressesPage::chooseExportFile()
{
    const QString path = QFileDialog::getSaveFileName(
        this, trText(QStringLiteral("Export addresses")), QStringLiteral("gitwallet-addresses.csv"),
        QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty())
        return;
    QString error;
    if (exportCsv(path, &error))
        Feedback::show(ui_->exportCsv, Feedback::Type::Success,
                       controller_ && controller_->showingSaved()
                           ? trText(QStringLiteral("Addresses exported, with saved balances still being checked"))
                           : trText(QStringLiteral("Addresses exported")));
    else
        Feedback::show(this, Feedback::Type::Error, error);
}

bool AddressesPage::exportCsv(const QString& path, QString* error) const
{
    const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
    const net::PriceFeed* prices = controller_ ? controller_->prices() : nullptr;
    const CsvFormat format = csvFormat();
    QStringList lines;
    QStringList titles{QStringLiteral("wallet"), QStringLiteral("chain"), QStringLiteral("type"),
                       QStringLiteral("path"), QStringLiteral("address"), QStringLiteral("label"), QStringLiteral("state")};
    for (const wallet::AssetInfo& info : wallet::assets())
        titles << csvTitle(info);
    titles << QStringLiteral("USD") << QStringLiteral("balances as of") << QStringLiteral("prices as of");
    lines << titles.join(format.separator);
    // When each network's figures are from, and whether this run has checked them at all.
    QHash<wallet::Network, QString> asOf;
    for (const wallet::NetworkInfo& network : wallet::networks()) {
        const wallet::NetworkStatus status = controller_ ? controller_->networkStatus(network.network) : wallet::NetworkStatus();
        QString text = status.balancesAt.isValid() ? status.balancesAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
        if (controller_ && controller_->showingSaved() && !status.answered)
            text = (QStringLiteral("saved ") + text).trimmed();
        asOf.insert(network.network, text);
    }
    for (int row = 0; model && row < ui_->filter->rowCount(); ++row) {
        const QModelIndex first = ui_->filter->mapToSource(ui_->filter->index(row, 0));
        if (first.row() >= model->size())
            continue;
        const wallet::AddressRow& r = model->at(first.row());
        const auto cell = [&](int column) { return first.siblingAtColumn(column).data().toString(); };
        QStringList fields;
        fields << csvField(cell(AddressTableModel::ColWallet), format)
               << csvField(cell(AddressTableModel::ColChain), format)
               << csvField(cell(AddressTableModel::ColType), format)
               << csvField(r.path(), format)
               << csvField(r.address, format)
               << csvField(cell(AddressTableModel::ColLabel), format)
               << csvField(cell(AddressTableModel::ColState), format);
        double usd = 0.0;
        // Only once every balance of the row has answered: a sum of this column
        // must not silently leave out an amount still unknown.
        bool priced = r.checked;
        QList<wallet::Asset> held;
        try {
            for (const wallet::AssetInfo& info : wallet::assets()) {
                fields << csvAmount(r, info, format);
                const chain::Amount amount = r.amount(info.asset);
                if (amount.isZero())
                    continue;
                held.append(info.asset);
                const double price = prices ? prices->usd(info.asset) : 0.0;
                priced = priced && price > 0.0;
                usd += amount.toDouble(info.decimals) * price;
            }
        } catch (const std::exception&) {
            *error = trText(QStringLiteral("An amount could not be read; nothing was exported."));
            return false;
        }
        fields << (priced ? csvNumber(QString::number(usd, 'f', 2), format) : QString());
        fields << csvField(asOf.value(r.network), format);
        const QDateTime pricedAt = prices && !held.isEmpty() ? prices->oldest(held) : QDateTime();
        fields << (pricedAt.isValid() ? csvField(pricedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")), format) : QString());
        lines << fields.join(format.separator);
    }

    QSaveFile file(path);
    const QByteArray body = QByteArray("\xEF\xBB\xBF") + lines.join(QStringLiteral("\r\n")).toUtf8() + "\r\n";
    if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size() || !file.commit()) {
        *error = trText(QStringLiteral("Could not write the file: %1")).arg(file.errorString());
        return false;
    }
    return true;
}

void AddressesPage::applyColumnWidths()
{
    // Each column fits its longest usual value in the table's own font (QSS);
    // Balance takes the rest. An address that still does not fit is cut in
    // the middle and shown whole in its tooltip; columns stay resizable.
    QTableView* view = ui_->view;
    QHeaderView* header = view->horizontalHeader();
    view->ensurePolished();
    header->ensurePolished();
    const QFontMetrics cell(view->font());
    const QFontMetrics head(header->font());
    const QStyle* style = view->style();
    // Cells: the QSS item padding plus Qt's text margin, both sides.
    const int textMargin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, view) + 1;
    const int cellPad = 2 * (page::kCellPadX + textMargin);
    // Headers: the QSS section padding plus room for the sort arrow.
    const int headPad = 2 * page::kCellPadX + textMargin
                        + style->pixelMetric(QStyle::PM_HeaderMarkSize, nullptr, header)
                        + 2 * style->pixelMetric(QStyle::PM_HeaderMargin, nullptr, header);

    const auto fit = [&](int column, const QStringList& samples, int slack) {
        int width = head.horizontalAdvance(
                        ui_->model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString())
                    + headPad;
        for (const QString& sample : samples)
            width = qMax(width, cell.horizontalAdvance(sample) + cellPad + slack);
        header->setSectionResizeMode(column, QHeaderView::Interactive);
        header->resizeSection(column, width);
    };
    fit(AddressTableModel::ColWallet, {QStringLiteral("A wallet name")}, 0);
    fit(AddressTableModel::ColChain, {QStringLiteral("Bitcoin"), QStringLiteral("Ethereum")}, 0);
    fit(AddressTableModel::ColType,
        {QStringLiteral("Native SegWit"), QStringLiteral("Nested SegWit"), QStringLiteral("Account")}, 0);
    fit(AddressTableModel::ColPath,
        {QStringLiteral("m/84'/0'/0'/1/00000"), QStringLiteral("m/44'/60'/0'/0/00000")}, 0);
    // Addresses differ in glyph widths: the two public test vectors plus slack.
    fit(AddressTableModel::ColAddress,
        {QStringLiteral("bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu"),
         QStringLiteral("0x9858EfFD232B4033E47d90003D41EC34EcaEda94")},
        2 * cell.averageCharWidth());
    fit(AddressTableModel::ColLabel, {QStringLiteral("A label for it")}, 0);
    fit(AddressTableModel::ColUsd, {QStringLiteral("$00,000,000.00")}, 0);
    fit(AddressTableModel::ColState, {QStringLiteral("Checking…"), QStringLiteral("Funded")}, 0);
    header->setSectionResizeMode(AddressTableModel::ColBalance, QHeaderView::Stretch);
}

void AddressesPage::keepTopRow(int first, int last)
{
    // Rows inserted above the first visible one (a sorted list while a scan
    // streams addresses in) would push what the user is reading down. At the
    // very top the new rows may show; otherwise scroll along by what came in.
    QTableView* view = ui_->view;
    QScrollBar* bar = view->verticalScrollBar();
    if (bar->value() <= bar->minimum())
        return;
    // The pixel offset has not moved yet, so this is the index of the row
    // that was on top before the insert.
    const int top = view->rowAt(0);
    if (top < 0 || first > top)
        return;
    const int inserted = last - first + 1;
    view->doItemsLayout(); // the scroll range must count the new rows first
    if (view->verticalScrollMode() == QAbstractItemView::ScrollPerItem)
        bar->setValue(bar->value() + inserted);
    else
        bar->setValue(bar->value() + view->rowViewportPosition(top + inserted)
                      - view->rowViewportPosition(top));
}

void AddressesPage::scheduleUpdate()
{
    if (!ui_->update->isActive())
        ui_->update->start();
}

void AddressesPage::updateState()
{
    ui_->update->stop();
    const bool hasWallet = controller_ && controller_->hasWallet();
    ui_->search->setEnabled(hasWallet);
    ui_->wallet->setEnabled(hasWallet);
    ui_->chain->setEnabled(hasWallet);
    ui_->state->setEnabled(hasWallet);
    ui_->refresh->setEnabled(hasWallet && !ui_->cooldown->isActive());
    const bool chosen = !ui_->view->selectionModel()->selectedRows().isEmpty();
    ui_->history->setEnabled(chosen);
    ui_->label->setEnabled(chosen);

    const int shown = ui_->filter->rowCount();
    // Not part of what the database kept, with nothing to say so.
    ui_->exportCsv->setEnabled(shown > 0 && !(controller_ && controller_->loadingSaved()));
    const int total = ui_->model->rowCount();
    int funded = 0;
    if (!ui_->filter->narrowed()) {
        // Every row shows: the model counts them, so a scan streaming in
        // thousands of rows costs no recount per burst.
        funded = fundedAmong(controller_ ? controller_->model() : nullptr, total);
    } else if (const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr) {
        // Few rows hold anything: the filter is only asked about those.
        for (int row = 0; row < total && row < model->size(); ++row) {
            if (model->at(row).state() == wallet::AddressState::Funded && ui_->filter->accepts(row))
                ++funded;
        }
    }
    QString countLine = hasWallet ? countText(shown, total, funded) : QString();
    if (hasWallet && controller_->showingSaved())
        countLine += QLatin1String(" · ") + trText(QStringLiteral("saved, being checked"));
    ui_->count->setText(countLine);
    ui_->count->setVisible(hasWallet);

    // Without a wallet a leftover search or filter must not turn the
    // first-run state into "No results".
    QWidget* empty = hasWallet ? ui_->waiting : ui_->noWallet;
    (hasWallet ? ui_->noWallet : ui_->waiting)->hide();
    page::showListState(ui_->view, empty, ui_->noResults, shown,
                        hasWallet ? ui_->search->text().trimmed() : QString(),
                        hasWallet && ui_->filter->filtered());
}

} // namespace app::ui
