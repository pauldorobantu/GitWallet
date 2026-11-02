#include "wallet/Payments.h"

#include "wallet/History.h"
#include "wallet/WalletModel.h"

#include <QDateTime>
#include <QTimer>

#include <algorithm>
#include <exception>
#include <utility>

namespace wallet {

namespace {

// Many at once are told of as one.
int gPauseMs = 1000;
// A source behind the others says less for about this long at most.
constexpr qint64 kFallMs = 15 * 60 * 1000;
// A transaction that took the place of another came soon after it.
constexpr qsizetype kLookBack = 64;

bool spendTheSame(const TxRecord& a, const TxRecord& b)
{
    for (const chain::OutPoint& point : a.inputs) {
        if (std::find(b.inputs.cbegin(), b.inputs.cend(), point) != b.inputs.cend())
            return true;
    }
    return false;
}

} // namespace

void PaymentBook::setPauseForTest(int ms)
{
    gPauseMs = ms;
}

PaymentBook::PaymentBook(WalletModel* model, const QList<HistoryBook*>& books, QObject* parent)
    : QObject(parent), model_(model), books_(books)
{
    pause_ = new QTimer(this);
    pause_->setSingleShot(true);
    connect(pause_, &QTimer::timeout, this, &PaymentBook::tellNow);
    connect(model_, &WalletModel::rowMoved, this, &PaymentBook::onRowMoved);
    for (HistoryBook* book : std::as_const(books_)) {
        connect(book, &HistoryBook::added, this, [this, book](int position) { onAdded(book, position); });
        connect(book, &HistoryBook::cleared, this, [this, book] { expecting_.remove(book->network()); });
    }
}

void PaymentBook::onRowMoved(int row, const AddressRow& before)
{
    // What had no answer before tells nothing: the first look at a wallet finds all it ever had.
    if (!before.checked)
        return;
    const AddressRow& now = model_->at(row);
    if (now.isUtxo()) {
        if (now.status == before.status)
            return;
        const auto own = std::find_if(books_.cbegin(), books_.cend(), [&now](const HistoryBook* book) { return book->network() == now.network; });
        if (own != books_.cend() && (*own)->statusOf(now.scripthash) == before.status)
            expecting_[now.network].insert(now.scripthash);
        return;
    }
    if (before.balances == now.balances)
        return;
    const qint64 clock = QDateTime::currentMSecsSinceEpoch();
    for (const Asset asset : networkInfo(now.network).assets) {
        const chain::Amount was = before.amount(asset);
        const chain::Amount is = now.amount(asset);
        const int order = is.compare(was);
        if (order == 0)
            continue;
        const QString key = assetInfo(asset).key + QLatin1Char('/') + now.address;
        if (order < 0) {
            falls_.insert(key, Fall{was, clock});
            continue;
        }
        const auto fell = falls_.constFind(key);
        const bool back = fell != falls_.cend() && fell->from == is && clock - fell->at < kFallMs;
        if (fell != falls_.cend())
            falls_.erase(fell);
        if (back)
            continue;
        chain::Amount gain = is;
        gain -= was;
        if (!isDust(asset, gain, {row}))
            record(asset, gain, now.wallet, now.address, 1, nullptr);
    }
}

void PaymentBook::onAdded(HistoryBook* book, int position)
{
    const TxRecord& tx = book->at(position);
    const QSet<QString> expected = expecting_.value(book->network());
    if (expected.isEmpty() || told_.contains(tx.txid))
        return;
    qint64 received = 0;
    bool news = false;
    QList<int> rows;
    QString first;
    bool firstIsChange = false;
    quint32 wallet = 0;
    int addresses = 0;
    for (const TxEffect& effect : book->effectsOf(position)) {
        // Something of the wallets' paid for it: their own change, or a move.
        if (effect.spent > 0)
            return;
        if (effect.received <= 0)
            continue;
        const HistoryBook::AddressEntry& entry = book->addressAt(effect.address);
        news = news || expected.contains(entry.scripthash);
        received += effect.received;
        ++addresses;
        const int row = model_->findByScripthash(book->network(), entry.scripthash);
        if (row < 0)
            continue;
        rows.append(row);
        // A receive address first: what came to a change address is the odd one out.
        const AddressRow& it = model_->at(row);
        if (first.isEmpty() || (firstIsChange && it.change == 0)) {
            first = it.address;
            wallet = it.wallet;
            firstIsChange = it.change != 0;
        }
    }
    if (!news || received <= 0 || first.isEmpty())
        return;
    const chain::Amount amount = chain::Amount::fromU64(quint64(received));
    const QString assetKey = assetInfo(networkInfo(book->network()).coin).key;
    // The same payment with a higher fee: it takes the old one's place, and is no news.
    for (qsizetype i = payments_.size() - 1; i >= 0 && i >= payments_.size() - kLookBack; --i) {
        Payment& earlier = payments_[i];
        if (!earlier.hasTx || earlier.asset != assetKey || earlier.amount != amount)
            continue;
        const int old = book->find(earlier.txid);
        if (old >= 0 && spendTheSame(book->at(old), tx)) {
            earlier.txid = tx.txid;
            told_.insert(tx.txid);
            emit recorded(earlier);
            emit changed();
            return;
        }
    }
    if (isDust(networkInfo(book->network()).coin, amount, rows))
        return;
    told_.insert(tx.txid);
    record(networkInfo(book->network()).coin, amount, wallet, first, addresses, &tx.txid);
}

bool PaymentBook::isDust(Asset asset, const chain::Amount& amount, const QList<int>& rows) const
{
    if (!hooks_.hideDust || !hooks_.hideDust() || amount.compare(assetInfo(asset).dust) >= 0)
        return false;
    for (const int row : rows) {
        const AddressRow& it = model_->at(row);
        if ((hooks_.givenOut && hooks_.givenOut(it)) || (hooks_.labelled && hooks_.labelled(it.address)))
            return false;
    }
    return true;
}

void PaymentBook::record(Asset asset, const chain::Amount& amount, quint32 wallet, const QString& address, int addresses, const chain::Hash256* txid)
{
    const AssetInfo& info = assetInfo(asset);
    Payment payment;
    payment.at = QDateTime::currentMSecsSinceEpoch();
    // The clock can go back: what came later still reads as later.
    payment.id = std::max(lastId_ + 1, payment.at);
    lastId_ = payment.id;
    payment.asset = info.key;
    payment.amount = amount;
    try {
        payment.usd = hooks_.price ? amount.toDouble(info.decimals) * hooks_.price(asset) : 0.0;
    } catch (const std::exception&) {
        payment.usd = 0.0;
    }
    payment.wallet = wallet;
    payment.address = address;
    payment.addresses = addresses;
    if (txid) {
        payment.txid = *txid;
        payment.hasTx = true;
    }
    payment.whileClosed = hooks_.starting && hooks_.starting();
    payments_.append(payment);
    ++unread_;
    if (payments_.size() > kMaxKept) {
        if (!payments_.constFirst().read)
            --unread_;
        payments_.removeFirst();
    }
    emit recorded(payment);
    emit changed();
    toTell_.append(payment);
    if (!pause_->isActive())
        pause_->start(gPauseMs);
}

void PaymentBook::tellNow()
{
    const QList<Payment> payments = std::exchange(toTell_, {});
    if (!payments.isEmpty())
        emit told(payments);
}

void PaymentBook::markAllRead()
{
    if (unread_ == 0)
        return;
    for (Payment& payment : payments_)
        payment.read = true;
    unread_ = 0;
    emit readUpTo(lastId_);
    emit changed();
}

void PaymentBook::addSaved(const QList<Payment>& saved)
{
    QList<Payment> all = saved;
    all.append(payments_);
    std::stable_sort(all.begin(), all.end(), [](const Payment& a, const Payment& b) { return a.id < b.id; });
    payments_ = all;
    if (payments_.size() > kMaxKept)
        payments_.remove(0, payments_.size() - kMaxKept);
    unread_ = 0;
    for (const Payment& payment : std::as_const(payments_)) {
        lastId_ = std::max(lastId_, payment.id);
        if (payment.hasTx)
            told_.insert(payment.txid);
        if (!payment.read)
            ++unread_;
    }
    emit changed();
}

void PaymentBook::forgetWallet(quint32 wallet)
{
    const qsizetype gone = payments_.removeIf([wallet](const Payment& payment) { return payment.wallet == wallet; });
    toTell_.removeIf([wallet](const Payment& payment) { return payment.wallet == wallet; });
    if (gone == 0)
        return;
    unread_ = int(std::count_if(payments_.cbegin(), payments_.cend(), [](const Payment& payment) { return !payment.read; }));
    emit walletForgotten(wallet);
    emit changed();
}

void PaymentBook::clear()
{
    pause_->stop();
    payments_.clear();
    toTell_.clear();
    told_.clear();
    expecting_.clear();
    falls_.clear();
    unread_ = 0;
    emit changed();
}

} // namespace wallet
