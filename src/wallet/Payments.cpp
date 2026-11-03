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
// Payments coming one after the other wait for each other this many pauses at most.
constexpr int kPausesAtMost = 30;
// A server can stop listing what another still does, and a fee bump is read a moment after the list
// that left its old transaction out: so long before anything is said of it, in pauses.
constexpr int kGracePauses = 30;
// Without a chain or a look at the servers to go by, one source behind the others takes about this long.
constexpr qint64 kFallMs = 15 * 60 * 1000;
// A transaction that took the place of another came soon after it.
constexpr qsizetype kLookBack = 64;
// Blocks under one, for it to be no longer worth watching.
constexpr int kDeepEnough = 6;

bool spendTheSame(const TxRecord& a, const TxRecord& b)
{
    for (const chain::OutPoint& point : a.inputs) {
        if (std::find(b.inputs.cbegin(), b.inputs.cend(), point) != b.inputs.cend())
            return true;
    }
    return false;
}

// Whether two transactions pay the wallets' addresses the same, in whatever order their outputs come.
bool payTheSame(const HistoryBook* book, int a, int b)
{
    QHash<int, qint64> paid;
    for (const TxEffect& effect : book->effectsOf(a)) {
        if (effect.received > 0)
            paid[effect.address] += effect.received;
    }
    for (const TxEffect& effect : book->effectsOf(b)) {
        if (effect.received <= 0)
            continue;
        const auto found = paid.find(effect.address);
        if (found == paid.end() || *found != effect.received)
            return false;
        paid.erase(found);
    }
    return paid.isEmpty();
}

QString firstScanKey(Network network, quint32 wallet)
{
    return QStringLiteral("first-scan/%1/%2").arg(networkInfo(network).id).arg(wallet);
}

QString expectedKey(Network network, const QString& scripthash)
{
    return QStringLiteral("expecting/%1/%2").arg(networkInfo(network).id, scripthash);
}

QString assetsKey(Network network)
{
    return QStringLiteral("assets/%1").arg(networkInfo(network).id);
}

} // namespace

void PaymentNews::add(const PaymentNews& more)
{
    inABlock += more.inABlock;
    gone += more.gone;
    listedAgain += more.listedAgain;
    leftABlock += more.leftABlock;
    noProof += more.noProof;
    wentBack += more.wentBack;
}

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
    lookAgain_ = new QTimer(this);
    lookAgain_->setSingleShot(true);
    connect(lookAgain_, &QTimer::timeout, this, &PaymentBook::lookAtWaiting);
    stateSave_ = new QTimer(this);
    stateSave_->setSingleShot(true);
    connect(stateSave_, &QTimer::timeout, this, &PaymentBook::flushState);
    connect(model_, &WalletModel::rowMoved, this, &PaymentBook::onRowMoved);
    connect(model_, &WalletModel::rowsAppended, this, &PaymentBook::onRowsAppended);
    for (HistoryBook* book : std::as_const(books_)) {
        connect(book, &HistoryBook::added, this, [this, book](int position) { onAdded(book, position); });
        connect(book, &HistoryBook::spendFound, this, [this, book](int position) { onSpendFound(book, position); });
        connect(book, &HistoryBook::statusSet, this, [this, book](const QString& scripthash) { onStatusSet(book, scripthash); });
        connect(book, &HistoryBook::cleared, this, [this, book] {
            const QHash<QString, Expected> were = expecting_.take(book->network());
            for (auto it = were.cbegin(); it != were.cend(); ++it)
                saveLater(expectedKey(book->network(), it.key()), QString());
        });
        connect(book, &HistoryBook::changed, this, [this] {
            if (!waiting_.isEmpty() && (!lookAgain_->isActive() || lookAgain_->remainingTime() > gPauseMs))
                lookAgain_->start(gPauseMs);
        });
    }
}

HistoryBook* PaymentBook::bookOf(Network network) const
{
    const auto own = std::find_if(books_.cbegin(), books_.cend(), [network](const HistoryBook* book) { return book->network() == network; });
    return own != books_.cend() ? *own : nullptr;
}

const Payment* PaymentBook::findPayment(qint64 id) const
{
    const auto found = std::lower_bound(payments_.cbegin(), payments_.cend(), id, [](const Payment& one, qint64 wanted) { return one.id < wanted; });
    return found != payments_.cend() && found->id == id ? &*found : nullptr;
}

