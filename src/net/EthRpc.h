#pragma once

#include <QDeadlineTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPair>
#include <QStringList>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QTimer;

namespace net {

class BudgetPool;

// Ethereum-style JSON-RPC over HTTPS, for one network. A batch is posted as one
// JSON array per kMaxBatch calls. The same request is retried on the next endpoint (each
// endpoint at most once per request) on a transport error, HTTP error, rate
// limit (429), a malformed reply, a reply in which any call came back without
// a result (rate limits and quotas reported in-band, disabled methods), or,
// when the request carries eth_chainId, an answer for a chain other than this
// network: that endpoint is then skipped until setEndpoints(). One the
// caller rejects, for answers only it can judge, is tried last for a while
// instead (see rejectEndpoint()). Every HTTP request is
// abandoned after a fixed deadline, however slowly its server answers, so
// every reply arrives. What up to kMaxBatch calls deliver always comes from
// one endpoint, which the reply names: the first that answered every call,
// else the most complete answer. GUI thread only.
class EthRpc : public QObject
{
    Q_OBJECT
public:
    // `endpoint`: the full URL the answer came from (empty if none was asked).
    // It may carry an API key: hand it back to rejectEndpoint(), never show it.
    using Reply = std::function<void(const QJsonValue& result, const QString& error, const QString& endpoint)>;
    // results[i] / errors[i] / endpoints[i] belong to calls[i]; errors[i] is
    // empty on success.
    using BatchReply = std::function<void(const QList<QJsonValue>& results, const QStringList& errors,
                                          const QStringList& endpoints)>;
    static constexpr int kMaxBatch = 250; // larger batches are split transparently

    // `name` is for the log; `chainId` what eth_chainId must answer; `builtIn` the
    // endpoints used when Settings lists none, `pace` the calls a second each of those
    // keeps answering. `budgets`, if given, paces requests per provider, shared with the
    // other networks' clients: a request goes to the first endpoint with room for it.
    EthRpc(const QString& name, quint64 chainId, const QStringList& builtIn, const QHash<QString, int>& pace,
           BudgetPool* budgets, QObject* parent = nullptr);

    // Empty = the built-in list. Starts over: requests still pending are
    // cancelled (their replies report an error), so no answer obtained under
    // the old list arrives after this.
    void setEndpoints(const QStringList& urls);
    QString currentEndpoint() const;
    // Ends what is pending or waiting its turn: nothing more goes out for it.
    void cancelAll();
    // http://127.0.0.1 too, for a test's server on this PC. Everything else stays https.
    void allowLocalHttpForTest() { localForTest_ = true; }

    void call(const QString& method, const QJsonArray& params, Reply reply);
    void batch(const QList<QPair<QString, QJsonArray>>& calls, BatchReply reply);

    // For an endpoint whose answer the caller found unusable although nothing
    // here could tell (an eth_call that shows no state of this network behind the
    // right chain id): requests move on from it, and for a while it is only tried
    // when no other endpoint is left, so one glitch never benches a healthy
    // endpoint for good. `url` as a reply named it; one not in the current
    // list is ignored.
    void rejectEndpoint(const QString& url);

    // True if `url` answered eth_chainId for a chain other than this network since
    // the list was set: its replies carry that error instead of answers.
    bool isOffChain(const QString& url) const;

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
    void finish(Chunk& chunk);
    void deliver(Chunk& chunk);
    int cancelPending(const QString& why);
    QString nextEndpoint(const Chunk& chunk) const;
    int takeBudget(const QString& url, int cost); // ms to wait; 0: it may go, and it counts
    QString moveCurrentPast(const QString& url);
    bool isDoubted(const QString& url) const;
    bool isAvoided(const QString& url) const; // doubted, or it just failed: last in line
    bool usableUrl(const QString& url) const;

    QString name_;
    quint64 chainId_ = 0;
    QStringList builtIn_;
    QHash<QString, int> pace_; // by provider (providerOf())
    BudgetPool* budgets_ = nullptr;
    QTimer* pumpTimer_ = nullptr; // a chunk waits for its host's budget
    QNetworkAccessManager* nam_ = nullptr;
    QStringList endpoints_;
    QStringList offChain_;                   // answered eth_chainId for another chain: skipped until setEndpoints()
    QHash<QString, QDeadlineTimer> doubted_; // rejected by the caller: tried last until the timer expires
    QHash<QString, QDeadlineTimer> cooling_; // failed an attempt just now: the same, for a shorter time
    bool localForTest_ = false;
    int current_ = 0;
    int nextId_ = 1;
    QList<std::shared_ptr<Chunk>> queue_; // chunks waiting for a free request slot
    QList<std::shared_ptr<Chunk>> wire_;  // chunks with a request on the wire (at most kMaxInFlight)
};

} // namespace net
