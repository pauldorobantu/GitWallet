#include "net/PriceFeed.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QTimeZone>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <memory>

namespace net {

namespace {

constexpr int kTransferTimeoutMs = 15000;
// However slowly its data keeps arriving: that restarts the transfer timeout with every
// byte, and one source's reply would hold back every later refresh.
constexpr int kRequestDeadlineMs = 30000;
// The longest answer, Coinbase's whole list, is about 17 KB. The reply is buffered
// whole and the transfer timeout only fires when no data moves, so a server that
// keeps streaming is cut off here instead of filling memory.
constexpr qint64 kMaxReplyBytes = 256 * 1024;
// DefiLlama refuses a URL over 9,000 characters without a paid plan: about 150 assets.
constexpr qsizetype kMaxUrlChars = 8500;
// A price DefiLlama itself is not sure of.
constexpr double kMinConfidence = 0.5;
// A price older than this is not an answer as long as another source may have a newer one.
constexpr qint64 kOldPriceSeconds = 3600;
// A coin that holds a dollar has been further off than people like, not this far: one
// source saying so needs another to agree.
constexpr double kMinDollarPrice = 0.5;
constexpr double kMaxDollarPrice = 2.0;
// Little moves this much while a held price is this young. The same: a second source decides.
// Past that it is believed: a bad price cannot stand in the way of a real one for long.
constexpr double kMaxJump = 10.0;
constexpr qint64 kJumpWindowSeconds = 3600;
// Two sources agree within this much.
constexpr double kAgreement = 0.05;
// No coin is worth this: a rate of nearly nothing, inverted.
constexpr double kMaxPrice = 1e12;

QString sourceName(int source)
{
    switch (source) {
    case 0: return QStringLiteral("DefiLlama");
    case 1: return QStringLiteral("CoinGecko");
    default: return QStringLiteral("Coinbase");
    }
}

// Why the transfer failed; empty when it delivered an HTTP 200 body.
// `oversized`: refresh() abandoned a body longer than kMaxReplyBytes.
QString failureOf(QNetworkReply* reply, bool oversized)
{
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid() && status.toInt() != 200) {
        // Rate limiting (429) is routine on the keyless APIs.
        const QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        return reason.isEmpty() ? QStringLiteral("HTTP %1").arg(status.toInt())
                                : QStringLiteral("HTTP %1 %2").arg(status.toInt()).arg(reason.left(100));
    }
    // Only now: an HTTP error status says more than the size of its error page,
    // and our own abort would otherwise read as a timeout below.
    if (oversized) return QStringLiteral("reply too large");
    switch (reply->error()) {
    case QNetworkReply::NoError:
        return status.isValid() ? QString() : QStringLiteral("no HTTP status");
    case QNetworkReply::OperationCanceledError: // how the transfer timeout's abort is reported
    case QNetworkReply::TimeoutError:
        return QStringLiteral("timed out");
    default:
        return reply->errorString().left(200);
    }
}

bool usable(double usd)
{
    return std::isfinite(usd) && usd > 0.0 && usd < kMaxPrice;
}

} // namespace

PriceFeed::PriceFeed(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this))
{
}

void PriceFeed::refresh()
{
    if (busy_) return; // the answer already on its way will do
    busy_ = true;
    fresh_.clear();
    old_.clear();
    doubted_.clear();
    failures_.clear();
    askedLlama_.clear();
    ask(Source::DefiLlama);
}

void PriceFeed::restore(const QList<PriceQuote>& quotes)
{
    bool any = false;
    for (const PriceQuote& quote : quotes) {
        if (!usable(quote.usd) || !quote.at.isValid())
            continue;
        // A clock that was ahead must not freeze a price for as long as it was.
        PriceQuote held = quote;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (held.at > now)
            held.at = now;
        const auto known = quotes_.constFind(held.asset);
        if (known == quotes_.cend() || known->at < held.at) {
            quotes_.insert(held.asset, held);
            // What was in doubt when it was saved is not known now: the sources have their say.
            if (!wallet::assetInfo(held.asset).dollar)
                unconfirmed_.insert(held.asset);
            any = true;
        }
    }
    if (any) emit updated();
}