qint64 PaymentBook::fallWindow(Network network) const
{
    const qint64 window = hooks_.fallWindowMs ? hooks_.fallWindowMs(network) : 0;
    return window > 0 ? window : kFallMs;
}

void PaymentBook::saveLater(const QString& key, const QString& value)
{
    stateChanges_.insert(key, value);
    if (!stateSave_->isActive())
        stateSave_->start(gPauseMs);
}

void PaymentBook::flushState()
{
    stateSave_->stop();
    const QHash<QString, QString> changes = std::exchange(stateChanges_, {});
    if (!changes.isEmpty())
        emit stateToSave(changes);
}

void PaymentBook::expect(Network network, const QString& scripthash, const Expected& expected)
{
    expecting_[network].insert(scripthash, expected);
    saveLater(expectedKey(network, scripthash), QString::number(expected.above));
}

void PaymentBook::stopExpecting(Network network, const QString& scripthash)
{
    const auto of = expecting_.find(network);
    if (of != expecting_.end() && of->remove(scripthash))
        saveLater(expectedKey(network, scripthash), QString());
}

void PaymentBook::onRowsAppended(int first, int last)
{
    if (gapsChanged_)
        return;
    for (int row = first; row <= last; ++row) {
        const AddressRow& it = model_->at(row);
        // Its window reached it because of what was paid: what it holds is news, unlike all a wallet
        // ever held on the rows of its first look.
        if (!it.isUtxo() && firstScan_.value(it.network).contains(it.wallet))
            grown_.insert(it.address);
    }
}

void PaymentBook::onRowMoved(int row, const AddressRow& before)
{
    const AddressRow& now = model_->at(row);
    const bool firstAnswer = !before.checked;
    if (now.isUtxo()) {
        if (now.status == before.status)
            return;
        // Known only by its balance, as no server would read it: that is no history complete up to anything.
        if (before.used && before.status.isEmpty())
            return;
        Expected expected;
        if (firstAnswer) {
            // The first look at a wallet finds all it ever had. A row its window reached later finds
            // that too, and what is newer than that first look.
            const QHash<quint32, int> scanned = firstScan_.value(now.network);
            const auto of = scanned.constFind(now.wallet);
            if (of == scanned.cend() || *of <= 0)
                return;
            expected.above = *of;
        }
        const HistoryBook* book = bookOf(now.network);
        if (book && book->statusOf(now.scripthash) == before.status) {
            expected.whileClosed = hooks_.starting && hooks_.starting(now);
            expect(now.network, now.scripthash, expected);
        }
        return;
    }
    if (firstAnswer && !grown_.remove(now.address))
        return;
    if (before.balances == now.balances)
        return;
    const qint64 clock = QDateTime::currentMSecsSinceEpoch();
    const qint64 window = fallWindow(now.network);
    const QSet<QString> readWith = assetsRead_.value(now.network);
    for (const Asset asset : networkInfo(now.network).assets) {
        const chain::Amount was = before.amount(asset);
        const chain::Amount is = now.amount(asset);
        const int order = is.compare(was);
        if (order == 0)
            continue;
        const QString key = assetInfo(asset).key + QLatin1Char('/') + now.address;
        if (order < 0) {
            falls_.insert(key, Fall{was, clock});
            const auto gain = gains_.constFind(key);
            if (gain != gains_.cend()) {
                // Told of a moment ago, and taken back: whoever counted on it should know.
                if (clock - gain->at < window && is.compare(gain->before) <= 0) {
                    if (const Payment* payment = findPayment(gain->payment)) {
                        held_.wentBack.append(*payment);
                        if (!lookAgain_->isActive() || lookAgain_->remainingTime() > gPauseMs)
                            lookAgain_->start(gPauseMs);
                    }
                }
                gains_.erase(gain);
            }
            continue;
        }
        // A token new on the list of coins: what every address long held of it is no news.
        if (!readWith.contains(assetInfo(asset).key))
            continue;
        const auto fell = falls_.constFind(key);
        chain::Amount from = was;
        bool back = false;
        if (fell != falls_.cend()) {
            if (clock - fell->at < window) {
                const int against = is.compare(fell->from);
                back = against == 0;
                if (against > 0)
                    from = fell->from; // the source behind caught up, with something new on top
            }
            falls_.erase(fell);
        }
        if (back)
            continue;
        chain::Amount gain = is;
        gain -= from;
        if (!isDust(asset, gain, {row})) {
            record(asset, gain, now.wallet, now.address, 1, nullptr, hooks_.starting && hooks_.starting(now));
            gains_.insert(key, Gain{from, lastId_, clock});
        }
    }
}

