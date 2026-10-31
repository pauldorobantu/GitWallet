#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace net {

// USD prices from CoinGecko's public simple/price endpoint (bitcoin, ethereum,
// tether, usd-coin). refresh() is called by the controller on start and every
// few minutes; a failed refresh keeps the previous prices.
class PriceFeed : public QObject
{
    Q_OBJECT
public:
    explicit PriceFeed(QObject* parent = nullptr);

    void refresh();
    // Price of one unit of `symbol` ("BTC", "ETH", "USDT", "USDC") in USD; 0 if unknown.
    double usd(const QString& symbol) const;
    QDateTime updatedAt() const { return updatedAt_; }

signals:
    void updated();
    void failed(const QString& reason);

private:
    QNetworkAccessManager* nam_ = nullptr;
    QHash<QString, double> usd_;
    QDateTime updatedAt_;
    bool busy_ = false;
};

} // namespace net
