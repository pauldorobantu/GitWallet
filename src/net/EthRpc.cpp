#include "net/EthRpc.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <limits>
#include <memory>
#include <utility>

namespace net {

namespace {

constexpr int kTransferTimeoutMs = 20000; // a request that moves no data for this long is aborted
// Requests on the wire at once; further chunks wait here. A big refresh then
// neither trips a public endpoint's rate limit nor queues inside Qt's per-host
// connection limit, where its transfer timeout would already be running.
constexpr int kMaxInFlight = 4;
constexpr qsizetype kMaxServerText = 200; // server-supplied text is clipped for logs and UI
// A reply is buffered whole until it finishes, and the transfer timeout only
// fires when no data moves, so without a cap a server that keeps streaming
// could fill memory. The largest legitimate answer, an aggregate3 of 150
// sub-calls, is about 50 KB; even kMaxBatch of those stay well below this.
constexpr qint64 kMaxReplyBytes = 8 * 1024 * 1024;

QString clip(const QString& text)
{
    return text.size() > kMaxServerText ? text.left(kMaxServerText) + QStringLiteral("...") : text;
}

// Host name only: a configured endpoint may carry an API key in its path or
// query, and that must not end up in a log.
QString hostOf(const QString& url)
{
    const QString host = QUrl(url).host();
    return host.isEmpty() ? QStringLiteral("(invalid endpoint)") : host;
}

bool isHttpsUrl(const QString& url)
{
    const QUrl u(url, QUrl::StrictMode);
    return u.isValid() && u.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0
           && !u.host().isEmpty();
}

// Why the transfer failed; empty when it delivered an HTTP 200 body.
// `oversized`: send() abandoned a body longer than kMaxReplyBytes.
QString failureOf(QNetworkReply* reply, bool oversized)
{
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid() && status.toInt() != 200) {
        // Built here rather than taken from errorString(), which quotes the
        // full URL (and with it any API key).
        const QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        return reason.isEmpty() ? QStringLiteral("HTTP %1").arg(status.toInt())
                                : QStringLiteral("HTTP %1 %2").arg(status.toInt()).arg(clip(reason));
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
        return clip(reply->errorString());
    }
}

// The text of a JSON-RPC "error" member: its message plus its code.
QString rpcErrorText(const QJsonValue& error)
{
    if (error.isObject()) {
        const QJsonObject e = error.toObject();
        QString message = clip(e.value(QStringLiteral("message")).toString());
        if (message.isEmpty()) message = QStringLiteral("error");
        const QJsonValue code = e.value(QStringLiteral("code"));
        // Both values in one arg() pass: a chained second arg() would replace a
        // "%1" inside the server's message instead of our "%2".
        return code.isDouble()
                   ? QStringLiteral("%1 (code %2)").arg(message, QString::number(code.toInteger()))
                   : message;
    }
    if (error.isString() && !error.toString().isEmpty()) return clip(error.toString());
    return QStringLiteral("error");
}

// Why `body` is not a usable batch answer; empty (with `answers` filled) when
// it is a JSON array.
QString batchBodyProblem(const QByteArray& body, QJsonArray* answers)
{
    if (body.trimmed().isEmpty()) return QStringLiteral("empty reply");
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parse);
    if (parse.error != QJsonParseError::NoError) return QStringLiteral("malformed JSON reply");
    if (doc.isArray()) {
        *answers = doc.array();
        return {};
    }
    // A server that rejects a whole batch typically answers with one error object.
    const QJsonValue error = doc.object().value(QStringLiteral("error"));
    if (doc.isObject() && !error.isUndefined() && !error.isNull())
        return QStringLiteral("reply is not a JSON array: ") + rpcErrorText(error);
    return QStringLiteral("reply is not a JSON array");
}

// Our ids are positive ints; 0 means "not one of ours".
int idOf(const QJsonValue& id)
{
    if (id.isDouble()) return id.toInt(0);           // 0 unless a whole number in range
    if (id.isString()) return id.toString().toInt(); // tolerate servers that echo ids as strings
    return 0;
}

} // namespace

// One batch() call. Its chunks fill `results` / `errors` in place; when the
// last one finishes they go to `reply`.
struct EthRpc::Batch {
    BatchReply reply;
    QList<QJsonValue> results;
    QStringList errors;
    int chunksLeft = 0;
};

