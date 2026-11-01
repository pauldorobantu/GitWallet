#include "net/PriceFeed.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <cmath>
#include <memory>

namespace net {

namespace {

const char* const kPriceUrl =
    "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,ethereum,tether,usd-coin&vs_currencies=usd";
constexpr int kTransferTimeoutMs = 15000;
// The answer is about 130 bytes. The reply is buffered whole and the transfer
// timeout only fires when no data moves, so a server that keeps streaming is
// cut off here instead of filling memory.
constexpr qint64 kMaxReplyBytes = 64 * 1024;

struct Coin {
    const char* coingeckoId;
    const char* symbol;
};
constexpr Coin kCoins[] = {
    {"bitcoin", "BTC"},
    {"ethereum", "ETH"},
    {"tether", "USDT"},
    {"usd-coin", "USDC"},
};

// Why the transfer failed; empty when it delivered an HTTP 200 body.
// `oversized`: refresh() abandoned a body longer than kMaxReplyBytes.
QString failureOf(QNetworkReply* reply, bool oversized)
{
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid() && status.toInt() != 200) {
        // Rate limiting (429) is the usual case on the keyless API.
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

} // namespace

PriceFeed::PriceFeed(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this))
{
}

void PriceFeed::refresh()
{
    if (busy_) return; // the answer already on its way will do
    busy_ = true;

    QNetworkRequest request{QUrl(QString::fromLatin1(kPriceUrl))};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("GitWallet/0.1"));
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
    connect(reply, &QNetworkReply::readyRead, this, [reply, oversized] {
        if (!*oversized && reply->bytesAvailable() > kMaxReplyBytes) {
            *oversized = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, oversized] {
        reply->deleteLater();
        busy_ = false;

        const QString failure = failureOf(reply, *oversized || reply->bytesAvailable() > kMaxReplyBytes);
        if (!failure.isEmpty()) {
            emit failed(QStringLiteral("CoinGecko: ") + failure);
            return;
        }
        QJsonParseError parse;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
            emit failed(QStringLiteral("CoinGecko: malformed reply"));
            return;
        }

        // {"bitcoin":{"usd":67000.5}, ...}. Only a positive finite number is a
        // price; a coin missing from the reply keeps its previous price.
        const QJsonObject root = doc.object();
        QHash<QString, double> fresh;
        for (const Coin& coin : kCoins) {
            const QJsonValue usd = root.value(QLatin1String(coin.coingeckoId)).toObject().value(QStringLiteral("usd"));
            const double value = usd.isDouble() ? usd.toDouble() : 0.0;
            if (std::isfinite(value) && value > 0.0) fresh.insert(QString::fromLatin1(coin.symbol), value);
        }
        if (fresh.isEmpty()) {
            emit failed(QStringLiteral("CoinGecko: no usable price in the reply"));
            return;
        }
        for (auto it = fresh.cbegin(); it != fresh.cend(); ++it)
            usd_.insert(it.key(), it.value());
        updatedAt_ = QDateTime::currentDateTimeUtc();
        emit updated();
    });
}

double PriceFeed::usd(const QString& symbol) const
{
    return usd_.value(symbol.toUpper(), 0.0);
}

} // namespace net
