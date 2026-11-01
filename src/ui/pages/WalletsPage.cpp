#include "ui/pages/WalletsPage.h"

#include "app/WalletController.h"
#include "chain/Address.h"
#include "net/PriceFeed.h"
#include "ui/DialogChrome.h"
#include "ui/Feedback.h"
#include "ui/Figures.h"
#include "ui/HoverRowTable.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/ThemeManager.h"
#include "ui/XpubField.h"
#include "ui/design/Style.h"
#include "wallet/BulkImport.h"
#include "wallet/Types.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QAbstractTableModel>
#include <QDialog>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QVBoxLayout>

#include <exception>
#include <utility>

namespace app::ui {

namespace {

using figures::atLeast;
using figures::dash;
using figures::formatAmount;
using figures::formatCount;
using figures::formatUsd;

// A scan signals per address; the table needs the state a few times a second.
constexpr int kRepaintMs = 300;

constexpr int kSortRole = Qt::UserRole + 1;
constexpr int kIdRole = Qt::UserRole + 2;

enum Column { ColName, ColSource, ColAddresses, ColFunded, ColBtc, ColEth, ColUsdt, ColUsdc, ColUsd, ColState, ColumnCount };

// The asset of a balance column; nullptr for any other column.
const wallet::AssetInfo* assetOf(int column)
{
    switch (column) {
    case ColBtc: return &wallet::assetInfo(wallet::Asset::BTC);
    case ColEth: return &wallet::assetInfo(wallet::Asset::ETH);
    case ColUsdt: return &wallet::assetInfo(wallet::Asset::USDT);
    case ColUsdc: return &wallet::assetInfo(wallet::Asset::USDC);
    default: return nullptr;
    }
}

bool numericColumn(int column)
{
    return column >= ColAddresses && column <= ColUsd;
}

// What the State column says of a wallet, the most pressing first.
enum class RowState {
    Limit,      // the scan was cut at the address limit: any wallet's figures may be low
    Scanning,   // an account has no rows yet, and a scan is on its way to them
    NotScanned, // an account has no rows, and nothing is on its way to them
    Checking,   // rows without an answer yet
    LastKnown,  // every row has answered once, but the balances are not current now
    UpToDate,
};

// The wallets of the store, read from the controller. Rows only come and go on
// walletChanged; between those the figures repaint. What takes more than a lookup
// is read once per repaint, so that a table of hundreds of wallets stays cheap.
class WalletTableModel : public QAbstractTableModel
{
public:
    explicit WalletTableModel(WalletController* controller, QObject* parent)
        : QAbstractTableModel(parent), controller_(controller)
    {
    }

    void reload()
    {
        beginResetModel();
        wallets_ = controller_ ? controller_->store().wallets : QList<wallet::WalletEntry>();
        takeSnapshot();
        endResetModel();
    }

    void repaintFigures()
    {
        if (wallets_.isEmpty())
            return;
        takeSnapshot();
        emit dataChanged(index(0, ColAddresses), index(int(wallets_.size()) - 1, ColumnCount - 1));
    }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(wallets_.size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : ColumnCount; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal)
            return QAbstractTableModel::headerData(section, orientation, role);
        if (role == Qt::TextAlignmentRole && numericColumn(section))
            return QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter));
        if (role != Qt::DisplayRole)
            return {};
        if (const wallet::AssetInfo* info = assetOf(section))
            return info->symbol;
        switch (section) {
        case ColName: return trText(QStringLiteral("Name"));
        case ColSource: return trText(QStringLiteral("Source"));
        case ColAddresses: return trText(QStringLiteral("Addresses"));
        case ColFunded: return trText(QStringLiteral("Funded"));
        case ColUsd: return QStringLiteral("USD");
        case ColState: return trText(QStringLiteral("State"));
        default: return {};
        }
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        const wallet::WalletModel* model = controller_ ? controller_->model() : nullptr;
        if (!model || !index.isValid() || index.row() >= wallets_.size())
            return {};
        const wallet::WalletEntry& entry = wallets_.at(index.row());
        const int column = index.column();
        const wallet::WalletCounts counts = model->counts(entry.id);
        const Shown shown = shown_.value(entry.id);
        const RowState state = stateOf(counts, shown);
        // A figure reads plain only once every balance behind it has answered and
        // the scan reached all it should: a zero with an address nobody looked at
        // behind it is no answer. Figures from an earlier moment (LastKnown) stay
        // plain, and the State column says so, as on the Overview.
        const bool complete = state == RowState::LastKnown || state == RowState::UpToDate;

