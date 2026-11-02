#pragma once

#include "wallet/Registry.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace net {

// One asset's price, and where and when it comes from.
struct PriceQuote {
    wallet::Asset asset{};
    double usd = 0.0;
    QDateTime at;   // when the source says it holds, UTC
    QString source; // "DefiLlama"
};

// USD prices of everything the list of coins names. The first source is DefiLlama: it
// answers by chain and contract, so no symbol can be taken for another, and says how old
// each price is. What it does not answer, answers with an old price or is not sure of goes
// to CoinGecko, by its id, and then to Coinbase, by symbol: only the first keeps answering
// when asked often. Every price stays until a newer one replaces it, however often a
// refresh fails, and says when it is from. One source alone does not decide what makes no
// sense (a dollar coin far from a dollar, ten times what was held a moment ago): that takes
// a second source agreeing, which is also how a wrong price held gives way, and a first
// price is confirmed by the next source that can be asked. docs/ENDPOINTS.md has what was
// measured. GUI thread only.
class PriceFeed : public QObject
{
    Q_OBJECT
public:
    explicit PriceFeed(QObject* parent = nullptr);

    void refresh();
    bool isBusy() const { return busy_; }
    // Every source asked at `base`/<its name> instead. For tests.
    void useTestServer(const QString& base) { testBase_ = base; }
    // Prices the database kept from an earlier run, until the first answer replaces them.
    void restore(const QList<PriceQuote>& quotes);
    QList<PriceQuote> quotes() const;

    // Price of one unit of `asset` in USD; 0 if unknown.
    double usd(wallet::Asset asset) const;
    // The oldest time among the prices of `assets`, as local time goes; invalid if none is known.
    QDateTime oldest(const QList<wallet::Asset>& assets) const;
    QString sources(const QList<wallet::Asset>& assets) const; // "DefiLlama", or several joined
    bool lastRefreshFailed() const { return lastFailed_; } // some asset got no price from any source
    bool inDoubt(wallet::Asset asset) const { return doubted_.contains(asset); } // a source said otherwise, and none agreed

signals:
    void updated();
    void failed(const QString& reason);

private:
    enum class Source { DefiLlama, CoinGecko, Coinbase };

    void ask(Source source);
    void askNext(Source after, const QStringList& failures);
    void handleReply(Source source, const QByteArray& body, const QString& failure);
    bool readDefiLlama(const QByteArray& body);
    bool readCoinGecko(const QByteArray& body);
    bool readCoinbase(const QByteArray& body);
    QList<wallet::AssetInfo> missing() const; // what this refresh has not priced yet
    bool accept(const wallet::AssetInfo& asset, double usd, QDateTime at, const QString& source);
    void finish(const QStringList& failures);

    QNetworkAccessManager* nam_ = nullptr;
    QHash<wallet::Asset, PriceQuote> quotes_;
    QHash<wallet::Asset, bool> fresh_; // answered in this refresh
    QHash<wallet::Asset, PriceQuote> old_; // answered too, with a price that much behind: if none has better
    QHash<wallet::Asset, QList<PriceQuote>> doubted_; // what this refresh refused: another source may agree
    QSet<wallet::Asset> unconfirmed_;  // what is held came with nothing recent to compare: the next source is asked too
    QSet<wallet::Asset> askedLlama_;   // already on a DefiLlama URL in this refresh
    QStringList failures_;
    QString testBase_;
    bool busy_ = false;
    bool lastFailed_ = false;
};

} // namespace net