void PaymentBook::onAdded(HistoryBook* book, int position)
{
    const TxRecord& tx = book->at(position);
    const auto expected = expecting_.constFind(book->network());
    if (expected == expecting_.cend() || expected->isEmpty() || told_.contains(tx.txid))
        return;
    qint64 received = 0;
    bool news = false;
    bool whileClosed = false;
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
        const auto expects = expected->constFind(entry.scripthash);
        if (expects != expected->cend() && (tx.height <= 0 || tx.height > expects->above)) {
            news = true;
            whileClosed = whileClosed || expects->whileClosed;
        }
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
    const chain::Hash256 txid = tx.txid;
    // The same payment with a higher fee: it takes the old one's place, and is no news. One that pays
    // another of the wallets' addresses is: what the old one was for has not been paid.
    for (qsizetype i = payments_.size() - 1; i >= 0 && i >= payments_.size() - kLookBack; --i) {
        Payment& earlier = payments_[i];
        if (!earlier.hasTx || earlier.asset != assetKey || earlier.amount != amount)
            continue;
        const int old = book->find(earlier.txid);
        if (old >= 0 && old != position && spendTheSame(book->at(old), tx) && payTheSame(book, old, position)) {
            Watched watched = waiting_.take(earlier.txid);
            watched.payment = earlier.id;
            watched.goneSince = 0;
            waiting_.insert(txid, watched);
            for (Payment& waitingToBeTold : toTell_) {
                if (waitingToBeTold.id == earlier.id)
                    waitingToBeTold.txid = txid;
            }
            earlier.txid = txid;
            told_.insert(txid);
            emit recorded(earlier);
            emit changed();
            return;
        }
    }
    if (isDust(networkInfo(book->network()).coin, amount, rows))
        return;
    told_.insert(txid);
    record(networkInfo(book->network()).coin, amount, wallet, first, addresses, &txid, whileClosed);
    // A first read is no proof, whatever height a server gives it, and whatever a block is said to hold
    // can give way: all of it is watched.
    Watched watched;
    watched.payment = lastId_;
    waiting_.insert(txid, watched);
}

void PaymentBook::onSpendFound(HistoryBook* book, int position)
{
    const chain::Hash256 txid = book->at(position).txid;
    if (!told_.contains(txid))
        return;
    // What it spends was read after it: it was the wallets' own all along, change or a move. It stays
    // in told_, so that nothing tells it of again.
    const QString assetKey = assetInfo(networkInfo(book->network()).coin).key;
    for (qsizetype i = payments_.size() - 1; i >= 0; --i) {
        if (!payments_.at(i).hasTx || payments_.at(i).txid != txid || payments_.at(i).asset != assetKey)
            continue;
        const qint64 id = payments_.at(i).id;
        if (!payments_.at(i).read)
            --unread_;
        payments_.remove(i);
        toTell_.removeIf([id](const Payment& one) { return one.id == id; });
        waiting_.remove(txid);
        emit removed(id);
        emit changed();
        return;
    }
}

void PaymentBook::onStatusSet(HistoryBook* book, const QString& scripthash)
{
    // Whole up to where the row stands: whatever is read of it from here on moved it again first.
    const int row = model_->findByScripthash(book->network(), scripthash);
    if (row >= 0 && model_->at(row).status == book->statusOf(scripthash))
        stopExpecting(book->network(), scripthash);
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

void PaymentBook::record(Asset asset, const chain::Amount& amount, quint32 wallet, const QString& address, int addresses, const chain::Hash256* txid, bool whileClosed)
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
    payment.whileClosed = whileClosed;
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
    // A stream of them is one thing to say too, within reason; one alone goes after a pause.
    const qint64 clock = QDateTime::currentMSecsSinceEpoch();
    if (!pause_->isActive())
        pauseSince_ = clock;
    const qint64 left = pauseSince_ + qint64(gPauseMs) * kPausesAtMost - clock;
    pause_->start(int(std::clamp<qint64>(left, 0, gPauseMs)));
}

