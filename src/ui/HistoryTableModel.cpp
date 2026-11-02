#include "ui/HistoryTableModel.h"

#include "app/WalletController.h"
#include "net/PriceFeed.h"
#include "ui/Figures.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/TableUtil.h"
#include "ui/design/Style.h"
#include "wallet/UtxoHistory.h"
#include "wallet/WalletModel.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QLocale>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <exception>
#include <numeric>
#include <type_traits>
#include <vector>

namespace app::ui {

namespace {

// How often the list is put together again while a history streams in: at least this far apart,
// and further at a size where it takes longer.
constexpr int kMinRebuildMs = 400;
constexpr int kRebuildFactor = 10;
// As many blocks as a merchant usually waits for: past it a block says "Confirmed".
constexpr int kEnoughConfirmations = 6;

const wallet::AssetInfo& coinOf(const wallet::HistoryBook& book)
{
    return wallet::assetInfo(wallet::networkInfo(book.network()).coin);
}

QVariant rightAligned()
{
    return QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter));
}

QString kindText(TxKind kind)
{
    switch (kind) {
    case TxKind::Received:
        return trText(QStringLiteral("Received"));
    case TxKind::Sent:
        return trText(QStringLiteral("Sent"));
    case TxKind::Moved:
        return trText(QStringLiteral("Moved"));
    }
    return {};
}

QString signedCoin(qint64 units, const wallet::AssetInfo& coin)
{
    const quint64 magnitude = units < 0 ? 0 - quint64(units) : quint64(units);
    const QString digits = figures::formatAmount(chain::Amount::fromU64(magnitude), coin);
    if (units == 0)
        return digits + QLatin1Char(' ') + coin.symbol;
    return QLatin1Char(units < 0 ? '-' : '+') + digits + QLatin1Char(' ') + coin.symbol;
}

QString exactCoin(qint64 units, const wallet::AssetInfo& coin)
{
    const quint64 magnitude = units < 0 ? 0 - quint64(units) : quint64(units);
    const QString digits = QString::fromStdString(chain::Amount::fromU64(magnitude).format(coin.decimals, coin.decimals));
    return units < 0 ? QLatin1Char('-') + digits : digits;
}

QString shortTx(const QString& hex)
{
    return hex.left(8) + QStringLiteral("…") + hex.right(8);
}