        // Amounts format through chain::Amount (Botan): nothing may throw out of
        // a model call into Qt's painting or sorting.
        try {
            switch (role) {
            case kIdRole:
                return entry.id;
            case Qt::TextAlignmentRole:
                return numericColumn(column) ? QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter))
                                             : QVariant();
            case Qt::ForegroundRole:
                if (column != ColState)
                    return {};
                switch (state) {
                case RowState::Limit: return toneColor(Tone::Error);
                case RowState::NotScanned:
                case RowState::LastKnown: return toneColor(Tone::Warning);
                case RowState::UpToDate: return toneColor(Tone::Success);
                case RowState::Scanning:
                case RowState::Checking: break;
                }
                return toneColor(Tone::Muted);
            case kSortRole:
                if (const wallet::AssetInfo* info = assetOf(column))
                    return model->total(info->asset, entry.id).toDouble(info->decimals);
                switch (column) {
                case ColName: return entry.label;
                case ColSource: return entry.fromKeys ? 1 : 0;
                case ColAddresses: return counts.rows;
                case ColFunded: return counts.funded;
                case ColUsd: return controller_->totalUsd(entry.id);
                case ColState: return int(state);
                default: return {};
                }
            case Qt::ToolTipRole:
                if (column == ColName)
                    return entry.label;
                if (column == ColState && state != RowState::UpToDate)
                    return status_; // the whole of it, as the title bar says it
                if (const wallet::AssetInfo* info = assetOf(column); info && !watches(shown, *info))
                    return trText(QStringLiteral("This wallet has no key for this chain, so nothing was looked at."));
                return {};
            case Qt::DisplayRole:
                break;
            default:
                return {};
            }

            if (const wallet::AssetInfo* info = assetOf(column)) {
                if (!watches(shown, *info))
                    return dash(); // no key on this chain: not a zero
                const chain::Amount amount = model->total(info->asset, entry.id);
                if (complete)
                    return formatAmount(amount, *info);
                return amount.isZero() ? dash() : atLeast(formatAmount(amount, *info));
            }
            switch (column) {
            case ColName:
                return entry.label;
            case ColSource:
                return entry.fromKeys ? trText(QStringLiteral("Public keys")) : trText(QStringLiteral("Seed"));
            case ColAddresses:
                return formatCount(counts.rows);
            case ColFunded:
                return formatCount(counts.funded);
            case ColUsd: {
                // An asset with a balance but no price would make the sum too low.
                for (const wallet::AssetInfo& info : wallet::assets()) {
                    if (!model->total(info.asset, entry.id).isZero() && controller_->prices()->usd(info.symbol) <= 0.0)
                        return dash();
                }
                const double usd = controller_->totalUsd(entry.id);
                if (complete)
                    return formatUsd(usd);
                return usd > 0.0 ? atLeast(formatUsd(usd)) : dash();
            }
            case ColState:
                switch (state) {
                case RowState::Limit: return trText(QStringLiteral("Address limit reached"));
                case RowState::Scanning: return trText(QStringLiteral("Scanning"));
                case RowState::NotScanned: return trText(QStringLiteral("Not fully scanned"));
                case RowState::LastKnown: return trText(QStringLiteral("Last known"));
                case RowState::UpToDate: return trText(QStringLiteral("Up to date"));
                case RowState::Checking: break;
                }
                return trText(QStringLiteral("Checking %1 of %2"))
                    .arg(formatCount(counts.checked), formatCount(counts.rows));
            default:
                return {};
            }
        } catch (const std::exception&) {
            return {};
        }
    }

