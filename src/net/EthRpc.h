#pragma once

#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPair>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;

namespace net {

// Public JSON-RPC endpoints that accepted batch requests and eth_call to
// Multicall3 in a probe on 2026-10-31 (no API key needed).
QStringList defaultEthEndpoints();

// Ethereum JSON-RPC over HTTPS. A batch is posted as one JSON array; on a
// transport error, HTTP error, rate limit (429) or a malformed reply the same
// request is retried on the next endpoint (each endpoint at most once per
// request) before the reply reports an error. GUI thread only.
class EthRpc : public QObject
{
    Q_OBJECT
public:
    using Reply = std::function<void(const QJsonValue& result, const QString& error)>;
    // results[i] / errors[i] belong to calls[i]; errors[i] is empty on success.
    using BatchReply = std::function<void(const QList<QJsonValue>& results, const QStringList& errors)>;
    static constexpr int kMaxBatch = 50; // larger batches are split transparently

    explicit EthRpc(QObject* parent = nullptr);

    void setEndpoints(const QStringList& urls); // empty = defaultEthEndpoints()
    QString currentEndpoint() const;

    void call(const QString& method, const QJsonArray& params, Reply reply);
    void batch(const QList<QPair<QString, QJsonArray>>& calls, BatchReply reply);

signals:
    void endpointChanged(const QString& url);
    void log(const QString& line);

private:
    // --- implementation (EthRpc.cpp may add private members) ---
    struct Batch; // one batch() call: the caller's reply and the answers collected so far
    struct Chunk; // up to kMaxBatch calls of a batch, posted as one HTTP request

    void pump();
    void send(Chunk& chunk, const QString& url);
    void handleReply(Chunk& chunk, const QString& url, const QString& failure, const QByteArray& body);
    void retry(Chunk& chunk, const QString& failedUrl, const QString& reason);
    void fail(Chunk& chunk, const QString& error);
    void finish(Chunk& chunk);
    QString nextEndpoint(const Chunk& chunk) const;

    QNetworkAccessManager* nam_ = nullptr;
    QStringList endpoints_;
    int current_ = 0;
    int nextId_ = 1;
    QList<std::function<void()>> queue_; // chunks waiting for a free request slot
    int inFlight_ = 0;                   // requests on the wire
};

} // namespace net