QString dateText(qint64 seconds)
{
    if (seconds <= 0)
        return figures::dash();
    static const QLocale english(QLocale::English, QLocale::UnitedStates);
    return english.toString(QDateTime::fromSecsSinceEpoch(seconds).toLocalTime(), QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace

HistoryTableModel::HistoryTableModel(WalletController* controller, QObject* parent)
    : QAbstractTableModel(parent), controller_(controller)
{
    rebuild_ = new QTimer(this);
    rebuild_->setSingleShot(true);
    connect(rebuild_, &QTimer::timeout, this, &HistoryTableModel::rebuildNow);
    if (controller_) {
        refreshLabels();
        const QList<wallet::UtxoHistory*> histories = controller_->histories();
        for (const wallet::UtxoHistory* history : histories) {
            connect(history->book(), &wallet::HistoryBook::changed, this, &HistoryTableModel::scheduleRebuild);
            connect(history->book(), &wallet::HistoryBook::positionsAboutToMove, this, &HistoryTableModel::positionsAboutToMove);
            connect(history->book(), &wallet::HistoryBook::positionsMoved, this, &HistoryTableModel::rebuildNow);
            connect(history->book(), &wallet::HistoryBook::notesChanged, this, &HistoryTableModel::scheduleRebuild);
        }
        connect(controller_, &WalletController::walletChanged, this, [this] {
            refreshLabels();
            scheduleRebuild();
        });
        connect(controller_, &WalletController::historyProgress, this, &HistoryTableModel::noteProgress);
        // A search goes through addresses' labels too.
        connect(controller_, &WalletController::labelsChanged, this, [this] {
            if (!filter_.query.isEmpty())
                scheduleRebuild();
        });
        connect(controller_->prices(), &net::PriceFeed::updated, this, [this] {
            if (!lines_.isEmpty())
                emit dataChanged(index(0, ColUsdNow), index(int(lines_.size()) - 1, ColUsdNow));
        });
    }
    connect(LanguageManager::instance(), &LanguageManager::changed, this, [this] {
        emit headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);
        if (!lines_.isEmpty())
            emit dataChanged(index(0, 0), index(int(lines_.size()) - 1, ColumnCount - 1));
    });
}

void HistoryTableModel::noteProgress()
{
    // Confirmations move with the tip, which changes no transaction, and a proof that did not hold
    // says so on its line.
    int tips = 0;
    int failed = 0;
    const QList<wallet::UtxoHistory*> histories = controller_->histories();
    for (const wallet::UtxoHistory* history : histories) {
        tips += history->tip();
        failed += history->progress().unproven;
    }
    if (tips != tipsSeen_ || failed != failedSeen_) {
        tipsSeen_ = tips;
        failedSeen_ = failed;
        scheduleRebuild();
    }
}

void HistoryTableModel::refreshLabels()
{
    labels_.clear();
    for (const wallet::WalletEntry& w : controller_->store().wallets)
        labels_.insert(w.id, w.label);
}

void HistoryTableModel::setFilter(const Filter& filter)
{
    filter_ = filter;
    rebuildNow();
}

int HistoryTableModel::totalCount() const
{
    int total = 0;
    if (controller_) {
        const QList<wallet::UtxoHistory*> histories = controller_->histories();
        for (const wallet::UtxoHistory* history : histories)
            total += history->book()->lineCount();
    }
    return total;
}

const HistoryTableModel::Line* HistoryTableModel::lineAt(int row) const
{
    if (row < 0 || row >= lines_.size())
        return nullptr;
    const Line& line = lines_.at(row);
    return line.position < line.book->size() ? &line : nullptr;
}

int HistoryTableModel::rowOf(wallet::HistoryBook* book, const chain::Hash256& txid) const
{
    const int position = book->find(txid);
    for (int row = 0; position >= 0 && row < lines_.size(); ++row) {
        if (lines_.at(row).book == book && lines_.at(row).position == position)
            return row;
    }
    return -1;
}

void HistoryTableModel::scheduleRebuild()
{
    // Nobody is looking: it costs a tenth of the window's time for nothing, on every other page too.
    if (!watched_) {
        stale_ = true;
        return;
    }
    if (!rebuild_->isActive())
        rebuild_->start(qMax(kMinRebuildMs, lastRebuildMs_ * kRebuildFactor));
}

void HistoryTableModel::setWatched(bool watched)
{
    watched_ = watched;
    if (watched_ && stale_)
        rebuildNow();
}

int HistoryTableModel::tipOf(const wallet::HistoryBook& book) const
{
    const wallet::UtxoHistory* history = controller_ ? controller_->historyOf(book.network()) : nullptr;
    return history ? history->tip() : -1;
}

TxState HistoryTableModel::stateOf(const wallet::TxRecord& tx, int tip, const wallet::UtxoHistory* history) const
{
    switch (tx.seen) {
    case wallet::TxSeen::Replaced:
        return TxState::Replaced;
    case wallet::TxSeen::Dropped:
        return TxState::Dropped;
    case wallet::TxSeen::Alive:
        break;
    }
    if (tx.height <= 0)
        return TxState::Pending;
    if (!tx.proven)
        return history && history->proofFailed(tx.txid) ? TxState::ProofFailed : TxState::Unproven;
    return tip - tx.height + 1 >= kEnoughConfirmations ? TxState::Confirmed : TxState::Confirming;
}

bool HistoryTableModel::makeLine(wallet::HistoryBook* book, const wallet::UtxoHistory* history, int position, int scope, int tip, const Filter& filter, Line* out) const
{
    const QVector<wallet::TxEffect>& effects = book->effectsOf(position);
    Line line;
    line.book = book;
    line.position = position;
    bool inside = false;
    for (const wallet::TxEffect& effect : effects) {
        if (scope >= 0 && effect.address != scope)
            continue;
        if (filter.wallet != 0 && book->addressAt(effect.address).wallet != filter.wallet)
            continue;
        line.received += effect.received;
        line.spent += effect.spent;
        inside = true;
    }
    if (!inside)
        return false;
    const wallet::TxRecord& tx = book->at(position);
    line.time = tx.time;
    line.state = stateOf(tx, tip, history);
    if (line.spent == 0)
        line.kind = TxKind::Received;
    else if (line.received == 0)
        line.kind = TxKind::Sent;
    else if (line.received >= tx.outTotal && line.spent >= line.received)
        line.kind = TxKind::Moved; // every output came back here, and it was here it came from
    else
        line.kind = line.received > line.spent ? TxKind::Received : TxKind::Sent;
    *out = line;
    return true;
}

bool HistoryTableModel::lineOf(wallet::HistoryBook* book, const chain::Hash256& txid, Line* out) const
{
    const int position = book->find(txid);
    if (position < 0)
        return false;
    const int scope = filter_.scripthash.isEmpty() ? -1 : book->findAddress(filter_.scripthash);
    if (!filter_.scripthash.isEmpty() && scope < 0)
        return false;
    return makeLine(book, controller_ ? controller_->historyOf(book->network()) : nullptr, position, scope, tipOf(*book), filter_, out);
}

qint64 HistoryTableModel::feeShown(const Line& line) const
{
    const wallet::TxRecord& tx = line.book->at(line.position);
    if (tx.fee < 0 || line.spent <= 0)
        return -1;
    // All of what the wallets spent, or it is not this line's to carry: lines of two wallets would both.
    qint64 spent = 0;
    for (const wallet::TxEffect& effect : line.book->effectsOf(line.position))
        spent += effect.spent;
    return line.spent == spent ? tx.fee : -1;
}

bool HistoryTableModel::matches(const Line& line, const wallet::HistoryBook& book, const wallet::TxRecord& tx,
                                const QVector<wallet::TxEffect>& effects) const
{
    if (filter_.kind >= 0 && int(line.kind) != filter_.kind)
        return false;
    if (filter_.state >= 0) {
        const bool pending = line.state == TxState::Pending;
        const bool gone = line.state == TxState::Replaced || line.state == TxState::Dropped;
        const bool failed = line.state == TxState::ProofFailed;
        if (filter_.state == kStatePending ? !pending
            : filter_.state == kStateGone ? !gone
            : filter_.state == kStateProofFailed ? !failed
                                                 : (pending || gone))
            return false;
    }
    if (filter_.from > 0 && line.time < filter_.from)
        return false;
    if (filter_.until > 0 && line.time >= filter_.until)
        return false;
    if (filter_.query.isEmpty())
        return true;
    const auto contains = [this](const QString& text) { return text.contains(filter_.query, Qt::CaseInsensitive); };
    if (contains(QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(tx.txid.data()), qsizetype(tx.txid.size())).toHex())))
        return true;
    for (const wallet::TxEffect& effect : effects) {
        const wallet::HistoryBook::AddressEntry& entry = book.addressAt(effect.address);
        if (contains(entry.address))
            return true;
        if (controller_ && contains(controller_->addressLabel(entry.address)))
            return true;
    }
    const wallet::TxNote note = book.noteOf(tx.txid);
    return contains(note.label) || contains(note.note);
}

