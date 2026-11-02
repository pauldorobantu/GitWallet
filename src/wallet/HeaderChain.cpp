#include "wallet/HeaderChain.h"

#include "net/ElectrumClient.h"
#include "wallet/History.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include <algorithm>

namespace wallet {

namespace {

constexpr int kChunkHeaders = 2016;    // what every server measured gives a call
constexpr int kFirstForkDepth = 8;
// No real reorganisation has gone this deep in years: past this it is not one.
constexpr int kDeepestFork = 512;
constexpr int kRetryMs = 30'000;
// Below the checkpoint, on their way at once: each answer lets the next go.
constexpr int kMaxAsksOut = 24;
// Of what was refused, asked again per try.
constexpr int kAsksPerRetry = 200;

int gRetryMs = kRetryMs;

chain::Bytes bytesOf(const QByteArray& raw)
{
    return chain::Bytes(raw.cbegin(), raw.cend());
}

bool readHex(const QString& hex, chain::Bytes* out)
{
    if (hex.size() % 2 != 0)
        return false;
    const QByteArray raw = QByteArray::fromHex(hex.toLatin1());
    if (raw.size() != hex.size() / 2)
        return false;
    *out = bytesOf(raw);
    return true;
}

} // namespace

HeaderChain::HeaderChain(const ChainProofs& proofs, net::ElectrumClient* client, HistoryBook* book, QObject* parent)
    : QObject(parent), proofs_(proofs), client_(client), book_(book)
{
    retry_ = new QTimer(this);
    retry_->setSingleShot(true);
    retry_->setInterval(kRetryMs);
    connect(retry_, &QTimer::timeout, this, [this] {
        // A server that stays, because there is no other, gets another chance.
        refused_ = false;
        int moved = 0;
        for (auto it = refusedAsks_.begin(); it != refusedAsks_.end() && moved < kAsksPerRetry; ++moved) {
            wanted_.insert(*it);
            it = refusedAsks_.erase(it);
        }
        if (!refusedAsks_.isEmpty())
            retryLater();
        sendAsks();
        sync();
    });
    // What was on its way goes to the next server with the rest of what the client holds.
    connect(client_, &net::ElectrumClient::ready, this, [this] {
        refused_ = false;
        wanted_.unite(refusedAsks_);
        refusedAsks_.clear();
        sendAsks();
        sync();
    });
    connect(client_, &net::ElectrumClient::tipChanged, this, &HeaderChain::sync);
}

HeaderChain::~HeaderChain() = default;

void HeaderChain::setRetryForTest(int ms)
{
    gRetryMs = ms > 0 ? ms : kRetryMs;
}

void HeaderChain::start()
{
    running_ = true;
    ++generation_;
    below_.clear();
    above_.clear();
    wanted_.clear();
    asked_.clear();
    refusedAsks_.clear();
    askedAgain_.clear();
    syncing_ = false;
    refused_ = false;
    retry_->setInterval(gRetryMs);
    // The file has its own code per page; a header that no longer follows ends what is believed.
    const QMap<int, QByteArray>& kept = book_->headers();
    int next = proofs_.checkpointHeight + 1;
    bool holds = true;
    for (auto it = kept.cbegin(); it != kept.cend(); ++it) {
        chain::BlockHeader header;
        if (!chain::parseHeader(bytesOf(it.value()), &header))
            continue;
        if (it.key() <= proofs_.checkpointHeight) {
            below_.insert(it.key(), header);
        } else if (holds && it.key() == next && follows(header, next, [this](int height) { return aboveAt(height); })) {
            above_.append(header);
            ++next;
        } else {
            holds = false;
        }
    }
    if (!holds)
        book_->dropHeadersFrom(next);
    if (client_->isReady())
        sync();
}

void HeaderChain::stop()
{
    running_ = false;
    ++generation_;
    retry_->stop();
    syncing_ = false;
}

const chain::BlockHeader* HeaderChain::aboveAt(int height) const
{
    const int at = height - proofs_.checkpointHeight - 1;
    return at >= 0 && at < above_.size() ? &above_.at(at) : nullptr;
}

const chain::BlockHeader* HeaderChain::at(int height) const
{
    if (height > proofs_.checkpointHeight)
        return aboveAt(height);
    const auto found = below_.constFind(height);
    return found == below_.cend() ? nullptr : &found.value();
}

template <typename Prior>
bool HeaderChain::follows(const chain::BlockHeader& header, int height, Prior prior) const
{
    const int previousHeight = height - 1;
    const chain::BlockHeader* previous = prior(previousHeight);
    if (previousHeight == proofs_.checkpointHeight) {
        if (header.previous != proofs_.checkpointHash)
            return false;
    } else if (!previous || header.previous != previous->hash) {
        return false;
    }
    if (!chain::headerHasWork(header, proofs_.powLimit))
        return false;
    const uint32_t lastBits = previous ? previous->bits : proofs_.checkpointBits;
    if (proofs_.retargetBlocks > 0 && height % proofs_.retargetBlocks == 0) {
        const int firstHeight = height - proofs_.retargetBlocks;
        uint32_t firstTime = 0;
        if (firstHeight <= proofs_.checkpointHeight) {
            firstTime = proofs_.periodStart;
        } else if (const chain::BlockHeader* first = prior(firstHeight)) {
            firstTime = first->time;
        } else {
            return false;
        }
        const uint32_t lastTime = previous ? previous->time : proofs_.checkpointTime;
        return header.bits == chain::nextBits(lastBits, firstTime, lastTime, proofs_.powLimit, proofs_.targetSeconds);
    }
    return header.bits == lastBits;
}

void HeaderChain::ask(int height)
{
    if (height > proofs_.checkpointHeight) {
        sync();
        return;
    }
    if (below_.contains(height) || asked_.contains(height) || wanted_.contains(height) || refusedAsks_.contains(height))
        return;
    wanted_.insert(height);
    sendAsks();
}

void HeaderChain::askAgain(int height)
{
    const auto kept = below_.constFind(height);
    if (kept == below_.cend())
        return;
    askedAgain_.insert(height, kept->hash);
    below_.erase(kept);
    ask(height);
}

void HeaderChain::sendAsks()
{
    if (!running_ || refused_)
        return;
    while (asked_.size() < kMaxAsksOut && !wanted_.isEmpty()) {
        const int height = *wanted_.cbegin();
        wanted_.erase(wanted_.cbegin());
        asked_.insert(height);
        client_->request(QStringLiteral("blockchain.block.header"), QJsonArray{height, proofs_.checkpointHeight},
                         [this, generation = generation_, height](const QJsonValue& result, const QString& error) {
                             onBranch(generation, height, result, error);
                         });
    }
}

void HeaderChain::onBranch(quint64 generation, int height, const QJsonValue& result, const QString& error)
{
    if (generation != generation_)
        return;
    asked_.remove(height);
    sendAsks();
    if (!error.isEmpty()) {
        refusedAsks_.insert(height); // asked again in a while, or of the next server
        noteRefused();
        return;
    }
    const QJsonObject answer = result.toObject();
    chain::Bytes raw;
    chain::BlockHeader header;
    std::vector<chain::Hash256> branch;
    bool whole = readHex(answer.value(QLatin1String("header")).toString(), &raw) && chain::parseHeader(raw, &header);
    const QJsonArray steps = answer.value(QLatin1String("branch")).toArray();
    whole = whole && steps.size() <= 32;
    for (const QJsonValue& step : steps) {
        chain::Hash256 hash{};
        whole = whole && chain::hashFromHex(step.toString().toStdString(), &hash);
        branch.push_back(hash);
    }
    if (!whole || chain::merkleRootOf(header.hash, branch, quint64(height)) != proofs_.headersRoot) {
        wanted_.insert(height);
        misbehave(QStringLiteral("a block header does not lead to the checkpoint"));
        return;
    }
    below_.insert(height, header);
    book_->addHeader(height, QByteArray(reinterpret_cast<const char*>(raw.data()), qsizetype(raw.size())));
    const auto kept = askedAgain_.constFind(height);
    if (kept != askedAgain_.cend()) {
        const bool moved = *kept != header.hash;
        askedAgain_.erase(kept);
        if (moved)
            emit headerMoved(height);
    }
    emit headerReady(height);
}

void HeaderChain::sync()
{
    if (!running_ || refused_ || !client_->isReady())
        return;
    if (syncing_) {
        resync_ = true;
        return;
    }
    const int from = tip() + 1;
    if (from > client_->tipHeight())
        return; // nothing new on this server's word
    syncing_ = true;
    resync_ = false;
    client_->request(QStringLiteral("blockchain.block.headers"), QJsonArray{from, kChunkHeaders},
                     [this, generation = generation_, from](const QJsonValue& result, const QString& error) {
                         onChunk(generation, from, result, error);
                     });
}

bool HeaderChain::parseChunk(const QJsonValue& result, Candidate* out) const
{
    const QJsonObject answer = result.toObject();
    const int count = answer.value(QLatin1String("count")).toInt(-1);
    const QString hex = answer.value(QLatin1String("hex")).toString();
    chain::Bytes raw;
    if (count < 0 || count > kChunkHeaders || hex.size() != qsizetype(count) * 160 || !readHex(hex, &raw))
        return false;
    out->reserve(count);
    for (int i = 0; i < count; ++i) {
        chain::BlockHeader header;
        const chain::Bytes one(raw.begin() + 80 * i, raw.begin() + 80 * (i + 1));
        if (!chain::parseHeader(one, &header))
            return false;
        out->append(header);
    }
    return true;
}

void HeaderChain::appendAbove(const Candidate& chunk)
{
    for (const chain::BlockHeader& header : chunk) {
        above_.append(header);
        const chain::Bytes raw = chain::serializeHeader(header);
        book_->addHeader(tip(), QByteArray(reinterpret_cast<const char*>(raw.data()), qsizetype(raw.size())));
    }
}

void HeaderChain::onChunk(quint64 generation, int from, const QJsonValue& result, const QString& error)
{
    if (generation != generation_)
        return;
    syncing_ = false;
    if (from != tip() + 1) {
        sync(); // this answers a question from before the chain moved: asked afresh
        return;
    }
    if (!error.isEmpty()) {
        noteRefused();
        return;
    }
    Candidate chunk;
    if (!parseChunk(result, &chunk)) {
        misbehave(QStringLiteral("its block headers cannot be read"));
        return;
    }
    if (chunk.isEmpty()) {
        // Nothing from there on, from one that has just said it has more.
        if (from <= client_->tipHeight())
            noteRefused();
        return;
    }
    // Not following the last one held may be blocks that gave way to others.
    const chain::Hash256 lastHash = above_.isEmpty() ? proofs_.checkpointHash : above_.last().hash;
    if (chunk.first().previous != lastHash) {
        if (above_.isEmpty()) {
            misbehave(QStringLiteral("its block headers do not follow the checkpoint"));
            return;
        }
        forkDepth_ = kFirstForkDepth;
        lookForFork(generation);
        return;
    }
    for (int i = 0; i < chunk.size(); ++i) {
        const auto prior = [&](int height) -> const chain::BlockHeader* {
            const int inChunk = height - from;
            return inChunk >= 0 ? (inChunk < i ? &chunk.at(inChunk) : nullptr) : aboveAt(height);
        };
        if (!follows(chunk.at(i), from + i, prior)) {
            misbehave(QStringLiteral("its block headers do not hold: one lacks the work or the difficulty it should have"));
            return;
        }
    }
    appendAbove(chunk);
    forkDepth_ = 0;
    emit tipMoved();
    if (tip() < client_->tipHeight() || resync_)
        sync();
}

void HeaderChain::lookForFork(quint64 generation)
{
    const int from = qMax(tip() + 1 - forkDepth_, proofs_.checkpointHeight + 1);
    syncing_ = true;
    client_->request(QStringLiteral("blockchain.block.headers"), QJsonArray{from, kChunkHeaders},
                     [this, generation, from](const QJsonValue& result, const QString& error) {
                         onForkChunk(generation, from, result, error);
                     });
}

void HeaderChain::onForkChunk(quint64 generation, int from, const QJsonValue& result, const QString& error)
{
    if (generation != generation_)
        return;
    syncing_ = false;
    if (!error.isEmpty()) {
        noteRefused();
        return;
    }
    Candidate chunk;
    if (!parseChunk(result, &chunk) || chunk.isEmpty()) {
        misbehave(QStringLiteral("its block headers cannot be read"));
        return;
    }
    // Where it leaves what is held. It has to come back to it, or it is further down still.
    const auto followsHeld = [&] {
        const chain::Hash256 before = from - 1 == proofs_.checkpointHeight ? proofs_.checkpointHash : aboveAt(from - 1)->hash;
        return chunk.first().previous == before;
    };
    if (!followsHeld()) {
        if (from == proofs_.checkpointHeight + 1 || forkDepth_ >= kDeepestFork) {
            misbehave(QStringLiteral("its block headers leave the chain too far down"));
            return;
        }
        forkDepth_ *= 2;
        lookForFork(generation);
        return;
    }
    int same = 0;
    while (same < chunk.size() && aboveAt(from + same) && aboveAt(from + same)->hash == chunk.at(same).hash)
        ++same;
    const int forkAt = from + same;
    for (int i = same; i < chunk.size(); ++i) {
        const auto prior = [&](int height) -> const chain::BlockHeader* {
            const int inChunk = height - from;
            return inChunk >= 0 ? (inChunk < i ? &chunk.at(inChunk) : nullptr) : aboveAt(height);
        };
        if (!follows(chunk.at(i), from + i, prior)) {
            misbehave(QStringLiteral("its block headers do not hold: one lacks the work or the difficulty it should have"));
            return;
        }
    }
    // Only for more than was held does a chain give way, and work is what makes one more.
    if (from + int(chunk.size()) - 1 <= tip()) {
        if (same == chunk.size())
            return; // what it has is what is held: its tip was behind
        retryLater();
        return;
    }
    above_.resize(forkAt - proofs_.checkpointHeight - 1);
    book_->dropHeadersFrom(forkAt);
    appendAbove(chunk.mid(same));
    forkDepth_ = 0;
    emit reorged(forkAt);
    emit tipMoved();
    if (tip() < client_->tipHeight() || resync_)
        sync();
}

void HeaderChain::retryLater()
{
    if (running_ && !retry_->isActive())
        retry_->start();
}

void HeaderChain::noteRefused()
{
    if (!running_ || retry_->isActive())
        return; // the rest of the same try
    retry_->start();
    emit headersRefused();
}

void HeaderChain::misbehave(const QString& why)
{
    refused_ = true;
    retryLater();
    emit misbehaved(why);
}

} // namespace wallet
