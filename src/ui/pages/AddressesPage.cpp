#include "ui/pages/AddressesPage.h"

#include "app/WalletController.h"
#include "ui/AddressTableModel.h"
#include "ui/Feedback.h"
#include "ui/HoverRowTable.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/TableUtil.h"
#include "ui/ThemeManager.h"
#include "wallet/Types.h"
#include "wallet/WalletModel.h"

#include <QComboBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace app::ui {

namespace {

// Combo indices of the two filters.
enum ChainFilter { ChainAll, ChainBitcoin, ChainEthereum };
enum StateFilter { StateAll, StateFunded, StateUsed };

// Refresh calls CoinGecko and the public RPC endpoints, which rate-limit;
// the same pause as the Overview page's button.
constexpr int kRefreshCooldownMs = 3000;

// Narrows the table to rows whose address or path contains the search text,
// on one chain, in one state. Sorting is the base class's, on the model's
// SortRole (numeric for path, balance, USD and state).
class AddressFilter : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setQuery(const QString& query)
    {
        if (query != query_)
            change([&] { query_ = query; });
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
    // A combo narrows the list (the search text is reported separately).
    bool filtered() const { return chain_ != ChainAll || state_ != StateAll; }
    // The search text or a combo hides rows; otherwise every row passes.
    bool narrowed() const { return filtered() || !query_.isEmpty(); }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        const QAbstractItemModel* source = sourceModel();
        if (!source)
            return false;
        const QModelIndex first = source->index(sourceRow, 0, sourceParent);
        if (chain_ != ChainAll
            && first.data(AddressTableModel::IsBitcoinRole).toBool() != (chain_ == ChainBitcoin))
            return false;
        if (state_ != StateAll) {
            const auto state =
                static_cast<wallet::AddressState>(first.data(AddressTableModel::StateRole).toInt());
            // Used includes Funded: an address holding coins has received them.
            const bool funded = state == wallet::AddressState::Funded;
            if (state_ == StateFunded ? !funded : !(funded || state == wallet::AddressState::Used))
                return false;
        }
        if (query_.isEmpty())
            return true;
        const QString address = first.data(AddressTableModel::AddressRole).toString();
        const QString path =
            source->index(sourceRow, AddressTableModel::ColPath, sourceParent).data().toString();
        return address.contains(query_, Qt::CaseInsensitive) || path.contains(query_, Qt::CaseInsensitive);
    }

private:
    template <typename Apply>
    void change(Apply apply)
    {
        beginFilterChange();
        apply();
        endFilterChange(Direction::Rows);
    }

    QString query_;
    int chain_ = ChainAll;
    int state_ = StateAll;
};

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

struct AddressesPage::Ui : QObject
{
    using QObject::QObject;