private:
    struct Shown {
        bool allRows = false;  // every account has rows
        bool bitcoin = false;  // the wallet has a key on that chain
        bool ethereum = false;
    };

    static bool watches(const Shown& shown, const wallet::AssetInfo& info)
    {
        return info.asset == wallet::Asset::BTC ? shown.bitcoin : shown.ethereum;
    }

    RowState stateOf(const wallet::WalletCounts& counts, const Shown& shown) const
    {
        if (limit_)
            return RowState::Limit;
        if (!shown.allRows || counts.rows == 0)
            return scanning_ ? RowState::Scanning : RowState::NotScanned;
        if (counts.checked < counts.rows)
            return scanning_ ? RowState::Checking : RowState::NotScanned;
        return current_ ? RowState::UpToDate : RowState::LastKnown;
    }

    void takeSnapshot()
    {
        shown_.clear();
        if (!controller_)
            return;
        limit_ = controller_->addressLimitReached();
        current_ = controller_->balancesComplete();
        scanning_ = controller_->isScanning();
        status_ = controller_->statusText();
        for (const wallet::WalletEntry& entry : std::as_const(wallets_)) {
            Shown shown;
            shown.allRows = controller_->walletHasAllRows(entry);
            for (const wallet::AccountKey& key : entry.accounts)
                (chain::isBitcoin(key.type) ? shown.bitcoin : shown.ethereum) = true;
            shown_.insert(entry.id, shown);
        }
    }

    WalletController* controller_ = nullptr;
    QList<wallet::WalletEntry> wallets_;
    QHash<quint32, Shown> shown_;
    bool limit_ = false;    // the scan is cut at the address limit
    bool current_ = false;  // every balance is whole and current
    bool scanning_ = false;
    QString status_;        // the title bar's line
};

} // namespace

struct WalletsPage::Ui : QObject
{
    using QObject::QObject;

    WalletTableModel* model = nullptr;
    QSortFilterProxyModel* filter = nullptr;
    HoverRowTableView* view = nullptr;
    QLineEdit* search = nullptr;
    QLabel* count = nullptr;
    QPushButton* keys = nullptr;
    QPushButton* rename = nullptr;
    QPushButton* remove = nullptr;
    QPushButton* addresses = nullptr;
    QPushButton* addMany = nullptr;
    QPushButton* add = nullptr;
    QWidget* empty = nullptr;
    QWidget* noResults = nullptr;
    QTimer* repaint = nullptr;
};

