#include "wallet/History.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <ranges>
#include <utility>

namespace wallet {

namespace {

// Past this many effects on one transaction, a look along them gives way to an index.
constexpr qsizetype kIndexEffectsFrom = 32;

bool standsPending(const TxRecord& tx)
{
    return tx.seen == TxSeen::Alive && tx.height <= 0;
}

} // namespace

QByteArray writeLabelLine(const LabelLine& line)
{
    QJsonObject object;
    if (line.isTransaction) {
        object.insert(QStringLiteral("type"), QStringLiteral("tx"));
        object.insert(QStringLiteral("ref"), QString::fromStdString(chain::hashHex(line.txid)));
        if (!line.network.isEmpty())
            object.insert(QStringLiteral("network"), line.network);
    } else {
        object.insert(QStringLiteral("type"), QStringLiteral("addr"));
        object.insert(QStringLiteral("ref"), line.address);
    }
    object.insert(QStringLiteral("label"), line.label);
    if (!line.note.isEmpty())
        object.insert(QStringLiteral("note"), line.note);
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QVector<LabelLine> readLabelLines(const QByteArray& file, int* skipped)
{
    QVector<LabelLine> out;
    int left = 0;
    for (const QByteArrayView text : QByteArrayView(file) | std::views::split('\n')
                                        | std::views::transform([](auto part) { return QByteArrayView(part.data(), qsizetype(part.size())); })) {
        const QByteArray trimmed = text.toByteArray().trimmed();
        if (trimmed.isEmpty())
            continue;
        QJsonParseError error;
        const QJsonObject object = QJsonDocument::fromJson(trimmed, &error).object();
        const QString type = object.value(QLatin1String("type")).toString();
        const QString ref = object.value(QLatin1String("ref")).toString();
        LabelLine line;
        line.label = object.value(QLatin1String("label")).toString();
        line.note = object.value(QLatin1String("note")).toString();
        line.network = object.value(QLatin1String("network")).toString();
        bool usable = error.error == QJsonParseError::NoError && (!line.label.isEmpty() || !line.note.isEmpty());
        if (type == QLatin1String("tx")) {
            usable = usable && chain::hashFromHex(ref.toStdString(), &line.txid);
        } else if (type == QLatin1String("addr")) {
            line.isTransaction = false;
            line.address = ref;
            usable = usable && !ref.isEmpty();
        } else {
            usable = false;
        }
        if (usable)
            out.append(line);
        else
            ++left;
    }
    if (skipped)
        *skipped = left;
    return out;
}

HistoryBook::HistoryBook(Network network, QObject* parent) : QObject(parent), network_(network)
{
}

void HistoryBook::clear()
{
    txs_.clear();
    effects_.clear();
    positions_.clear();
    owned_.clear();
    spenders_.clear();
    addresses_.clear();
    addressIds_.clear();
    statuses_.clear();
    notes_.clear();
    effectIndex_.clear();
    lines_ = 0;
    pending_ = 0;
    dirtyTxs_.clear();
    removedTxs_.clear();
    dirtyAddresses_.clear();
    removedAddresses_.clear();
    headers_.clear();
    dirtyHeaders_.clear();
    dropHeadersFrom_ = -1;
    emit positionsMoved();
    emit cleared();
    emit changed();
}

int HistoryBook::find(const chain::Hash256& txid) const
{
    return positions_.value(txid, -1);
}

int HistoryBook::findAddress(const QString& scripthash) const
{
    return addressIds_.value(scripthash, -1);
}

int HistoryBook::addressId(const QString& scripthash)
{
    const auto found = addressIds_.constFind(scripthash);
    if (found != addressIds_.cend())
        return *found;
    const int id = int(addresses_.size());
    AddressEntry entry;
    entry.scripthash = scripthash;
    addresses_.append(std::move(entry));
    addressIds_.insert(scripthash, id);
    return id;
}

void HistoryBook::describeAddress(const QString& scripthash, quint32 wallet, const QString& address)
{
    AddressEntry& entry = addresses_[addressId(scripthash)];
    if (entry.wallet == wallet && entry.address == address)
        return;
    entry.wallet = wallet;
    entry.address = address;
    if (statuses_.contains(scripthash))
        dirtyAddresses_.insert(scripthash);
}

qint64 HistoryBook::netOf(const QString& scripthash, const QSet<chain::Hash256>* leftOut) const
{
    const int id = findAddress(scripthash);
    if (id < 0)
        return 0;
    qint64 net = 0;
    for (const int position : addresses_.at(id).txs) {
        if (txs_.at(position).seen != TxSeen::Alive || (leftOut && leftOut->contains(txs_.at(position).txid)))
            continue;
        for (const TxEffect& effect : effects_.at(position)) {
            if (effect.address == id)
                net += effect.received - effect.spent;
        }
    }
    return net;
}

bool HistoryBook::hasEffectOn(int position, const QString& scripthash) const
{
    const int id = findAddress(scripthash);
    if (id < 0)
        return false;
    const auto indexed = effectIndex_.constFind(position);
    if (indexed != effectIndex_.cend())
        return indexed->contains(id);
    const QVector<TxEffect>& effects = effects_.at(position);
    return std::any_of(effects.cbegin(), effects.cend(), [id](const TxEffect& e) { return e.address == id; });
}

void HistoryBook::addEffect(int position, int address, qint64 received, qint64 spent)
{
    QVector<TxEffect>& effects = effects_[position];
    const auto indexed = effectIndex_.find(position);
    if (indexed != effectIndex_.end()) {
        const auto at = indexed->constFind(address);
        if (at != indexed->cend()) {
            effects[*at].received += received;
            effects[*at].spent += spent;
            return;
        }
        indexed->insert(address, int(effects.size()));
    } else {
        for (TxEffect& effect : effects) {
            if (effect.address == address) {
                effect.received += received;
                effect.spent += spent;
                return;
            }
        }
        if (effects.size() + 1 == kIndexEffectsFrom) {
            QHash<int, int>& index = effectIndex_[position];
            for (int at = 0; at < effects.size(); ++at)
                index.insert(effects.at(at).address, at);
            index.insert(address, int(effects.size()));
        }
    }
    if (effects.isEmpty())
        ++lines_;
    effects.append({address, received, spent});
    addresses_[address].txs.append(position);
}

void HistoryBook::noteOutput(int position, int outputAt)
{
    OurOutput& output = txs_[position].outputs[outputAt];
    const int address = addressId(output.scripthash);
    output.scripthash = addresses_.at(address).scripthash; // one string, however many outputs pay it
    const chain::OutPoint point{txs_.at(position).txid, output.n};
    owned_.insert(point, {address, output.value});
    addEffect(position, address, output.value, 0);
    // Whatever spends it may have come first. A fee and a conflict both wait for it.
    for (auto it = spenders_.constFind(point); it != spenders_.cend() && it.key() == point; ++it) {
        addEffect(*it, address, 0, output.value);
        updateFee(*it);
        touch(*it);
    }
}

void HistoryBook::noteInput(int position, const chain::OutPoint& point)
{
    spenders_.insert(point, position);
    const auto owned = owned_.constFind(point);
    if (owned != owned_.cend())
        addEffect(position, owned->address, 0, owned->value);
}

void HistoryBook::updateFee(int position)
{
    TxRecord& tx = txs_[position];
    qint64 in = 0;
    for (const chain::OutPoint& point : tx.inputs) {
        const auto owned = owned_.constFind(point);
        if (owned == owned_.cend())
            return; // what the rest comes to is someone else's to know
        in += owned->value;
    }
    if (in >= tx.outTotal)
        tx.fee = in - tx.outTotal;
}

void HistoryBook::touch(int position)
{
    dirtyTxs_.insert(position);
}

int HistoryBook::add(const chain::Transaction& tx, int height, qint64 firstSeen, const std::function<bool(const QString&)>& isOurs)
{
    int position = find(tx.txid);
    const bool known = position >= 0;
    if (!known) {
        position = int(txs_.size());
        TxRecord record;
        record.network = network_;
        record.txid = tx.txid;
        record.height = height;
        record.time = height > 0 ? 0 : firstSeen;
        record.vsize = tx.vsize;
        record.outTotal = qint64(tx.outputTotal());
        record.replaceable = tx.signalsReplaceable();
        record.inputs.reserve(tx.inputs.size());
        if (!tx.coinbase()) {
            for (const chain::TxInput& input : tx.inputs)
                record.inputs.push_back(input.previous);
        }
        if (standsPending(record))
            ++pending_;
        txs_.append(std::move(record));
        effects_.append(QVector<TxEffect>());
        positions_.insert(tx.txid, position);
        for (const chain::OutPoint& point : txs_.at(position).inputs)
            noteInput(position, point);
    }

    // Which it knows of already. A new one's are all new.
    QSet<quint32> have;
    if (known) {
        for (const OurOutput& output : std::as_const(txs_.at(position).outputs))
            have.insert(output.n);
    }
    bool found = false;
    for (quint32 n = 0; n < quint32(tx.outputs.size()); ++n) {
        const chain::TxOutput& output = tx.outputs[n];
        const QString scripthash = QString::fromStdString(chain::scripthashOfScript(output.script));
        if (!isOurs(scripthash) || have.contains(n))
            continue;
        OurOutput ours;
        ours.n = n;
        ours.value = qint64(output.value);
        ours.scripthash = scripthash;
        txs_[position].outputs.append(ours);
        noteOutput(position, int(txs_.at(position).outputs.size()) - 1);
        found = true;
    }
    updateFee(position);
    if (!known)
        settleConflicts(position);
    if (!known || found) {
        touch(position);
        emit changed();
    }
    return position;
}

void HistoryBook::addSaved(const QVector<TxRecord>& records)
{
    // Whatever was found before the database opened is new to it, and stays to be written.
    const QSet<int> dirtyBefore = dirtyTxs_;
    for (const TxRecord& record : records) {
        if (positions_.contains(record.txid))
            continue;
        const int position = int(txs_.size());
        if (standsPending(record))
            ++pending_;
        txs_.append(record);
        effects_.append(QVector<TxEffect>());
        positions_.insert(record.txid, position);
        for (const chain::OutPoint& point : record.inputs)
            noteInput(position, point);
        for (int at = 0; at < record.outputs.size(); ++at)
            noteOutput(position, at);
    }
    // What the rows brought is what the file holds already.
    dirtyTxs_ = dirtyBefore;
    emit changed();
}

void HistoryBook::addSavedAddresses(const QVector<AddressHistory>& addresses)
{
    for (const AddressHistory& saved : addresses) {
        AddressEntry& entry = addresses_[addressId(saved.scripthash)];
        entry.wallet = saved.wallet;
        entry.address = saved.address;
        statuses_.insert(saved.scripthash, saved.status);
    }
    emit changed();
}

void HistoryBook::addSavedHeaders(const QMap<int, QByteArray>& headers)
{
    headers_.insert(headers);
}

void HistoryBook::addSavedNotes(const QHash<chain::Hash256, TxNote>& notes)
{
    notes_.insert(notes);
    emit notesChanged();
}

bool HistoryBook::isReplacedIn(int position, chain::Hash256* by) const
{
    for (const chain::OutPoint& point : txs_.at(position).inputs) {
        for (auto it = spenders_.constFind(point); it != spenders_.cend() && it.key() == point; ++it) {
            if (*it != position && txs_.at(*it).seen == TxSeen::Alive) {
                *by = txs_.at(*it).txid;
                return true;
            }
        }
    }
    return false;
}

void HistoryBook::settleConflicts(int position)
{
    const TxRecord& tx = txs_.at(position);
    if (tx.seen != TxSeen::Alive)
        return;
    const chain::Hash256 txid = tx.txid;
    // On a proof: a server's word that a block holds it takes nothing from anything else.
    const bool inBlock = tx.height > 0 && tx.proven;
    for (const chain::OutPoint& point : tx.inputs) {
        for (auto it = spenders_.constFind(point); it != spenders_.cend() && it.key() == point; ++it) {
            const int other = *it;
            if (other == position)
                continue;
            TxRecord& rival = txs_[other];
            // What is proven to have spent those coins says what this is, whatever a list says.
            if (tx.height <= 0 && rival.seen == TxSeen::Alive && rival.proven && rival.height > 0) {
                TxRecord& self = txs_[position];
                --pending_;
                self.seen = TxSeen::Replaced;
                self.replacedBy = rival.txid;
                touch(position);
                return;
            }
            // A pending one loses to what a block holds. Two pending ones wait: a server lists one
            // of them at most, and the one it stops listing goes through markGone().
            if (rival.seen == TxSeen::Dropped || (rival.seen == TxSeen::Alive && inBlock && rival.height <= 0)) {
                if (standsPending(rival))
                    --pending_;
                rival.seen = TxSeen::Replaced;
                rival.replacedBy = txid;
                rival.proven = false;
                touch(other);
            }
        }
    }
}

void HistoryBook::setHeight(int position, int height)
{
    TxRecord& tx = txs_[position];
    const bool wasPending = standsPending(tx);
    bool moved = false;
    if (tx.height != height) {
        tx.height = height;
        tx.proven = false;
        tx.blockPos = -1;
        moved = true;
    }
    if (tx.seen != TxSeen::Alive) {
        tx.seen = TxSeen::Alive;
        tx.replacedBy = {};
        moved = true;
    }
    if (!moved)
        return;
    pending_ += int(standsPending(tx)) - int(wasPending);
    settleConflicts(position);
    touch(position);
    emit changed();
}

void HistoryBook::markGone(int position)
{
    TxRecord& tx = txs_[position];
    if (tx.seen != TxSeen::Alive)
        return;
    if (standsPending(tx))
        --pending_;
    tx.proven = false;
    // How long it stays goes by this, and one in a block that was never proven has none.
    if (tx.time <= 0)
        tx.time = QDateTime::currentSecsSinceEpoch();
    chain::Hash256 by{};
    if (isReplacedIn(position, &by)) {
        tx.seen = TxSeen::Replaced;
        tx.replacedBy = by;
    } else {
        tx.seen = TxSeen::Dropped;
    }
    touch(position);
    emit changed();
}

void HistoryBook::setProven(int position, bool proven, int blockPos)
{
    TxRecord& tx = txs_[position];
    if (tx.proven == proven && (blockPos < 0 || tx.blockPos == blockPos))
        return;
    const bool newly = proven && !tx.proven;
    tx.proven = proven;
    if (blockPos >= 0)
        tx.blockPos = blockPos;
    if (newly)
        settleConflicts(position);
    touch(position);
    emit changed();
}

void HistoryBook::setBlockTime(int position, qint64 time)
{
    TxRecord& tx = txs_[position];
    if (tx.time == time)
        return;
    tx.time = time;
    // A price for another moment says nothing of this one.
    tx.usdThen = 0.0;
    tx.usdThenAt = 0;
    touch(position);
    emit changed();
}

void HistoryBook::setVsize(int position, int vsize)
{
    TxRecord& tx = txs_[position];
    if (tx.vsize == vsize)
        return;
    tx.vsize = vsize;
    touch(position);
    emit changed();
}

void HistoryBook::setUsdThen(int position, double usd, qint64 at)
{
    TxRecord& tx = txs_[position];
    tx.usdThen = usd;
    tx.usdThenAt = at;
    touch(position);
    emit changed();
}

void HistoryBook::setStatus(const QString& scripthash, const QString& status)
{
    addressId(scripthash);
    statuses_.insert(scripthash, status);
    dirtyAddresses_.insert(scripthash);
    removedAddresses_.remove(scripthash);
    emit changed();
}

void HistoryBook::unproveFrom(int height)
{
    bool any = false;
    for (int position = 0; position < txs_.size(); ++position) {
        TxRecord& tx = txs_[position];
        if (tx.height >= height && tx.proven) {
            tx.proven = false;
            touch(position);
            any = true;
        }
    }
    if (any)
        emit changed();
}

void HistoryBook::forget(const QSet<QString>& scripthashes)
{
    if (scripthashes.isEmpty())
        return;
    QSet<int> going;
    for (const QString& scripthash : scripthashes) {
        const int id = findAddress(scripthash);
        if (id >= 0)
            going.insert(id);
        if (statuses_.remove(scripthash) > 0)
            removedAddresses_.insert(scripthash);
        dirtyAddresses_.remove(scripthash);
    }
    if (going.isEmpty())
        return;
    std::vector<char> leaving(size_t(txs_.size()), 0);
    for (int position = 0; position < txs_.size(); ++position) {
        // What has nothing on any address yet may be another wallet's, still on its way, with its note.
        const QVector<TxEffect>& theirs = effects_.at(position);
        leaving[size_t(position)] = !theirs.isEmpty() && !std::any_of(theirs.cbegin(), theirs.cend(), [&going](const TxEffect& e) { return !going.contains(e.address); });
    }
    removeRecords(leaving, going, scripthashes);
}

void HistoryBook::forgetGoneBefore(qint64 seconds)
{
    std::vector<char> leaving(size_t(txs_.size()), 0);
    bool any = false;
    for (int position = 0; position < txs_.size(); ++position) {
        const TxRecord& tx = txs_.at(position);
        if (tx.seen == TxSeen::Alive || tx.time <= 0 || tx.time >= seconds || notes_.contains(tx.txid))
            continue;
        const bool spent = std::any_of(tx.outputs.cbegin(), tx.outputs.cend(), [this, &tx](const OurOutput& output) {
            return spenders_.contains(chain::OutPoint{tx.txid, output.n});
        });
        if (!spent) {
            leaving[size_t(position)] = 1;
            any = true;
        }
    }
    if (any)
        removeRecords(leaving, {}, {});
}

// In place, whatever the size: no record is copied and no key hashed again, and an address that goes
// keeps its number, empty, until the next start.
void HistoryBook::removeRecords(const std::vector<char>& leaving, const QSet<int>& goingAddresses, const QSet<QString>& goingScripthashes)
{
    emit positionsAboutToMove();
    std::vector<int> movedTo(leaving.size(), -1);
    int next = 0;
    lines_ = 0;
    pending_ = 0;
    QSet<int> dirty;
    for (int position = 0; position < txs_.size(); ++position) {
        TxRecord& record = txs_[position];
        if (leaving[size_t(position)]) {
            removedTxs_.append(record.txid);
            notes_.remove(record.txid);
            for (const OurOutput& output : std::as_const(record.outputs))
                owned_.remove(chain::OutPoint{record.txid, output.n});
            positions_.remove(record.txid);
            continue;
        }
        const int to = next++;
        movedTo[size_t(position)] = to;
        bool changed = false;
        if (!goingScripthashes.isEmpty()) {
            // Outputs of the addresses that go are no more to be counted as the wallets'.
            bool outputsGone = false;
            for (const OurOutput& output : std::as_const(record.outputs)) {
                if (goingScripthashes.contains(output.scripthash)) {
                    owned_.remove(chain::OutPoint{record.txid, output.n});
                    outputsGone = true;
                }
            }
            if (outputsGone)
                record.outputs.removeIf([&goingScripthashes](const OurOutput& output) { return goingScripthashes.contains(output.scripthash); });
            changed = outputsGone;
            // What stays spends and pays as it did; an address that went no longer counts. Of what it
            // spent from one, its fee is no more known than a scan without that wallet would know it.
            QVector<TxEffect>& effects = effects_[position];
            if (std::any_of(effects.cbegin(), effects.cend(), [&goingAddresses](const TxEffect& effect) { return effect.spent > 0 && goingAddresses.contains(effect.address); })) {
                record.fee = -1;
                changed = true;
            }
            effects.removeIf([&goingAddresses](const TxEffect& effect) { return goingAddresses.contains(effect.address); });
        }
        if (dirtyTxs_.contains(position) || changed)
            dirty.insert(to);
        if (!effects_.at(position).isEmpty())
            ++lines_;
        if (standsPending(record))
            ++pending_;
        if (to != position) {
            txs_[to] = std::move(record);
            effects_[to] = std::move(effects_[position]);
        }
    }
    txs_.resize(next);
    effects_.resize(next);
    dirtyTxs_ = std::move(dirty);
    effectIndex_.clear();

    for (auto it = positions_.begin(); it != positions_.end(); ++it)
        it.value() = movedTo[size_t(it.value())];
    // In two walks: an erase can bring back round what one walk has been past, and it would be
    // numbered twice.
    for (auto it = spenders_.begin(); it != spenders_.end();) {
        if (movedTo[size_t(it.value())] < 0)
            it = spenders_.erase(it);
        else
            ++it;
    }
    for (auto it = spenders_.begin(); it != spenders_.end(); ++it)
        it.value() = movedTo[size_t(it.value())];
    for (int id = 0; id < addresses_.size(); ++id) {
        AddressEntry& entry = addresses_[id];
        if (goingAddresses.contains(id)) {
            addressIds_.remove(entry.scripthash);
            entry = AddressEntry();
            continue;
        }
        QVector<int> kept;
        kept.reserve(entry.txs.size());
        for (const int position : std::as_const(entry.txs)) {
            if (movedTo[size_t(position)] >= 0)
                kept.append(movedTo[size_t(position)]);
        }
        entry.txs = std::move(kept);
    }
    emit positionsMoved();
    emit notesChanged();
    emit changed();
}

void HistoryBook::forgetWallet(quint32 wallet)
{
    QSet<QString> going;
    for (const AddressEntry& entry : std::as_const(addresses_)) {
        if (entry.wallet == wallet)
            going.insert(entry.scripthash);
    }
    forget(going);
}

void HistoryBook::keepWallets(const QSet<quint32>& wallets, quint32 nextWallet)
{
    QSet<QString> removed;
    QSet<QString> going;
    for (const AddressEntry& entry : std::as_const(addresses_)) {
        if (entry.wallet != 0 && !wallets.contains(entry.wallet))
            (entry.wallet < nextWallet ? removed : going).insert(entry.scripthash);
    }
    // One this wallet.dat gave out and no longer has was removed, and that did not get as far as the
    // database: it finishes now.
    forget(removed);
    // Nobody removed the others: a wallet.dat from before they were added comes to the same. What only
    // this PC has stays, for when they are back; a removal is what takes that.
    const QHash<chain::Hash256, TxNote> notes = notes_;
    forget(going);
    if (notes_.size() != notes.size()) {
        notes_ = notes;
        emit notesChanged();
    }
}

void HistoryBook::addHeader(int height, const QByteArray& raw)
{
    headers_.insert(height, raw);
    dirtyHeaders_.insert(height, raw);
}

void HistoryBook::dropHeadersFrom(int height)
{
    headers_.erase(headers_.lowerBound(height), headers_.end());
    dirtyHeaders_.erase(dirtyHeaders_.lowerBound(height), dirtyHeaders_.end());
    dropHeadersFrom_ = dropHeadersFrom_ < 0 ? height : qMin(dropHeadersFrom_, height);
}

HistoryDelta HistoryBook::takeDelta()
{
    HistoryDelta delta;
    delta.txs.reserve(dirtyTxs_.size());
    QList<int> positions = dirtyTxs_.values();
    std::sort(positions.begin(), positions.end());
    for (const int position : std::as_const(positions)) {
        if (position < txs_.size() && !effects_.at(position).isEmpty())
            delta.txs.append(txs_.at(position));
    }
    dirtyTxs_.clear();
    delta.removedTxs = std::exchange(removedTxs_, {});
    // A note written since, an import say, is newer than the removal.
    for (const chain::Hash256& txid : std::as_const(delta.removedTxs)) {
        if (!notes_.contains(txid))
            delta.removedNotes.append(txid);
    }
    delta.addresses.reserve(dirtyAddresses_.size());
    for (const QString& scripthash : std::as_const(dirtyAddresses_)) {
        const auto status = statuses_.constFind(scripthash);
        const int id = findAddress(scripthash);
        if (status == statuses_.cend() || id < 0)
            continue;
        const AddressEntry& entry = addresses_.at(id);
        delta.addresses.append({scripthash, entry.wallet, entry.address, *status});
    }
    dirtyAddresses_.clear();
    delta.removedAddresses = removedAddresses_.values();
    removedAddresses_.clear();
    delta.headers = std::exchange(dirtyHeaders_, {});
    delta.dropHeadersFrom = std::exchange(dropHeadersFrom_, -1);
    return delta;
}

void HistoryBook::takeBack(const HistoryDelta& failed)
{
    for (const TxRecord& tx : failed.txs) {
        const int position = find(tx.txid);
        if (position >= 0)
            dirtyTxs_.insert(position);
    }
    for (const chain::Hash256& txid : failed.removedTxs) {
        if (find(txid) < 0)
            removedTxs_.append(txid);
    }
    for (const AddressHistory& address : failed.addresses) {
        if (statuses_.contains(address.scripthash))
            dirtyAddresses_.insert(address.scripthash);
    }
    for (const QString& scripthash : failed.removedAddresses) {
        if (!statuses_.contains(scripthash))
            removedAddresses_.insert(scripthash);
    }
    if (failed.dropHeadersFrom >= 0)
        dropHeadersFrom_ = dropHeadersFrom_ < 0 ? failed.dropHeadersFrom : qMin(dropHeadersFrom_, failed.dropHeadersFrom);
    for (auto it = failed.headers.cbegin(); it != failed.headers.cend(); ++it) {
        if (headers_.value(it.key()) == it.value())
            dirtyHeaders_.insert(it.key(), it.value());
    }
}

void HistoryBook::setNote(const chain::Hash256& txid, const TxNote& note)
{
    if (note.isEmpty())
        notes_.remove(txid);
    else
        notes_.insert(txid, note);
    emit notesChanged();
}

} // namespace wallet