QList<PriceQuote> PriceFeed::quotes() const
{
    return quotes_.values();
}

double PriceFeed::usd(wallet::Asset asset) const
{
    return quotes_.value(asset).usd;
}

QDateTime PriceFeed::oldest(const QList<wallet::Asset>& assets) const
{
    QDateTime out;
    for (const wallet::Asset asset : assets) {
        const auto quote = quotes_.constFind(asset);
        if (quote != quotes_.cend() && (!out.isValid() || quote->at < out))
            out = quote->at;
    }
    return out;
}

QString PriceFeed::sources(const QList<wallet::Asset>& assets) const
{
    QStringList names;
    for (const wallet::Asset asset : assets) {
        const auto quote = quotes_.constFind(asset);
        if (quote != quotes_.cend() && !names.contains(quote->source))
            names.append(quote->source);
    }
    return names.join(QStringLiteral(", "));
}

QList<wallet::AssetInfo> PriceFeed::missing() const
{
    QList<wallet::AssetInfo> out;
    for (const wallet::AssetInfo& info : wallet::assets()) {
        if (!fresh_.value(info.asset) || unconfirmed_.contains(info.asset))
            out.append(info);
    }
    return out;
}

void PriceFeed::ask(Source source)
{
    const QList<wallet::AssetInfo> wanted = missing();
    QStringList ids;
    QString url;
    switch (source) {
    case Source::DefiLlama:
        url = QStringLiteral("https://coins.llama.fi/prices/current/");
        for (const wallet::AssetInfo& info : wanted) {
            if (info.defillamaId.isEmpty() || askedLlama_.contains(info.asset))
                continue;
            if (url.size() + info.defillamaId.size() + 1 >= kMaxUrlChars)
                break; // the next call takes the rest
            if (!ids.isEmpty())
                url += QLatin1Char(',');
            url += info.defillamaId;
            ids.append(info.defillamaId);
            askedLlama_.insert(info.asset);
        }
        break;
    case Source::CoinGecko:
        for (const wallet::AssetInfo& info : wanted) {
            if (!info.coingeckoId.isEmpty())
                ids.append(info.coingeckoId);
        }
        url = QStringLiteral("https://api.coingecko.com/api/v3/simple/price?ids=%1&vs_currencies=usd&include_last_updated_at=true")
                  .arg(ids.join(QLatin1Char(',')));
        break;
    case Source::Coinbase:
        for (const wallet::AssetInfo& info : wanted) {
            if (!info.coinbaseSymbol.isEmpty())
                ids.append(info.coinbaseSymbol);
        }
        url = QStringLiteral("https://api.coinbase.com/v2/exchange-rates?currency=USD");
        break;
    }
    if (ids.isEmpty()) {
        askNext(source, {});
        return;
    }
    if (!testBase_.isEmpty())
        url = testBase_ + QLatin1Char('/') + sourceName(int(source)).toLower();

    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("GitWallet/0.2"));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(kTransferTimeoutMs);

    // The reply is a child of nam_ and `this` is the handlers' context, so
    // neither runs after PriceFeed is destroyed.
    QNetworkReply* reply = nam_->get(request);

    // Nothing is read before the reply finishes, so bytesAvailable() is the
    // body received so far (already inflated when it came compressed).
    // abort() emits finished() before it returns and drops the buffered body,
    // hence the flag. finished() checks the size again because Qt can end a
    // reply without a readyRead for its last data.
    auto oversized = std::make_shared<bool>(false);
    // The reply as its context: the deadline ends with it. Between finished() and the
    // deferred delete it may still fire, hence the check. An abort reads as timed out.
    QTimer::singleShot(kRequestDeadlineMs, reply, [reply] {
        if (!reply->isFinished())
            reply->abort();
    });
    connect(reply, &QNetworkReply::readyRead, this, [reply, oversized] {
        if (!*oversized && reply->bytesAvailable() > kMaxReplyBytes) {
            *oversized = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, oversized, source] {
        reply->deleteLater();
        const QString failure = failureOf(reply, *oversized || reply->bytesAvailable() > kMaxReplyBytes);
        handleReply(source, failure.isEmpty() ? reply->readAll() : QByteArray(), failure);
    });
}