WalletsPage::WalletsPage(WalletController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller)
{
    ui_ = new Ui(this);
    auto* root = new QVBoxLayout(this);
    page::applyPageLayout(root);

    ui_->model = new WalletTableModel(controller_, this);
    ui_->filter = new QSortFilterProxyModel(this);
    ui_->filter->setSourceModel(ui_->model);
    ui_->filter->setSortRole(kSortRole);
    ui_->filter->setSortCaseSensitivity(Qt::CaseInsensitive);
    ui_->filter->setFilterKeyColumn(ColName);
    ui_->filter->setFilterCaseSensitivity(Qt::CaseInsensitive);

    auto* view = new HoverRowTableView(this);
    ui_->view = view;
    view->setModel(ui_->filter);
    page::configureTable(view);
    view->setMouseTracking(true);
    view->setWordWrap(false);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    QHeaderView* header = view->horizontalHeader();
    header->setSectionsClickable(true);
    header->setSortIndicatorClearable(true);
    header->setHighlightSections(false);
    header->setStretchLastSection(false);
    view->setSortingEnabled(true);
    header->setSortIndicator(-1, Qt::AscendingOrder); // the order they were added in
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ColName, QHeaderView::Stretch);
    header->setResizeContentsPrecision(0); // the rows on screen: hundreds of wallets stay cheap to measure
    root->addWidget(view, 1);

    ui_->empty = page::makeEmptyState(
        this, int(icon::Kind::ListRows), QStringLiteral("No wallet yet"),
        QStringLiteral("Add each seed you receive payments on, one by one or a whole list, and see what "
                       "every one of them holds."));
    ui_->noResults = page::makeNoResultsState(this);
    root->addWidget(ui_->empty, 1);
    root->addWidget(ui_->noResults, 1);

    const page::ActionBar bar = page::makeActionBar(this);
    ui_->search = page::makeSearch(bar.widget, QStringLiteral("Search wallet name"));
    ui_->count = page::makeBarCount(bar.widget);
    bar.left->addWidget(ui_->search);
    page::finishBarLeft(bar, ui_->search, ui_->count);

    ui_->keys = new QPushButton(QStringLiteral("Public keys"), bar.widget);
    ui_->rename = new QPushButton(QStringLiteral("Rename"), bar.widget);
    ui_->remove = new QPushButton(QStringLiteral("Remove"), bar.widget);
    ui_->addresses = new QPushButton(QStringLiteral("View addresses"), bar.widget);
    ui_->addMany = new QPushButton(QStringLiteral("Add many"), bar.widget);
    ui_->addMany->setToolTip(QStringLiteral("A list of seeds or public keys, pasted or from a file"));
    ui_->add = new QPushButton(QStringLiteral("Add wallet"), bar.widget);
    ui_->add->setObjectName(QStringLiteral("primaryButton"));
    bar.right->addWidget(ui_->keys);
    bar.right->addWidget(ui_->rename);
    bar.right->addWidget(ui_->remove);
    bar.right->addWidget(ui_->addresses);
    bar.right->addWidget(ui_->addMany);
    bar.right->addWidget(ui_->add);
    root->addWidget(bar.widget);

    ui_->repaint = new QTimer(this);
    ui_->repaint->setSingleShot(true);
    ui_->repaint->setInterval(kRepaintMs);
    connect(ui_->repaint, &QTimer::timeout, this, [this] {
        if (!isVisible())
            return; // showEvent() catches up
        ui_->model->repaintFigures();
        updateState();
    });

    connect(ui_->search, &QLineEdit::textChanged, this, [this](const QString& text) {
        ui_->filter->setFilterFixedString(text.trimmed());
        updateState();
    });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &WalletsPage::updateState);
    connect(view, &QAbstractItemView::doubleClicked, this, [this] {
        if (const quint32 id = selectedId())
            emit showAddressesRequested(id);
    });
    connect(ui_->add, &QPushButton::clicked, this, &WalletsPage::addRequested);
    connect(ui_->addMany, &QPushButton::clicked, this, &WalletsPage::addManyRequested);
    connect(ui_->addresses, &QPushButton::clicked, this, [this] {
        if (const quint32 id = selectedId())
            emit showAddressesRequested(id);
    });
    connect(ui_->rename, &QPushButton::clicked, this, &WalletsPage::renameSelected);
    connect(ui_->remove, &QPushButton::clicked, this, &WalletsPage::confirmRemoveSelected);
    connect(ui_->keys, &QPushButton::clicked, this, &WalletsPage::showKeysOfSelected);

    connect(LanguageManager::instance(), &LanguageManager::changed, this, [this] {
        emit ui_->model->headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);
        ui_->model->repaintFigures();
        updateState();
    });
    const auto queueRepaint = [this] {
        if (!ui_->repaint->isActive())
            ui_->repaint->start();
    };
    // The columns measure the rows on screen: rows scrolled into view are measured too.
    connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, queueRepaint);
    if (controller_) {
        connect(controller_, &WalletController::totalsChanged, this, queueRepaint);
        connect(controller_, &WalletController::statusChanged, this, queueRepaint);
        connect(controller_, &WalletController::walletChanged, this, [this] {
            const quint32 id = selectedId();
            ui_->model->reload();
            // A rename or another wallet leaves the row the user was on selected.
            for (int row = 0; id != 0 && row < ui_->filter->rowCount(); ++row) {
                if (ui_->filter->index(row, 0).data(kIdRole).toUInt() == id)
                    ui_->view->selectRow(row);
            }
            updateState();
        });
    }
    ui_->model->reload();
    updateState();
}

void WalletsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    ui_->model->repaintFigures();
    updateState();
}

quint32 WalletsPage::selectedId() const
{
    const QModelIndexList rows = ui_->view->selectionModel()->selectedRows();
    return rows.isEmpty() ? 0 : rows.first().data(kIdRole).toUInt();
}

void WalletsPage::updateState()
{
    const int total = ui_->model->rowCount();
    const int shown = ui_->filter->rowCount();
    const bool chosen = selectedId() != 0;
    ui_->search->setEnabled(total > 0);
    ui_->keys->setEnabled(chosen);
    ui_->rename->setEnabled(chosen);
    ui_->remove->setEnabled(chosen);
    ui_->addresses->setEnabled(chosen);
    ui_->count->setVisible(total > 0);
    ui_->count->setText(shown == total
                            ? trText(total == 1 ? QStringLiteral("%1 wallet") : QStringLiteral("%1 wallets"))
                                  .arg(formatCount(total))
                            : trText(QStringLiteral("%1 of %2 wallets"))
                                  .arg(formatCount(shown), formatCount(total)));
    page::showListState(ui_->view, ui_->empty, ui_->noResults, shown,
                        total > 0 ? ui_->search->text().trimmed() : QString());
}