void HistoryTableModel::rebuildNow()
{
    rebuild_->stop();
    QElapsedTimer clock;
    clock.start();
    QVector<Line> lines;
    qint64 receivedShown = 0;
    qint64 spentShown = 0;
    if (controller_) {
        const QList<wallet::UtxoHistory*> histories = controller_->histories();
        for (const wallet::UtxoHistory* history : histories) {
            wallet::HistoryBook* book = history->book();
            const int scope = filter_.scripthash.isEmpty() ? -1 : book->findAddress(filter_.scripthash);
            if (!filter_.scripthash.isEmpty() && scope < 0)
                continue;
            lines.reserve(lines.size() + book->size());
            const int tip = tipOf(*book);
            for (int position = 0; position < book->size(); ++position) {
                Line line;
                if (!makeLine(book, history, position, scope, tip, filter_, &line))
                    continue;
                if (!matches(line, *book, book->at(position), book->effectsOf(position)))
                    continue;
                // What it did, as the line says: change and moves between what is shown are nothing.
                if (line.state == TxState::Confirming || line.state == TxState::Confirmed) {
                    const qint64 net = line.received - line.spent;
                    if (net > 0)
                        receivedShown += net;
                    else
                        spentShown -= net;
                }
                lines.append(line);
            }
        }
    }
    stale_ = false;
    totalAtRebuild_ = totalCount();
    emit aboutToRebuild();
    beginResetModel();
    lines_ = std::move(lines);
    receivedShown_ = receivedShown;
    spentShown_ = spentShown;
    sortLines();
    endResetModel();
    lastRebuildMs_ = int(clock.elapsed());
    emit rebuilt();
}