    AddressTableModel* model = nullptr;
    AddressFilter* filter = nullptr;
    HoverRowTableView* view = nullptr;
    QLineEdit* search = nullptr;
    QComboBox* chain = nullptr;
    QComboBox* state = nullptr;
    QLabel* count = nullptr;
    QPushButton* refresh = nullptr;
    QWidget* noWallet = nullptr; // first run: no seed imported yet
    QWidget* waiting = nullptr;  // a wallet whose first addresses are not derived yet
    QWidget* noResults = nullptr;
    QTimer* update = nullptr;    // batches recounts while a scan streams rows in
    QTimer* cooldown = nullptr;  // Refresh stays disabled while it runs
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
    // configureTable() makes headers inert; this list sorts on a header click
    // (a third click returns to the scan order). Path order by default: each
    // chain's addresses together, by index.
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
                                         QStringLiteral("Import a seed on the Overview page."));
    ui_->waiting = page::makeEmptyState(this, int(icon::Kind::Refresh), QStringLiteral("Scanning addresses"),
                                        QStringLiteral("Addresses appear here as GitWallet derives and checks them."));
    ui_->noResults = page::makeNoResultsState(this);
    root->addWidget(ui_->noWallet, 1);
    root->addWidget(ui_->waiting, 1);
    root->addWidget(ui_->noResults, 1);

    const page::ActionBar bar = page::makeActionBar(this);
    ui_->search = page::makeSearch(bar.widget, QStringLiteral("Search address or path"));
    ui_->chain = new QComboBox(bar.widget);
    ui_->chain->addItems({QStringLiteral("All chains"), QStringLiteral("Bitcoin"), QStringLiteral("Ethereum")});
    ui_->chain->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ui_->state = new QComboBox(bar.widget);
    ui_->state->addItems({QStringLiteral("All states"), QStringLiteral("Funded"), QStringLiteral("Used")});
    ui_->state->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ui_->state->setToolTip(QStringLiteral("Funded: holds a balance now. Used: has transactions or a balance."));
    ui_->count = new QLabel(bar.widget);
    ui_->count->setObjectName(QStringLiteral("mimCount"));
    bar.left->addWidget(ui_->search);
    bar.left->addWidget(ui_->chain);
    bar.left->addWidget(ui_->state);
    bar.left->addWidget(ui_->count);

    ui_->refresh = new QPushButton(QStringLiteral("Refresh"), bar.widget);
    ui_->refresh->setToolTip(QStringLiteral("Fetch prices and Ethereum balances now (Bitcoin updates live)"));
    page::setLabeledIcon(ui_->refresh, refreshIcon());
    bar.right->addWidget(ui_->refresh);
    root->addWidget(bar.widget);

    ui_->update = new QTimer(this);
    ui_->update->setSingleShot(true);
    ui_->update->setInterval(100);
    connect(ui_->update, &QTimer::timeout, this, &AddressesPage::updateState);
    ui_->cooldown = new QTimer(this);
    ui_->cooldown->setSingleShot(true);
    ui_->cooldown->setInterval(kRefreshCooldownMs);
    connect(ui_->cooldown, &QTimer::timeout, this, &AddressesPage::updateState);

    connect(ui_->search, &QLineEdit::textChanged, this, [this](const QString& text) {
        ui_->filter->setQuery(text.trimmed());
        updateState();
    });
    connect(ui_->chain, &QComboBox::currentIndexChanged, this, [this](int index) {
        ui_->filter->setChain(index);
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
                       trText(QStringLiteral("Refreshing prices and Ethereum balances")));
        ui_->cooldown->start();
        ui_->refresh->setEnabled(false);
    });

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
    if (controller_)
        connect(controller_, &WalletController::walletChanged, this, &AddressesPage::updateState);
    updateState();
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
    fit(AddressTableModel::ColChain, {QStringLiteral("BTC"), QStringLiteral("ETH")}, 0);
    fit(AddressTableModel::ColType,
        {QStringLiteral("Native SegWit"), QStringLiteral("Nested SegWit"), QStringLiteral("Ethereum")}, 0);
    fit(AddressTableModel::ColPath,
        {QStringLiteral("m/84'/0'/0'/1/00000"), QStringLiteral("m/44'/60'/0'/0/00000")}, 0);
    // Addresses differ in glyph widths: the two public test vectors plus slack.
    fit(AddressTableModel::ColAddress,
        {QStringLiteral("bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu"),
         QStringLiteral("0x9858EfFD232B4033E47d90003D41EC34EcaEda94")},
        2 * cell.averageCharWidth());
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
    ui_->chain->setEnabled(hasWallet);
    ui_->state->setEnabled(hasWallet);
    ui_->refresh->setEnabled(hasWallet && !ui_->cooldown->isActive());

    const int shown = ui_->filter->rowCount();
    const int total = ui_->model->rowCount();
    int funded = 0;
    if (!ui_->filter->narrowed()) {
        // Every row shows: the model counts them, so a scan streaming in
        // thousands of rows costs no recount per burst.
        funded = fundedAmong(controller_ ? controller_->model() : nullptr, total);
    } else {
        for (int row = 0; row < shown; ++row) {
            const QVariant state = ui_->filter->index(row, 0).data(AddressTableModel::StateRole);
            if (state.toInt() == int(wallet::AddressState::Funded))
                ++funded;
        }
    }
    ui_->count->setText(hasWallet ? countText(shown, total, funded) : QString());
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