void PriceFeed::handleReply(Source source, const QByteArray& body, const QString& failure)
{
    const QString name = sourceName(int(source));
    QStringList failures;
    if (!failure.isEmpty()) {
        failures << name + QStringLiteral(": ") + failure;
    } else {
        bool read = false;
        switch (source) {
        case Source::DefiLlama: read = readDefiLlama(body); break;
        case Source::CoinGecko: read = readCoinGecko(body); break;
        case Source::Coinbase: read = readCoinbase(body); break;
        }
        if (!read)
            failures << name + QStringLiteral(": no usable price in the reply");
    }
    askNext(source, failures);
}

void PriceFeed::askNext(Source after, const QStringList& failures)
{
    failures_ += failures;
    const QList<wallet::AssetInfo> wanted = missing();
    if (wanted.isEmpty() || after == Source::Coinbase) {
        finish(failures_);
        return;
    }
    if (after == Source::DefiLlama && failures.isEmpty()) {
        const bool more = std::any_of(wanted.cbegin(), wanted.cend(), [this](const wallet::AssetInfo& info) {
            return !info.defillamaId.isEmpty() && !askedLlama_.contains(info.asset);
        });
        if (more) {
            ask(Source::DefiLlama);
            return;
        }
    }
    ask(after == Source::DefiLlama ? Source::CoinGecko : Source::Coinbase);
}

void PriceFeed::finish(const QStringList& failures)
{
    busy_ = false;
    bool any = !fresh_.isEmpty();
    // No source had a recent price for these: an old one, if it is newer than what is held.
    for (auto it = old_.cbegin(); it != old_.cend(); ++it) {
        const auto held = quotes_.constFind(it.key());
        if (fresh_.value(it.key()) || (held != quotes_.cend() && held->at >= it->at))
            continue;
        quotes_.insert(it.key(), *it);
        any = true;
    }
    const QList<wallet::AssetInfo>& all = wallet::assets();
    const bool unanswered = std::any_of(all.cbegin(), all.cend(), [this](const wallet::AssetInfo& info) { return !fresh_.value(info.asset); });
    lastFailed_ = unanswered && !failures.isEmpty();
    // A coin missing from every reply keeps its previous price.
    if (any) emit updated();
    if (!failures.isEmpty()) emit failed(failures.join(QStringLiteral("; ")));
}

bool PriceFeed::accept(const wallet::AssetInfo& asset, double usd, QDateTime at, const QString& source)
{
    if (!usable(usd) || !at.isValid()) return false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (at > now) at = now; // a clock ahead of ours must not make it look forever new
    PriceQuote quote;
    quote.asset = asset.asset;
    quote.usd = usd;
    quote.at = at;
    quote.source = source;

    const auto held = quotes_.constFind(asset.asset);
    const bool recent = held != quotes_.cend() && held->at <= now && held->at.secsTo(now) < kJumpWindowSeconds;
    const bool jump = recent && (usd > held->usd * kMaxJump || usd * kMaxJump < held->usd);
    // A later source's word on a price nothing has confirmed: it confirms only by agreeing.
    const bool disputes = recent && unconfirmed_.contains(asset.asset) && held->source != source
                          && std::abs(held->usd - usd) > kAgreement * usd;
    if (jump || disputes || (asset.dollar && (usd < kMinDollarPrice || usd > kMaxDollarPrice))) {
        // One source against what is known. Two that agree have it, whichever is wrong: a
        // real crash, a real depeg, or a wrong price being held.
        QList<PriceQuote>& doubted = doubted_[asset.asset];
        const bool agreed = std::any_of(doubted.cbegin(), doubted.cend(), [&quote](const PriceQuote& other) {
            return other.source != quote.source && std::abs(other.usd - quote.usd) <= kAgreement * quote.usd;
        });
        if (!agreed) {
            doubted.append(quote);
            return false;
        }
        doubted_.remove(asset.asset);
        unconfirmed_.remove(asset.asset);
        quotes_.insert(asset.asset, quote);
        fresh_.insert(asset.asset, true);
        return true;
    }

    if (at.secsTo(now) > kOldPriceSeconds) {
        const auto known = old_.constFind(asset.asset);
        if (known == old_.cend() || known->at < at)
            old_.insert(asset.asset, quote);
        return false; // the next source may have a newer one
    }
    // Another source's price of a moment ago confirms it. With nothing recent to compare it
    // with, the next source is asked as well.
    if (recent && held->source != source)
        unconfirmed_.remove(asset.asset);
    else if (!recent && !asset.dollar)
        unconfirmed_.insert(asset.asset);
    // An answer, and what is held may still be newer: a source that was asked earlier. One
    // dated ahead of the clock is older than any answer.
    if (held == quotes_.cend() || held->at <= at || held->at > now)
        quotes_.insert(asset.asset, quote);
    fresh_.insert(asset.asset, true);
    return true;
}