double HistoryTableModel::priceNow(const wallet::HistoryBook& book) const
{
    return controller_ ? controller_->prices()->usd(coinOf(book).asset) : 0.0;
}

double HistoryTableModel::coinValue(const Line& line) const
{
    const qint64 net = netOf(line);
    return double(net < 0 ? -net : net) / 100000000.0;
}

void HistoryTableModel::sortLines()
{
    const bool ascending = sortOrder_ == Qt::AscendingOrder;
    // By date, as the column reads. What is pending has no block yet: it stands above every block, as
    // the newest. In one block, in the block's order: what spends an output comes after what made it.
    // One block's time can be before the last one's, and then the dates say so.
    const auto byTime = [](const Line& a, const Line& b) {
        const bool first = a.state == TxState::Pending;
        const bool second = b.state == TxState::Pending;
        if (first != second)
            return second;
        if (a.time != b.time)
            return a.time < b.time;
        const wallet::TxRecord& x = a.book->at(a.position);
        const wallet::TxRecord& y = b.book->at(b.position);
        if (x.height != y.height)
            return x.height < y.height;
        if (x.blockPos != y.blockPos)
            return x.blockPos < y.blockPos;
        return a.position < b.position;
    };
    // Each key once, on a list that half a million comparisons a level go over.
    const auto order = [&](const auto& keyOf) {
        using Key = std::decay_t<decltype(keyOf(lines_.first()))>;
        std::vector<Key> keys;
        keys.reserve(size_t(lines_.size()));
        for (const Line& line : std::as_const(lines_))
            keys.push_back(keyOf(line));
        std::vector<int> rows(keys.size());
        std::iota(rows.begin(), rows.end(), 0);
        const QVector<Line> before = lines_;
        std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
            const Key& x = keys[size_t(a)];
            const Key& y = keys[size_t(b)];
            if (x == y)
                return byTime(before.at(b), before.at(a)); // the newest first, whichever way the column goes
            return ascending ? x < y : y < x;
        });
        for (size_t i = 0; i < rows.size(); ++i)
            lines_[qsizetype(i)] = before.at(rows[i]);
    };
    // Text sorts as the numbers of what there is of it, of which there are few.
    const auto orderText = [&](const auto& textOf) {
        QHash<QString, int> numbers;
        for (const Line& line : std::as_const(lines_))
            numbers.insert(textOf(line), 0);
        // As the other pages do it, whatever case a name is in.
        QStringList distinct = numbers.keys();
        std::sort(distinct.begin(), distinct.end(), [](const QString& a, const QString& b) {
            const int order = QString::compare(a, b, Qt::CaseInsensitive);
            return order != 0 ? order < 0 : a < b;
        });
        for (int i = 0; i < distinct.size(); ++i)
            numbers.insert(distinct.at(i), i);
        order([&](const Line& line) { return numbers.value(textOf(line)); });
    };
    switch (sortColumn_) {
    case ColType:
        order([](const Line& l) { return int(l.kind); });
        break;
    case ColWallet:
        orderText([this](const Line& l) { return walletText(l); });
        break;
    case ColAmount:
        order([this](const Line& l) { return netOf(l); });
        break;
    case ColUsdNow:
        order([this](const Line& l) { return coinValue(l) * priceNow(*l.book); });
        break;
    case ColUsdThen:
        order([this](const Line& l) { return coinValue(l) * l.book->at(l.position).usdThen; });
        break;
    case ColFee:
        order([this](const Line& l) { return feeShown(l); });
        break;
    case ColState:
        order([](const Line& l) { return int(l.state); });
        break;
    case ColLabel:
        orderText([](const Line& l) { return l.book->noteOf(l.book->at(l.position).txid).label; });
        break;
    case ColTx:
        order([](const Line& l) { return l.book->at(l.position).txid; });
        break;
    case ColDate:
    default:
        std::stable_sort(lines_.begin(), lines_.end(), [&](const Line& a, const Line& b) {
            return ascending ? byTime(a, b) : byTime(b, a);
        });
        break;
    }
}

