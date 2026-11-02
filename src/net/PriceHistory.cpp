#include "net/PriceHistory.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <vector>

namespace net {

namespace {

constexpr int kRequestTimeoutMs = 30'000;
// A price the source itself does not stand behind is no price.
constexpr double kLeastConfidence = 0.5;

} // namespace

PriceHistory::PriceHistory(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
    nam_->setTransferTimeout(kRequestTimeoutMs);
}

void PriceHistory::ask(const wallet::AssetInfo& asset, const QList<qint64>& times, const std::function<void(const Answers&, bool)>& done)
{
    QStringList numbers;
    numbers.reserve(times.size());
    for (const qint64 time : times)
        numbers.append(QString::number(time));
    const QString coin = asset.defillamaId;
    if (coin.isEmpty()) {
        done({}, false);
        return;
    }
    QUrl url(testBase_.isEmpty() ? QStringLiteral("https://coins.llama.fi/batchHistorical") : testBase_ + QStringLiteral("/batchHistorical"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("coins"), QStringLiteral("{\"%1\":[%2]}").arg(coin, numbers.join(QLatin1Char(','))));
    query.addQueryItem(QStringLiteral("searchWidth"), QStringLiteral("24h"));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("GitWallet"));
    QNetworkReply* reply = nam_->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, coin, times, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done({}, true);
            return;
        }
        // Any page a server in front of it puts up has a 200 on it too.
        const QByteArray body = reply->readAll();
        if (!QJsonDocument::fromJson(body).object().value(QLatin1String("coins")).isObject()) {
            done({}, true);
            return;
        }
        done(read(body, coin, times), false);
    });
}

PriceHistory::Answers PriceHistory::read(const QByteArray& body, const QString& coin, const QList<qint64>& times)
{
    struct Point {
        qint64 at;
        double usd;
    };
    std::vector<Point> points;
    const QJsonArray prices = QJsonDocument::fromJson(body).object().value(QLatin1String("coins")).toObject().value(coin).toObject().value(QLatin1String("prices")).toArray();
    points.reserve(size_t(prices.size()));
    for (const QJsonValue& value : prices) {
        const QJsonObject one = value.toObject();
        const double usd = one.value(QLatin1String("price")).toDouble(-1.0);
        const qint64 at = qint64(one.value(QLatin1String("timestamp")).toDouble(0.0));
        const double confidence = one.value(QLatin1String("confidence")).toDouble(1.0);
        if (usd > 0.0 && std::isfinite(usd) && at > 0 && confidence >= kLeastConfidence)
            points.push_back({at, usd});
    }
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.at < b.at; });

    // A moment it has nothing for is simply not in the list: each goes by the nearest it does have.
    Answers out;
    for (const qint64 time : times) {
        const auto after = std::lower_bound(points.begin(), points.end(), time, [](const Point& p, qint64 t) { return p.at < t; });
        const Point* best = nullptr;
        if (after != points.end())
            best = &*after;
        if (after != points.begin()) {
            const Point* before = &*(after - 1);
            if (!best || time - before->at < best->at - time)
                best = before;
        }
        if (best && std::llabs(best->at - time) <= kMaxDistanceSeconds)
            out.insert(time, {best->usd, best->at});
    }
    return out;
}

} // namespace net