// Up to kMaxBatch consecutive calls of a batch, posted as one JSON array and
// retried as a unit. Owned jointly by whatever currently waits on it: the
// queue entry, then the handler of the reply in flight.
struct EthRpc::Chunk : std::enable_shared_from_this<EthRpc::Chunk> {
    std::shared_ptr<Batch> batch;
    qsizetype first = 0; // position of the chunk's first call in the batch
    QList<int> ids;      // request id of each call, in order
    QByteArray body;     // the JSON array, built once and re-sent on failover
    QStringList tried;   // endpoints this chunk has already been sent to
};

QStringList defaultEthEndpoints()
{
    return {
        QStringLiteral("https://ethereum-rpc.publicnode.com"),
        QStringLiteral("https://eth-mainnet.public.blastapi.io"),
        QStringLiteral("https://rpc.mevblocker.io"),
        QStringLiteral("https://1rpc.io/eth"),
    };
}

EthRpc::EthRpc(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)), endpoints_(defaultEthEndpoints())
{
}

void EthRpc::setEndpoints(const QStringList& urls)
{
    // Balances are read over TLS only, so anything but an https URL is dropped.
    QStringList usable;
    for (const QString& entry : urls) {
        const QString url = entry.trimmed();
        if (url.isEmpty() || usable.contains(url)) continue;
        if (!isHttpsUrl(url)) {
            emit log(QStringLiteral("Ethereum RPC: ignored endpoint %1 (not an https URL)").arg(hostOf(url)));
            continue;
        }
        usable.append(url);
    }
    if (usable.isEmpty()) usable = defaultEthEndpoints();

    const QString before = currentEndpoint();
    endpoints_ = usable;
    current_ = 0;
    if (endpoints_.first() != before) emit endpointChanged(endpoints_.first());
}

QString EthRpc::currentEndpoint() const
{
    return endpoints_.value(current_);
}

void EthRpc::call(const QString& method, const QJsonArray& params, Reply reply)
{
    batch({qMakePair(method, params)},
          [reply = std::move(reply)](const QList<QJsonValue>& results, const QStringList& errors) {
              if (reply) reply(results.value(0), errors.value(0));
          });
}

void EthRpc::batch(const QList<QPair<QString, QJsonArray>>& calls, BatchReply reply)
{
    if (calls.isEmpty()) {
        // Asynchronous like every other answer, so callers need no special case.
        QTimer::singleShot(0, this, [reply = std::move(reply)] {
            if (reply) reply({}, {});
        });
        return;
    }

    auto job = std::make_shared<Batch>();
    job->reply = std::move(reply);
    job->results.resize(calls.size());
    job->errors.resize(calls.size());

    for (qsizetype first = 0; first < calls.size(); first += kMaxBatch) {
        const qsizetype count = qMin<qsizetype>(kMaxBatch, calls.size() - first);
        auto chunk = std::make_shared<Chunk>();
        chunk->batch = job;
        chunk->first = first;
        QJsonArray requests;
        for (qsizetype i = first; i < first + count; ++i) {
            const int id = nextId_;
            nextId_ = nextId_ == std::numeric_limits<int>::max() ? 1 : nextId_ + 1;
            chunk->ids.append(id);
            requests.append(QJsonObject{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), id},
                {QStringLiteral("method"), calls.at(i).first},
                {QStringLiteral("params"), calls.at(i).second},
            });
        }
        chunk->body = QJsonDocument(requests).toJson(QJsonDocument::Compact);
        ++job->chunksLeft;
        // Picks its endpoint when it actually starts: by then an earlier
        // failover may have moved current_.
        queue_.append([this, chunk] { send(*chunk, nextEndpoint(*chunk)); });
    }
    pump();
}

void EthRpc::pump()
{
    while (inFlight_ < kMaxInFlight && !queue_.isEmpty()) {
        ++inFlight_;
        const std::function<void()> start = queue_.takeFirst();
        start();
    }
}

QString EthRpc::nextEndpoint(const Chunk& chunk) const
{
    // Start at the shared current endpoint and skip the ones this chunk already
    // failed on. endpoints_ is never empty, so a fresh chunk always gets one.
    const int n = int(endpoints_.size());
    for (int k = 0; k < n; ++k) {
        const QString& url = endpoints_.at((current_ + k) % n);
        if (!chunk.tried.contains(url)) return url;
    }
    return {};
}