void HistoryTableModel::sort(int column, Qt::SortOrder order)
{
    if (column < 0)
        column = ColDate;
    sortColumn_ = column;
    sortOrder_ = order;
    // A row chosen stays that transaction, not that number.
    emit aboutToRebuild();
    emit layoutAboutToBeChanged();
    sortLines();
    emit layoutChanged();
    emit rebuilt();
}

int HistoryTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(lines_.size());
}

int HistoryTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QStringList HistoryTableModel::walletNames(const Line& line, const Filter& filter, int atMost) const
{
    const QVector<wallet::TxEffect>& effects = line.book->effectsOf(line.position);
    QStringList names;
    for (const wallet::TxEffect& effect : effects) {
        const wallet::HistoryBook::AddressEntry& entry = line.book->addressAt(effect.address);
        if (!filter.scripthash.isEmpty() && entry.scripthash != filter.scripthash)
            continue;
        const QString name = labels_.value(entry.wallet);
        if ((filter.wallet == 0 || filter.wallet == entry.wallet) && !name.isEmpty() && !names.contains(name))
            names.append(name);
        if (atMost > 0 && names.size() >= atMost)
            break;
    }
    return names;
}

QString HistoryTableModel::walletText(const Line& line) const
{
    const QVector<wallet::TxEffect>& effects = line.book->effectsOf(line.position);
    // Nearly every line: nothing to put together.
    if (effects.size() == 1) {
        const quint32 id = line.book->addressAt(effects.first().address).wallet;
        return filter_.wallet == 0 || filter_.wallet == id ? labels_.value(id) : QString();
    }
    return walletNames(line, filter_, 3).join(QStringLiteral(" · "));
}

QList<QString> HistoryTableModel::walletsOf(const Line& line) const
{
    const QVector<wallet::TxEffect>& effects = line.book->effectsOf(line.position);
    QList<QString> names;
    for (const wallet::TxEffect& effect : effects) {
        const QString name = labels_.value(line.book->addressAt(effect.address).wallet);
        if (!name.isEmpty() && !names.contains(name))
            names.append(name);
    }
    return names;
}

QStringList HistoryTableModel::addressesOf(const Line& line, const Filter& filter) const
{
    const QVector<wallet::TxEffect>& effects = line.book->effectsOf(line.position);
    QStringList addresses;
    for (const wallet::TxEffect& effect : effects) {
        const wallet::HistoryBook::AddressEntry& entry = line.book->addressAt(effect.address);
        if (filter.wallet != 0 && entry.wallet != filter.wallet)
            continue;
        if (!filter.scripthash.isEmpty() && entry.scripthash != filter.scripthash)
            continue;
        if (!entry.address.isEmpty())
            addresses.append(entry.address);
    }
    return addresses;
}