void WalletsPage::renameSelected()
{
    const wallet::WalletEntry* entry = controller_ ? controller_->store().find(selectedId()) : nullptr;
    if (!entry)
        return;
    const quint32 id = entry->id;

    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("renameWalletDialog"));
    dialog->setWindowTitle(QStringLiteral("Rename wallet"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    applyDialogChrome(dialog);
    auto* column = new QVBoxLayout(dialog);
    page::applyDialogLayout(column);
    auto* name = new QLineEdit(entry->label, dialog);
    name->setMaxLength(wallet::WalletStore::kMaxLabelChars);
    name->selectAll();
    column->addWidget(name);
    auto* status = new QLabel(dialog);
    status->setWordWrap(true);
    setStatus(status, QString(), Tone::Muted);
    column->addWidget(status);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(tokens::layout::kToolbarSpacing);
    buttons->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), dialog);
    auto* save = new QPushButton(QStringLiteral("Rename"), dialog);
    save->setObjectName(QStringLiteral("primaryButton"));
    save->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(save);
    column->addLayout(buttons);
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, dialog, [this, dialog, name, status, id] {
        QString error;
        const bool renamed = controller_->renameWallet(id, name->text(), &error);
        if (renamed) {
            dialog->accept();
            return;
        }
        setStatus(status, error, Tone::Error);
        // Not kept on screen either, if that is why it was refused.
        if (wallet::looksLikeSecret(name->text(), wallet::WalletStore::kMaxLabelChars)) {
            name->setText(QString(name->text().size(), QLatin1Char(' ')));
            name->clear();
        }
    });
    page::fitDialog(dialog, page::DialogSize::Compact);
    // open(), not exec(): no nested event loop while the scanners keep running.
    dialog->open();
    name->setFocus();
}

void WalletsPage::confirmRemoveSelected()
{
    const wallet::WalletEntry* entry = controller_ ? controller_->store().find(selectedId()) : nullptr;
    if (!entry)
        return;
    const quint32 id = entry->id;
    auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Remove wallet"),
                                QStringLiteral("Remove “%1” from this PC?").arg(entry->label),
                                QMessageBox::NoButton, this);
    box->setTextFormat(Qt::PlainText); // a name is never markup
    box->setInformativeText(
        entry->fromKeys
            ? QStringLiteral("GitWallet forgets its public keys and stops watching its addresses. The other "
                             "wallets stay as they are. Nothing changes on the blockchain, but this PC has "
                             "no seed for it: to add it back you need these keys, which Public keys shows now.")
            : QStringLiteral("GitWallet forgets its public keys and stops watching its addresses. The other "
                             "wallets stay as they are. Nothing changes on the blockchain: adding its seed "
                             "again shows the same addresses."));
    QPushButton* remove = box->addButton(QStringLiteral("Remove wallet"), QMessageBox::DestructiveRole);
    remove->setObjectName(QStringLiteral("dangerButton"));
    QPushButton* cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    box->setAttribute(Qt::WA_DeleteOnClose);
    applyDialogChrome(box);
    connect(box, &QDialog::finished, this, [this, box, remove, id] {
        if (box->clickedButton() != remove)
            return;
        QString error;
        if (controller_->removeWallet(id, &error))
            Feedback::show(this, Feedback::Type::Success, trText(QStringLiteral("Wallet removed from this PC")));
        else
            Feedback::show(this, Feedback::Type::Error,
                           error.isEmpty() ? trText(QStringLiteral("Could not remove the wallet")) : error);
    });
    box->open();
}

void WalletsPage::showKeysOfSelected()
{
    const wallet::WalletEntry* entry = controller_ ? controller_->store().find(selectedId()) : nullptr;
    if (!entry)
        return;
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("walletKeysDialog"));
    dialog->setWindowTitle(entry->label);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    applyDialogChrome(dialog);
    auto* column = new QVBoxLayout(dialog);
    page::applyDialogLayout(column);
    auto* note = new QLabel(QStringLiteral("These public keys reveal every address and balance of this wallet "
                                           "but cannot spend."),
                            dialog);
    note->setWordWrap(true);
    setTone(note, Tone::Muted);
    column->addWidget(note);
    auto* form = new QFormLayout;
    page::applyFormLayout(form);
    for (const wallet::AccountKey& key : entry->accounts)
        form->addRow(accountLabel(key), makeXpubField(dialog, key));
    column->addLayout(form);
    column->addStretch(1);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* close = new QPushButton(QStringLiteral("Close"), dialog);
    close->setDefault(true);
    buttons->addWidget(close);
    column->addLayout(buttons);
    connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
    page::fitDialog(dialog, page::DialogSize::Form);
    dialog->open();
}

} // namespace app::ui