// {"coins":{"coingecko:bitcoin":{"price":78380.5,"symbol":"BTC","timestamp":1793587557,
// "confidence":0.99},...}}. A price it holds no opinion on, or a low opinion, is not one.
bool PriceFeed::readDefiLlama(const QByteArray& body)
{
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject coins = doc.object().value(QStringLiteral("coins")).toObject();
    bool any = false;
    for (const wallet::AssetInfo& info : wallet::assets()) {
        if (info.defillamaId.isEmpty() || (fresh_.value(info.asset) && !unconfirmed_.contains(info.asset))) continue;
        const QJsonObject coin = coins.value(info.defillamaId).toObject();
        const double usd = coin.value(QStringLiteral("price")).toDouble();
        const qint64 at = coin.value(QStringLiteral("timestamp")).toInteger();
        const double confidence = coin.value(QStringLiteral("confidence")).toDouble(-1.0);
        const QString symbol = coin.value(QStringLiteral("symbol")).toString();
        if (!usable(usd) || at <= 0 || confidence < kMinConfidence) continue;
        // It answers by chain and contract, and says what it took that to be.
        if (!symbol.isEmpty() && symbol.compare(info.symbol, Qt::CaseInsensitive) != 0) continue;
        any = accept(info, usd, QDateTime::fromSecsSinceEpoch(at, QTimeZone::UTC), sourceName(int(Source::DefiLlama))) || any;
    }
    return any;
}

// {"bitcoin":{"usd":78380,"last_updated_at":1793587557},...}
bool PriceFeed::readCoinGecko(const QByteArray& body)
{
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject root = doc.object();
    bool any = false;
    for (const wallet::AssetInfo& info : wallet::assets()) {
        if (info.coingeckoId.isEmpty() || (fresh_.value(info.asset) && !unconfirmed_.contains(info.asset))) continue;
        const QJsonObject coin = root.value(info.coingeckoId).toObject();
        const double usd = coin.value(QStringLiteral("usd")).toDouble();
        const qint64 at = coin.value(QStringLiteral("last_updated_at")).toInteger();
        // Without its time, an old price would read as a new one.
        if (!usable(usd) || at <= 0) continue;
        any = accept(info, usd, QDateTime::fromSecsSinceEpoch(at, QTimeZone::UTC), sourceName(int(Source::CoinGecko))) || any;
    }
    return any;
}

// {"data":{"currency":"USD","rates":{"BTC":"0.0000127647","ETH":"0.000436",...}}}: units
// of each per dollar, so the price is its inverse. Now, as far as it says.
bool PriceFeed::readCoinbase(const QByteArray& body)
{
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject rates = doc.object().value(QStringLiteral("data")).toObject().value(QStringLiteral("rates")).toObject();
    bool any = false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const wallet::AssetInfo& info : wallet::assets()) {
        if (info.coinbaseSymbol.isEmpty() || (fresh_.value(info.asset) && !unconfirmed_.contains(info.asset))) continue;
        bool ok = false;
        const double rate = rates.value(info.coinbaseSymbol).toString().toDouble(&ok);
        if (!ok || !usable(rate)) continue;
        any = accept(info, 1.0 / rate, now, sourceName(int(Source::Coinbase))) || any;
    }
    return any;
}

} // namespace net