QVariant HistoryTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= lines_.size() || index.column() >= ColumnCount)
        return {};
    const Line& line = lines_.at(index.row());
    if (line.position >= line.book->size())
        return {};
    const wallet::TxRecord& tx = line.book->at(line.position);
    const wallet::AssetInfo& coin = coinOf(*line.book);
    const int column = index.column();
    const qint64 net = netOf(line);
    const bool stands = tx.seen == wallet::TxSeen::Alive;

    try {
        switch (role) {
        case Qt::DisplayRole:
            switch (column) {
            case ColDate:
                return dateText(tx.time);
            case ColType:
                return kindText(line.kind);
            case ColWallet:
                return walletText(line);
            case ColAmount:
                return signedCoin(net, coin);
            case ColUsdNow: {
                const double price = priceNow(*line.book);
                return price > 0.0 ? figures::formatUsd(coinValue(line) * price) : figures::dash();
            }
            case ColUsdThen:
                return tx.usdThen > 0.0 ? figures::formatUsd(coinValue(line) * tx.usdThen) : figures::dash();
            case ColFee: {
                // What the wallets paid: of a payment from someone else it is theirs.
                const qint64 fee = feeShown(line);
                return fee >= 0 ? figures::formatAmount(chain::Amount::fromU64(quint64(fee)), coin) + QLatin1Char(' ') + coin.symbol : figures::dash();
            }
            case ColState:
                return stateText(line);
            case ColLabel:
                return line.book->noteOf(tx.txid).label;
            case ColTx:
                return shortTx(QString::fromStdString(chain::hashHex(tx.txid)));
            }
            break;
        case TxidRole:
            return QString::fromStdString(chain::hashHex(tx.txid));
        case KindRole:
            return int(line.kind);
        case StateRole:
            return int(line.state);
        case kCopyRole:
            return column == ColTx ? QVariant(QString::fromStdString(chain::hashHex(tx.txid))) : QVariant();
        case Qt::ToolTipRole:
            switch (column) {
            case ColAmount:
                return trText(QStringLiteral("What this did to the balance shown: %1")).arg(signedCoin(net, coin));
            case ColUsdNow: {
                return controller_ ? controller_->pricesLine() : QString();
            }
            case ColUsdThen:
                if (tx.usdThen > 0.0)
                    return trText(QStringLiteral("1 %1 = %2 at %3")).arg(coin.symbol, figures::formatUsd(tx.usdThen), dateText(tx.usdThenAt));
                return trText(QStringLiteral("The price at that moment is not known yet"));
            case ColState:
                switch (line.state) {
                case TxState::Pending:
                    return trText(QStringLiteral("Not in a block yet. It can still be replaced: do not count it as paid."));
                case TxState::Confirming:
                case TxState::Confirmed:
                    return trText(QStringLiteral("In block %1. Its Merkle proof and the block's work were checked on this PC.")).arg(tx.height);
                case TxState::Unproven:
                    return trText(QStringLiteral("A server says it is in block %1. Its proof has not been checked yet.")).arg(tx.height);
                case TxState::ProofFailed:
                    return trText(QStringLiteral("A server says it is in block %1, and the proof it gave does not hold, or it gives none. Do not count it as paid.")).arg(tx.height);
                case TxState::Replaced:
                    return trText(QStringLiteral("Another transaction spends the same coins. This one no longer counts."));
                case TxState::Dropped:
                    return trText(QStringLiteral("Servers no longer list it. Unless it comes back, it does not count."));
                }
                break;
            case ColLabel: {
                // Plain text: Qt reads a tooltip as markup only by what its first line looks like, and
                // that line is GitWallet's own.
                const QString note = line.book->noteOf(tx.txid).note;
                return note.isEmpty() ? QString() : trText(QStringLiteral("Note")) + QLatin1Char('\n') + note;
            }
            case ColTx:
                return QString::fromStdString(chain::hashHex(tx.txid));
            case ColWallet:
                return trText(QStringLiteral("Wallets")) + QLatin1Char('\n') + QStringList(walletsOf(line)).join(QLatin1Char('\n'));
            default:
                break;
            }
            break;
        case Qt::TextAlignmentRole:
            if (column == ColAmount || column == ColUsdNow || column == ColUsdThen || column == ColFee)
                return rightAligned();
            break;
        case Qt::ForegroundRole:
            if (!stands)
                return toneColor(Tone::Muted);
            if (column == ColAmount && net > 0 && line.kind == TxKind::Received)
                return toneColor(Tone::Success);
            if (column == ColState) {
                if (line.state == TxState::Pending || line.state == TxState::Unproven || line.state == TxState::ProofFailed)
                    return toneColor(Tone::Warning);
                if (line.state == TxState::Confirmed)
                    return toneColor(Tone::Success);
            }
            break;
        default:
            break;
        }
    } catch (const std::exception&) {
        return {};
    }
    return {};
}

