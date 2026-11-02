#pragma once

#include "wallet/Registry.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

#include <functional>

class QNetworkAccessManager;

namespace net {

// What a coin was worth at moments in the past, from DefiLlama: up to 500 moments a call
// (docs/ENDPOINTS.md). How close an answer is depends on how old: within minutes in recent years,
// the same day further back, and nothing before 2015. GUI thread only.
class PriceHistory : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMaxTimesPerCall = 500;
    // An answer further than this from the moment asked says nothing of it.
    static constexpr qint64 kMaxDistanceSeconds = 24 * 3600;

    using Answers = QHash<qint64, QPair<double, qint64>>; // asked -> price, when it held

    explicit PriceHistory(QObject* parent = nullptr);

    void useTestServer(const QString& base) { testBase_ = base; }
    // `done` runs once: what could be priced of `times`, and whether the source failed altogether.
    void ask(const wallet::AssetInfo& asset, const QList<qint64>& times, const std::function<void(const Answers&, bool failed)>& done);

    // What such an answer says of `times`. For tests too.
    static Answers read(const QByteArray& body, const QString& coin, const QList<qint64>& times);

private:
    QNetworkAccessManager* nam_ = nullptr;
    QString testBase_;
};

} // namespace net