void PaymentBook::tellNow()
{
    const QList<Payment> payments = std::exchange(toTell_, {});
    if (payments.isEmpty())
        return;
    // Proven by now, it is told of once, as received.
    for (const Payment& payment : payments) {
        const auto watched = payment.hasTx ? waiting_.find(payment.txid) : waiting_.end();
        const AssetInfo* info = payment.hasTx ? findAsset(payment.asset) : nullptr;
        const HistoryBook* book = info ? bookOf(info->network) : nullptr;
        const int position = book ? book->find(payment.txid) : -1;
        if (watched != waiting_.end() && position >= 0 && book->at(position).seen == TxSeen::Alive && book->at(position).height > 0 && book->at(position).proven)
            watched->said = Said::InABlock;
    }
    emit told(payments);
}

void PaymentBook::lookAtWaiting()
{
    PaymentNews news = std::exchange(held_, {});
    QSet<qint64> notTold;
    for (const Payment& payment : std::as_const(toTell_))
        notTold.insert(payment.id);
    const qint64 clock = QDateTime::currentMSecsSinceEpoch();
    const qint64 grace = qint64(gPauseMs) * kGracePauses;
    qint64 soonest = grace; // however it stands, a proof that fails says nothing to the book
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        const Payment* payment = findPayment(it->payment);
        const AssetInfo* info = payment ? findAsset(payment->asset) : nullptr;
        const HistoryBook* book = info ? bookOf(info->network) : nullptr;
        const int position = book ? book->find(it.key()) : -1;
        // Forgotten, or the wallet it was for is gone: nothing left to say of it.
        if (position < 0) {
            it = waiting_.erase(it);
            continue;
        }
        // What has not been told of yet has nothing to take back.
        if (notTold.contains(it->payment)) {
            ++it;
            continue;
        }
        const TxRecord& tx = book->at(position);
        Watched& watched = *it;
        // How long this has gone on for, and whether that is long enough to say so.
        const auto dueNow = [&](qint64* since) {
            if (*since == 0)
                *since = clock;
            const qint64 left = *since + grace - clock;
            if (left > 0)
                soonest = std::min(soonest, left);
            return left <= 0;
        };
        if (tx.seen != TxSeen::Alive) {
            watched.leftSince = 0;
            watched.failedSince = 0;
            if (watched.said != Said::Gone && dueNow(&watched.goneSince)) {
                news.gone.append(*payment);
                watched.said = Said::Gone;
                watched.noProofSaid = false;
            }
            ++it;
            continue;
        }
        watched.goneSince = 0;
        if (watched.said == Said::Gone) {
            news.listedAgain.append(*payment);
            watched.said = Said::Coming;
        }
        if (tx.height > 0 && tx.proven) {
            watched.leftSince = 0;
            watched.failedSince = 0;
            watched.noProofSaid = false;
            if (watched.said != Said::InABlock) {
                news.inABlock.append(*payment);
                watched.said = Said::InABlock;
            }
            const int tip = hooks_.tip ? hooks_.tip(info->network) : -1;
            if (tip >= tx.height + kDeepEnough - 1) {
                it = waiting_.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        if (tx.height <= 0) {
            watched.failedSince = 0;
            if (watched.said == Said::InABlock && dueNow(&watched.leftSince)) {
                news.leftABlock.append(*payment);
                watched.said = Said::Coming;
                watched.leftSince = 0;
            }
            ++it;
            continue;
        }
        // In a block on a server's word. A proof refused is asked for again, and can still come.
        watched.leftSince = 0;
        const bool failed = hooks_.proofFailed && hooks_.proofFailed(info->network, tx.txid);
        if (!failed) {
            watched.failedSince = 0;
        } else if (!watched.noProofSaid && dueNow(&watched.failedSince)) {
            news.noProof.append(*payment);
            watched.noProofSaid = true;
            if (watched.said == Said::InABlock)
                watched.said = Said::Coming;
        }
        ++it;
    }
    if (!waiting_.isEmpty() && !lookAgain_->isActive())
        lookAgain_->start(int(std::max<qint64>(1, soonest)));
    if (!news.isEmpty())
        emit settled(news);
}

void PaymentBook::markAllRead()
{
    markReadUpTo(lastId_);
}

void PaymentBook::markReadUpTo(qint64 id)
{
    if (unread_ == 0)
        return;
    int left = 0;
    for (Payment& payment : payments_) {
        if (payment.id <= id)
            payment.read = true;
        else if (!payment.read)
            ++left;
    }
    unread_ = left;
    emit readUpTo(id);
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

void PaymentBook::restoreState(const QHash<QString, QString>& state)
{
    for (auto it = state.cbegin(); it != state.cend(); ++it) {
        const QStringList parts = it.key().split(QLatin1Char('/'));
        const NetworkInfo* network = parts.size() >= 2 ? findNetwork(parts.at(1)) : nullptr;
        if (!network)
            continue;
        if (parts.at(0) == QLatin1String("first-scan") && parts.size() == 3) {
            firstScan_[network->network].insert(parts.at(2).toUInt(), it.value().toInt());
        } else if (parts.at(0) == QLatin1String("expecting") && parts.size() == 3) {
            // Noticed last time and not read yet: by now it came while this was closed.
            Expected expected;
            expected.whileClosed = true;
            expected.above = it.value().toInt();
            expecting_[network->network].insert(parts.at(2), expected);
        } else if (parts.at(0) == QLatin1String("assets") && parts.size() == 2) {
            const QStringList assets = it.value().split(QLatin1Char(','), Qt::SkipEmptyParts);
            assetsRead_[network->network] = QSet<QString>(assets.cbegin(), assets.cend());
        }
    }
}

void PaymentBook::watchSaved()
{
    for (const Payment& payment : std::as_const(payments_)) {
        if (!payment.hasTx || waiting_.contains(payment.txid))
            continue;
        const AssetInfo* info = findAsset(payment.asset);
        const HistoryBook* book = info ? bookOf(info->network) : nullptr;
        const int position = book ? book->find(payment.txid) : -1;
        if (position < 0)
            continue;
        const TxRecord& tx = book->at(position);
        const bool inABlock = tx.height > 0 && tx.proven;
        if (tx.seen == TxSeen::Alive && inABlock)
            continue;
        Watched watched;
        watched.payment = payment.id;
        watched.said = tx.seen == TxSeen::Alive ? Said::Coming : Said::Gone;
        waiting_.insert(payment.txid, watched);
    }
    if (!waiting_.isEmpty() && !lookAgain_->isActive())
        lookAgain_->start(gPauseMs);
}

void PaymentBook::noteAnswered(Network network, int tip, const QList<quint32>& wallets, const QStringList& assets)
{
    // Whatever the gaps brought back has answered: from here on the window grows by what was paid.
    gapsChanged_ = false;
    QHash<quint32, int>& scanned = firstScan_[network];
    for (const quint32 wallet : wallets) {
        if (scanned.contains(wallet))
            continue;
        scanned.insert(wallet, std::max(tip, 0));
        saveLater(firstScanKey(network, wallet), QString::number(std::max(tip, 0)));
    }
    QSet<QString>& read = assetsRead_[network];
    bool more = false;
    for (const QString& asset : assets) {
        if (!read.contains(asset)) {
            read.insert(asset);
            more = true;
        }
    }
    if (more) {
        QStringList sorted(read.cbegin(), read.cend());
        sorted.sort();
        saveLater(assetsKey(network), sorted.join(QLatin1Char(',')));
    }
}

void PaymentBook::noteGapsChanged()
{
    gapsChanged_ = true;
    grown_.clear();
}

void PaymentBook::forgetWallet(quint32 wallet)
{
    // Whether or not it was ever paid: its addresses can have moved, and it can be added again.
    for (int row = 0; row < model_->size(); ++row) {
        const AddressRow& it = model_->at(row);
        if (it.wallet != wallet)
            continue;
        if (it.isUtxo()) {
            stopExpecting(it.network, it.scripthash);
        } else {
            grown_.remove(it.address);
            for (const Asset asset : networkInfo(it.network).assets) {
                const QString key = assetInfo(asset).key + QLatin1Char('/') + it.address;
                falls_.remove(key);
                gains_.remove(key);
            }
        }
    }
    for (auto it = firstScan_.begin(); it != firstScan_.end(); ++it) {
        if (it->remove(wallet))
            saveLater(firstScanKey(it.key(), wallet), QString());
    }
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
    lookAgain_->stop();
    stateSave_->stop();
    told_.clear();
    expecting_.clear();
    firstScan_.clear();
    assetsRead_.clear();
    grown_.clear();
    gapsChanged_ = false;
    falls_.clear();
    gains_.clear();
    waiting_.clear();
    held_ = {};
    stateChanges_.clear();
    unread_ = 0;
    emit changed();
}

} // namespace wallet