QVariant HistoryTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return QAbstractTableModel::headerData(section, orientation, role);
    if (role == Qt::TextAlignmentRole)
        return (section == ColAmount || section == ColUsdNow || section == ColUsdThen || section == ColFee) ? rightAligned() : QVariant();
    if (role == Qt::ToolTipRole) {
        if (section == ColUsdNow && controller_)
            return controller_->pricesLine();
        if (section == ColUsdThen)
            return trText(QStringLiteral("What it was worth when it happened, from DefiLlama: to the minute in recent years, to the day further back, nothing before 2015."));
        return {};
    }
    if (role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColDate:
        return trText(QStringLiteral("Date"));
    case ColType:
        return trText(QStringLiteral("Type"));
    case ColWallet:
        return trText(QStringLiteral("Wallet"));
    case ColAmount:
        return trText(QStringLiteral("Amount"));
    case ColUsdNow:
        return trText(QStringLiteral("USD now"));
    case ColUsdThen:
        return trText(QStringLiteral("USD then"));
    case ColFee:
        return trText(QStringLiteral("Fee"));
    case ColState:
        return trText(QStringLiteral("State"));
    case ColLabel:
        return trText(QStringLiteral("Label"));
    case ColTx:
        return trText(QStringLiteral("Transaction"));
    default:
        return {};
    }
}

QString HistoryTableModel::stateText(const Line& line) const
{
    const wallet::TxRecord& tx = line.book->at(line.position);
    switch (line.state) {
    case TxState::Pending:
        return trText(QStringLiteral("Pending"));
    case TxState::Confirming: {
        const int blocks = qMax(1, tipOf(*line.book) - tx.height + 1);
        return blocks == 1 ? trText(QStringLiteral("1 confirmation")) : trText(QStringLiteral("%1 confirmations")).arg(blocks);
    }
    case TxState::Confirmed:
        return trText(QStringLiteral("Confirmed"));
    case TxState::Unproven:
        return trText(QStringLiteral("In a block, proof on its way"));
    case TxState::ProofFailed:
        return trText(QStringLiteral("In a block, proof did not hold"));
    case TxState::Replaced:
        return trText(QStringLiteral("Replaced"));
    case TxState::Dropped:
        return trText(QStringLiteral("Dropped"));
    }
    return {};
}

HistoryTableModel::ExportLine HistoryTableModel::exportLine(const Line& line, const Filter& filter) const
{
    const wallet::TxRecord& tx = line.book->at(line.position);
    const wallet::AssetInfo& coin = coinOf(*line.book);
    const wallet::TxNote note = line.book->noteOf(tx.txid);
    const double price = priceNow(*line.book);
    ExportLine e;
    e.date = tx.time > 0 ? QDateTime::fromSecsSinceEpoch(tx.time).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString();
    e.kind = kindText(line.kind);
    e.wallet = walletNames(line, filter, 0).join(QStringLiteral(" · "));
    e.amount = exactCoin(netOf(line), coin);
    e.coin = coin.symbol;
    // Signed as the amount is, so that a column for the books can be added up.
    const double signedCoins = double(netOf(line)) / 100000000.0;
    e.usdNow = price > 0.0 ? QString::number(signedCoins * price, 'f', 2) : QString();
    e.usdThen = tx.usdThen > 0.0 ? QString::number(signedCoins * tx.usdThen, 'f', 2) : QString();
    e.priceThenAt = tx.usdThenAt > 0 ? QDateTime::fromSecsSinceEpoch(tx.usdThenAt).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
    e.fee = feeShown(line) >= 0 ? exactCoin(feeShown(line), coin) : QString();
    e.state = stateText(line);
    e.height = tx.height > 0 ? QString::number(tx.height) : QString();
    e.label = note.label;
    e.note = note.note;
    e.txid = QString::fromStdString(chain::hashHex(tx.txid));
    e.addresses = addressesOf(line, filter).join(QLatin1Char(' '));
    return e;
}

} // namespace app::ui