void EthRpc::send(Chunk& chunk, const QString& url)
{
    chunk.tried.append(url);

    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("GitWallet/0.1"));
    request.setTransferTimeout(kTransferTimeoutMs);

    // The reply is a child of nam_ and the handlers have `this` as their
    // context: once EthRpc is destroyed neither they nor the caller's reply
    // run. The finished handler co-owns the chunk until then.
    QNetworkReply* reply = nam_->post(request, chunk.body);

    // Nothing is read before the reply finishes, so bytesAvailable() is the
    // body received so far (already inflated when it came compressed).
    // abort() emits finished() before it returns and drops the buffered body,
    // hence the flag; the chunk then fails over like after any other transfer
    // failure. finished() checks the size again because Qt can end a reply
    // without a readyRead for its last data.
    auto oversized = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::readyRead, this, [reply, oversized] {
        if (!*oversized && reply->bytesAvailable() > kMaxReplyBytes) {
            *oversized = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, url, oversized, self = chunk.shared_from_this()] {
                reply->deleteLater();
                const QString failure =
                    failureOf(reply, *oversized || reply->bytesAvailable() > kMaxReplyBytes);
                handleReply(*self, url, failure, failure.isEmpty() ? reply->readAll() : QByteArray());
            });
}

void EthRpc::handleReply(Chunk& chunk, const QString& url, const QString& failure, const QByteArray& body)
{
    QJsonArray answers;
    const QString problem = failure.isEmpty() ? batchBodyProblem(body, &answers) : failure;
    if (!problem.isEmpty()) {
        retry(chunk, url, problem);
        return;
    }

    // Answers may come in any order: match them to the calls by id.
    QHash<int, QJsonObject> byId;
    for (const QJsonValue& answer : std::as_const(answers)) {
        const QJsonObject object = answer.toObject();
        const int id = idOf(object.value(QStringLiteral("id")));
        if (id > 0 && !byId.contains(id)) byId.insert(id, object);
    }

    Batch& job = *chunk.batch;
    for (qsizetype i = 0; i < chunk.ids.size(); ++i) {
        const qsizetype slot = chunk.first + i;
        const auto it = byId.constFind(chunk.ids.at(i));
        if (it == byId.cend()) {
            job.errors[slot] = QStringLiteral("no reply");
            continue;
        }
        // Some servers send "error": null next to a result; only a real error counts.
        const QJsonValue error = it->value(QStringLiteral("error"));
        if (!error.isUndefined() && !error.isNull())
            job.errors[slot] = rpcErrorText(error);
        else if (it->contains(QStringLiteral("result")))
            job.results[slot] = it->value(QStringLiteral("result"));
        else
            job.errors[slot] = QStringLiteral("reply has neither result nor error");
    }
    finish(chunk);
}

void EthRpc::retry(Chunk& chunk, const QString& failedUrl, const QString& reason)
{
    // Move the shared starting point past the endpoint that failed, unless an
    // earlier failure (or setEndpoints) already moved it. Later requests then
    // begin at the endpoint that works instead of re-trying a dead one first.
    if (endpoints_.size() > 1 && endpoints_.value(current_) == failedUrl) {
        current_ = (current_ + 1) % int(endpoints_.size());
        emit endpointChanged(endpoints_.at(current_));
    }

    const QString next = nextEndpoint(chunk);
    if (next.isEmpty()) {
        emit log(QStringLiteral("Ethereum RPC: %1 failed (%2); no endpoint left to try")
                     .arg(hostOf(failedUrl), reason));
        fail(chunk, reason);
        return;
    }
    emit log(QStringLiteral("Ethereum RPC: %1 failed (%2); retrying on %3")
                 .arg(hostOf(failedUrl), reason, hostOf(next)));
    send(chunk, next);
}

void EthRpc::fail(Chunk& chunk, const QString& error)
{
    Batch& job = *chunk.batch;
    for (qsizetype i = 0; i < chunk.ids.size(); ++i)
        job.errors[chunk.first + i] = error;
    finish(chunk);
}

void EthRpc::finish(Chunk& chunk)
{
    --inFlight_;
    const std::shared_ptr<Batch> job = chunk.batch;
    if (--job->chunksLeft == 0) {
        // Handed over from the event loop rather than from inside a
        // QNetworkReply signal, so the caller may start new requests or even
        // delete this object from its reply. With `this` as the context it is
        // dropped if EthRpc is destroyed first.
        QTimer::singleShot(0, this, [job] {
            const BatchReply reply = std::move(job->reply);
            if (reply) reply(job->results, job->errors);
        });
    }
    pump();
}

} // namespace net
