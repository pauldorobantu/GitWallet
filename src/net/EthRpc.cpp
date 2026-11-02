#include "net/EthRpc.h"

#include "net/Budget.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <limits>
#include <memory>
#include <utility>

namespace net {

namespace {

constexpr int kTransferTimeoutMs = 20000; // a request that moves no data for this long is aborted
// A request still unfinished this long after it was sent is aborted too, however
// slowly its data keeps arriving: the transfer timeout restarts with every byte,
// so a server that drips its answer would otherwise hold the request, its slot
// and its caller forever, and the failover would never start. The largest
// legitimate answer (see kMaxReplyBytes) needs a fraction of this.
constexpr int kRequestDeadlineMs = 45000;
// Requests on the wire at once; further chunks wait here. A big refresh then
// neither trips a public endpoint's rate limit nor queues inside Qt's per-host
// connection limit, where its transfer timeout would already be running.
constexpr int kMaxInFlight = 4;
// Calls per second an endpoint nobody measured gets: one of the user's own may well take
// more, a public one rarely (docs/ENDPOINTS.md).
constexpr int kDefaultCallsPerSecond = 50;
// An eth_call to Multicall3 reads hundreds of balances, and costs an endpoint far less than
// that many plain calls, but more than one: public endpoints that stop answering plain
// calls at tens a second keep answering these two a second.
constexpr int kCostOfACall = 10;
constexpr qsizetype kMaxServerText = 200; // server-supplied text is clipped (one line) for logs and UI
// A reply is buffered whole until it finishes, and the transfer timeout only
// fires when no data moves, so without a cap a server that streams fast could
// fill memory long before the deadline. The largest answers, a batch of kMaxBatch
// calls or an aggregate3 of 1,800 sub-calls, are about 600 KB; this leaves room for
// a chunk of logs as well.
constexpr qint64 kMaxReplyBytes = 16 * 1024 * 1024;
// How long an endpoint the caller rejected is tried last. A healthy endpoint
// behind a load balancer can return one bad answer (a lagging backend), so the
// doubt expires; a broken one is simply rejected again by its next answer.
constexpr int kDoubtMs = 10 * 60 * 1000;
// How long an endpoint that failed an attempt gets no new work while another is left. One
// that hangs has a full budget, and would otherwise take every chunk the others have no
// room for.
constexpr int kCoolingMs = 30 * 1000;

QString clip(const QString& text)
{
    const QString line = text.simplified(); // no line breaks from a server in our log
    return line.size() > kMaxServerText ? line.left(kMaxServerText) + QStringLiteral("...") : line;
}

// Host name only: a configured endpoint may carry an API key in its path or
// query, and that must not end up in a log.
QString hostOf(const QString& url)
{
    const QString host = QUrl(url).host();
    return host.isEmpty() ? QStringLiteral("(invalid endpoint)") : host;
}

// errorString() may quote a URL. Of every URL in `text` keep only the scheme
// and host, for the same reason as hostOf().
QString withoutUrlSecrets(const QString& text)
{
    static const QRegularExpression urlRe(QStringLiteral(R"([A-Za-z][A-Za-z0-9+.\-]*://[^\s"'<>]+)"));
    QString out;
    qsizetype last = 0;
    for (QRegularExpressionMatchIterator it = urlRe.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        const QUrl u(m.captured());
        out += u.host().isEmpty() ? QStringLiteral("(url)") : u.scheme() + QStringLiteral("://") + u.host();
        last = m.capturedEnd();
    }
    return out + text.mid(last);
}

bool isHttpsUrl(const QString& url)
{
    const QUrl u(url, QUrl::StrictMode);
    return u.isValid() && u.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0
           && !u.host().isEmpty();
}

// Why the transfer failed; empty when it delivered an HTTP 200 body.
// `aborted`: why send() cut the transfer short itself (a body longer than
// kMaxReplyBytes, or the deadline); empty when it did not.
QString failureOf(QNetworkReply* reply, const QString& aborted)
{
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid() && status.toInt() != 200) {
        // Built here rather than taken from errorString(), which quotes the
        // full URL (and with it any API key).
        const QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        return reason.isEmpty() ? QStringLiteral("HTTP %1").arg(status.toInt())
                                : QStringLiteral("HTTP %1 %2").arg(status.toInt()).arg(clip(reason));
    }
    // Only now: an HTTP error status says more than why we cut its error page
    // short, and our own abort would otherwise read as a timeout below.
    if (!aborted.isEmpty()) return aborted;
    switch (reply->error()) {
    case QNetworkReply::NoError:
        return status.isValid() ? QString() : QStringLiteral("no HTTP status");
    case QNetworkReply::OperationCanceledError: // how the transfer timeout's abort is reported
    case QNetworkReply::TimeoutError:
        return QStringLiteral("timed out");
    default:
        return clip(withoutUrlSecrets(reply->errorString()));
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

// Who runs an endpoint, as far as its name says: what its limits go by, whichever
// network's host it is.
QString providerOf(const QString& url)
{
    const QStringList labels = QUrl(url).host().split(QLatin1Char('.'));
    return labels.size() < 2 ? labels.join(QLatin1Char('.')) : labels.mid(labels.size() - 2).join(QLatin1Char('.'));
}

// Whether an eth_chainId result names `expected`: a hex quantity, leading zeros
// tolerated.
bool namesChain(const QJsonValue& chainId, quint64 expected)
{
    if (!chainId.isString()) return false;
    const QString text = chainId.toString();
    if (text.size() < 3 || text.at(0) != QLatin1Char('0')
        || (text.at(1) != QLatin1Char('x') && text.at(1) != QLatin1Char('X')))
        return false;
    bool ok = false;
    const quint64 value = text.mid(2).toULongLong(&ok, 16);
    return ok && value == expected;
}

} // namespace

// One batch() call. Each of its chunks writes the answers of its calls in here
// when it is done; when the last one is, they go to `reply`.
struct EthRpc::Batch {
    BatchReply reply;
    QList<QJsonValue> results;
    QStringList errors;
    QStringList endpoints;
    int chunksLeft = 0;
};

// Up to kMaxBatch consecutive calls of a batch, posted as one JSON array and
// failed over as a unit. It delivers the answer of a single attempt, never a
// mix of two endpoints, so one of its calls (eth_chainId) can prove where all
// the others were answered. Owned by the queue while it waits, then by wire_
// and the handler of its reply in flight.
struct EthRpc::Chunk : std::enable_shared_from_this<EthRpc::Chunk> {
    std::shared_ptr<Batch> batch;
    qsizetype first = 0;            // position of the chunk's first call in the batch
    QList<int> ids;                 // request id of each call, in order
    QList<qsizetype> chainIdCalls;  // positions of its eth_chainId calls
    QByteArray body;                // the JSON array, built once and re-sent on failover
    QStringList tried;              // endpoints this chunk has already been sent to
    int cost = 0;                   // what it counts for in a host's budget
    QNetworkReply* reply = nullptr; // the attempt on the wire, if any (a child of nam_)
    bool lastResort = false;        // that attempt went to an endpoint found on another chain (nextEndpoint())
    // What it delivers: the first attempt that answers every call, else the
    // attempt with the most results that count, so far.
    QList<QJsonValue> results;
    QStringList errors;
    QString answeredBy; // the endpoint of that attempt
    int counted = -1;   // results of that attempt that count; -1 before the first attempt
};

EthRpc::EthRpc(const QString& name, quint64 chainId, const QStringList& builtIn, const QHash<QString, int>& pace,
               BudgetPool* budgets, QObject* parent)
    : QObject(parent)
    , name_(name)
    , chainId_(chainId)
    , builtIn_(builtIn)
    , budgets_(budgets)
    , pumpTimer_(new QTimer(this))
    , nam_(new QNetworkAccessManager(this))
    , endpoints_(builtIn)
{
    // By who runs them: a line in Settings that spells the address otherwise is the same provider.
    for (auto it = pace.cbegin(); it != pace.cend(); ++it) {
        const QString provider = providerOf(it.key());
        const auto known = pace_.constFind(provider);
        pace_.insert(provider, known == pace_.cend() ? it.value() : qMin(*known, it.value()));
    }
    pumpTimer_->setSingleShot(true);
    connect(pumpTimer_, &QTimer::timeout, this, &EthRpc::pump);
}

void EthRpc::setEndpoints(const QStringList& urls)
{
    // Balances are read over TLS only, so anything but an https URL is dropped.
    QStringList usable;
    QStringList ignored; // their hosts, logged once the new list is in place
    for (const QString& entry : urls) {
        const QString url = entry.trimmed();
        if (url.isEmpty() || usable.contains(url)) continue;
        if (!usableUrl(url)) {
            ignored.append(hostOf(url));
            continue;
        }
        usable.append(url);
    }
    if (usable.isEmpty()) usable = builtIn_;

    pumpTimer_->stop();
    // A new list starts over. Whatever is still pending belongs to the old one
    // and ends here, with an error and without failover: a request stuck on an
    // endpoint (or on one just removed) cannot keep its slot, and nothing it
    // obtained arrives after this.
    const int cancelled = cancelPending(QStringLiteral("cancelled: the endpoint list changed"));

    const QString before = currentEndpoint();
    endpoints_ = usable;
    offChain_.clear();
    doubted_.clear();
    cooling_.clear();
    current_ = 0;

    // Signals last: a slot may already start requests on the new list.
    for (const QString& host : std::as_const(ignored))
        emit log(QStringLiteral("%1 RPC: ignored endpoint %2 (not an https URL)").arg(name_, host));
    if (cancelled > 0)
        emit log(QStringLiteral("%1 RPC: endpoint list changed; %2 pending request(s) cancelled")
                     .arg(name_).arg(cancelled));
    if (endpoints_.first() != before) emit endpointChanged(endpoints_.first());
}

QString EthRpc::currentEndpoint() const
{
    return endpoints_.value(current_);
}

void EthRpc::cancelAll()
{
    pumpTimer_->stop();
    cancelPending(QStringLiteral("stopped"));
}

bool EthRpc::usableUrl(const QString& url) const
{
    if (isHttpsUrl(url)) return true;
    if (!localForTest_) return false;
    const QUrl u(url, QUrl::StrictMode);
    return u.isValid() && u.scheme() == QLatin1String("http") && u.host() == QLatin1String("127.0.0.1");
}

void EthRpc::call(const QString& method, const QJsonArray& params, Reply reply)
{
    batch({qMakePair(method, params)},
          [reply = std::move(reply)](const QList<QJsonValue>& results, const QStringList& errors,
                                     const QStringList& endpoints) {
              if (reply) reply(results.value(0), errors.value(0), endpoints.value(0));
          });
}

void EthRpc::batch(const QList<QPair<QString, QJsonArray>>& calls, BatchReply reply)
{
    if (calls.isEmpty()) {
        // Asynchronous like every other answer, so callers need no special case.
        QTimer::singleShot(0, this, [reply = std::move(reply)] {
            if (reply) reply({}, {}, {});
        });
        return;
    }

    auto job = std::make_shared<Batch>();
    job->reply = std::move(reply);
    job->results.resize(calls.size());
    job->errors.resize(calls.size());
    job->endpoints.resize(calls.size());

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
            chunk->cost += calls.at(i).first == QLatin1String("eth_call") ? kCostOfACall : 1;
            if (calls.at(i).first == QLatin1String("eth_chainId")) chunk->chainIdCalls.append(i - first);
            requests.append(QJsonObject{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), id},
                {QStringLiteral("method"), calls.at(i).first},
                {QStringLiteral("params"), calls.at(i).second},
            });
        }
        chunk->body = QJsonDocument(requests).toJson(QJsonDocument::Compact);
        ++job->chunksLeft;
        queue_.append(chunk);
    }
    pump();
}

void EthRpc::pump()
{
    while (wire_.size() < kMaxInFlight && !queue_.isEmpty()) {
        const std::shared_ptr<Chunk> first = queue_.constFirst();
        // Picks its endpoint only now: by then an earlier failover may have
        // moved current_. If that one is at its pace, the next with room takes it: spread
        // over them, each sees part of the addresses, and none is asked past what it answers.
        QString url = nextEndpoint(*first);
        if (url.isEmpty()) {
            // Back in line after a failure, and the endpoints it had left have gone since.
            queue_.removeFirst();
            deliver(*first);
            continue;
        }
        int wait = takeBudget(url, first->cost);
        const int n = int(endpoints_.size());
        for (int k = 1; wait > 0 && k < n; ++k) {
            const QString& other = endpoints_.at((current_ + k) % n);
            if (other == url || first->tried.contains(other) || offChain_.contains(other) || isAvoided(other))
                continue;
            const int waitThere = takeBudget(other, first->cost);
            if (waitThere == 0) {
                url = other;
                wait = 0;
            } else {
                wait = qMin(wait, waitThere);
            }
        }
        if (wait > 0) {
            if (!pumpTimer_->isActive())
                pumpTimer_->start(wait);
            return;
        }
        const std::shared_ptr<Chunk> chunk = queue_.takeFirst();
        wire_.append(chunk);
        send(*chunk, url);
    }
}

int EthRpc::takeBudget(const QString& url, int cost)
{
    if (!budgets_)
        return 0;
    const QString provider = providerOf(url);
    return budgets_->forHost(provider, pace_.value(provider, kDefaultCallsPerSecond)).take(cost);
}

QString EthRpc::nextEndpoint(const Chunk& chunk) const
{
    // Start at the shared current endpoint and skip the ones this chunk already
    // failed on and those on another chain. Endpoints the caller doubts come
    // last: a glitch that got a healthy endpoint rejected must not leave a
    // chunk without the one endpoint that works.
    const int n = int(endpoints_.size());
    for (const bool lastTry : {false, true}) {
        for (int k = 0; k < n; ++k) {
            const QString& url = endpoints_.at((current_ + k) % n);
            if (chunk.tried.contains(url) || offChain_.contains(url)) continue;
            if (lastTry || !isAvoided(url)) return url;
        }
    }
    // Every endpoint is on another chain: a fresh chunk still goes to one
    // (endpoints_ is never empty), and its caller learns what is wrong.
    return chunk.tried.isEmpty() ? endpoints_.value(current_) : QString();
}

bool EthRpc::isDoubted(const QString& url) const
{
    const auto it = doubted_.constFind(url);
    return it != doubted_.cend() && !it->hasExpired();
}

bool EthRpc::isAvoided(const QString& url) const
{
    if (isDoubted(url)) return true;
    const auto it = cooling_.constFind(url);
    return it != cooling_.cend() && !it->hasExpired();
}

bool EthRpc::isOffChain(const QString& url) const
{
    return offChain_.contains(url);
}

void EthRpc::send(Chunk& chunk, const QString& url)
{
    chunk.tried.append(url);
    chunk.lastResort = offChain_.contains(url);

    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("GitWallet/0.1"));
    request.setTransferTimeout(kTransferTimeoutMs);

    // The reply is a child of nam_ and the handlers have `this` as their
    // context: once EthRpc is destroyed neither they nor the caller's reply
    // run. The finished handler co-owns the chunk until then, and
    // cancelPending() disconnects it.
    QNetworkReply* reply = nam_->post(request, chunk.body);
    chunk.reply = reply;

    // Nothing is read before the reply finishes, so bytesAvailable() is the
    // body received so far (already inflated when it came compressed).
    // abort() emits finished() before it returns and drops the buffered body,
    // hence `aborted`; the chunk then fails over like after any other transfer
    // failure. finished() checks the size again because Qt can end a reply
    // without a readyRead for its last data.
    auto aborted = std::make_shared<QString>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, aborted] {
        if (aborted->isEmpty() && reply->bytesAvailable() > kMaxReplyBytes) {
            *aborted = QStringLiteral("reply too large");
            reply->abort();
        }
    });
    // The reply as its context: the deadline ends with the reply. Between
    // finished() and the deferred delete it may still fire, hence the check.
    QTimer::singleShot(kRequestDeadlineMs, reply, [reply, aborted] {
        if (!aborted->isEmpty() || reply->isFinished()) return;
        *aborted = QStringLiteral("no complete reply within %1 s").arg(kRequestDeadlineMs / 1000);
        reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, url, aborted, self = chunk.shared_from_this()] {
                reply->deleteLater();
                self->reply = nullptr;
                if (aborted->isEmpty() && reply->bytesAvailable() > kMaxReplyBytes)
                    *aborted = QStringLiteral("reply too large");
                const QString failure = failureOf(reply, *aborted);
                handleReply(*self, url, failure, failure.isEmpty() ? reply->readAll() : QByteArray());
            });
}

void EthRpc::handleReply(Chunk& chunk, const QString& url, const QString& failure, const QByteArray& body)
{
    const qsizetype n = chunk.ids.size();
    QList<QJsonValue> results(n);
    QStringList errors(n);
    int answered = 0; // calls that came back with a result
    QString reason;   // why this attempt does not settle the chunk; empty when it does

    QJsonArray answers;
    const QString problem = failure.isEmpty() ? batchBodyProblem(body, &answers) : failure;
    if (!problem.isEmpty()) {
        errors.fill(problem);
        reason = problem;
    } else {
        // Answers may come in any order: match them to the calls by id. Only
        // the first answer to each of our calls is kept, nothing else.
        QHash<int, qsizetype> position;
        for (qsizetype i = 0; i < n; ++i)
            position.insert(chunk.ids.at(i), i);
        QList<QJsonObject> byCall(n);
        QList<bool> seen(n, false);
        for (const QJsonValue& answer : std::as_const(answers)) {
            const QJsonObject object = answer.toObject();
            const auto it = position.constFind(idOf(object.value(QStringLiteral("id"))));
            if (it == position.cend() || seen.at(*it)) continue;
            seen[*it] = true;
            byCall[*it] = object;
        }

        QString firstError;
        for (qsizetype i = 0; i < n; ++i) {
            const QJsonObject& object = byCall.at(i);
            // Some servers send "error": null next to a result; only a real error counts.
            const QJsonValue error = object.value(QStringLiteral("error"));
            if (!seen.at(i))
                errors[i] = QStringLiteral("no reply");
            else if (!error.isUndefined() && !error.isNull())
                errors[i] = rpcErrorText(error);
            else if (!object.contains(QStringLiteral("result")))
                errors[i] = QStringLiteral("reply has neither result nor error");
            else {
                results[i] = object.value(QStringLiteral("result"));
                ++answered;
                continue;
            }
            if (firstError.isEmpty()) firstError = errors.at(i);
        }
        // A call without a result is the endpoint's doing (a rate limit or quota
        // reported in-band, a method it disables, a node that lags): the chunk
        // moves on as after a transport failure, instead of every retry of the
        // caller landing on the same endpoint again.
        if (answered < n) {
            reason = answered == 0 ? firstError
                                   : QStringLiteral("%1 of %2 calls failed: %3")
                                         .arg(QString::number(n - answered), QString::number(n), firstError);
        }
    }

    // This network only. Settings accept any https URL, and an L2 or testnet
    // endpoint answers every call as if nothing were wrong: where the request
    // asks eth_chainId, only this network's id settles it. Another chain's answers are
    // not delivered at all, and its endpoint is skipped until the list changes.
    for (const qsizetype i : std::as_const(chunk.chainIdCalls)) {
        if (!errors.at(i).isEmpty()) continue; // moves on already
        const QJsonValue chainId = results.at(i);
        if (namesChain(chainId, chainId_)) {
            // Reached as the last resort, and its chain is fine now; a caller
            // that still finds its answers unusable rejects it again. An
            // answer to a request sent before the rejection lifts nothing.
            if (chunk.lastResort) offChain_.removeAll(url);
            continue;
        }
        const QString why = chainId.isString()
                                ? QStringLiteral("endpoint is not on %1 (eth_chainId %2)").arg(name_, clip(chainId.toString()))
                                : QStringLiteral("endpoint is not on %1 (no valid eth_chainId answer)").arg(name_);
        results = QList<QJsonValue>(n);
        errors.fill(why);
        answered = 0;
        reason = why + QStringLiteral("; skipped until the endpoint list changes");
        if (!offChain_.contains(url)) offChain_.append(url);
        break;
    }

    // Kept when it carries more results than the answer kept so far. While none
    // carries any, the latest attempt is kept: its errors are the freshest.
    if (answered > chunk.counted || chunk.counted <= 0) {
        chunk.results = results;
        chunk.errors = errors;
        chunk.answeredBy = url;
        chunk.counted = answered;
    }
    if (reason.isEmpty()) {
        cooling_.remove(url);
        finish(chunk);
    } else {
        cooling_.insert(url, QDeadlineTimer(kCoolingMs));
        retry(chunk, url, reason);
    }
}

// Moves the shared starting point past `url` if it is still the current
// endpoint (an earlier failure, or setEndpoints(), may have moved it already),
// to the next one on this network, preferring one the caller does not doubt. Later
// requests then begin at an endpoint that works instead of trying a dead one
// first. Returns the endpoint it moved to, or an empty string if it stayed.
QString EthRpc::moveCurrentPast(const QString& url)
{
    const int n = int(endpoints_.size());
    if (n < 2 || endpoints_.value(current_) != url) return {};
    for (const bool lastTry : {false, true}) {
        for (int k = 1; k < n; ++k) {
            const int i = (current_ + k) % n;
            const QString& next = endpoints_.at(i);
            if (offChain_.contains(next) || (!lastTry && isAvoided(next))) continue;
            current_ = i;
            return next;
        }
    }
    return {};
}

void EthRpc::rejectEndpoint(const QString& url)
{
    if (!endpoints_.contains(url)) return; // empty, or of a list setEndpoints() replaced
    const bool added = !isDoubted(url);
    doubted_.insert(url, QDeadlineTimer(kDoubtMs)); // a repeat starts the time over
    const QString switchedTo = moveCurrentPast(url);

    if (!switchedTo.isEmpty()) emit endpointChanged(switchedTo);
    if (added)
        emit log(QStringLiteral("%1 RPC: %2 rejected, its answers are unusable; tried last for %3 min%4")
                     .arg(name_, hostOf(url), QString::number(kDoubtMs / 60000),
                          switchedTo.isEmpty() ? QString() : QStringLiteral("; now using ") + hostOf(switchedTo)));
}

void EthRpc::retry(Chunk& chunk, const QString& failedUrl, const QString& reason)
{
    const QString switchedTo = moveCurrentPast(failedUrl);

    const QString next = nextEndpoint(chunk);
    if (next.isEmpty()) {
        finish(chunk); // with the most complete answer a single endpoint gave
    } else if (takeBudget(next, chunk.cost) == 0) {
        send(chunk, next);
    } else {
        // That one is at its pace: first in line for whichever has room next.
        const std::shared_ptr<Chunk> waiting = chunk.shared_from_this();
        wire_.removeIf([&chunk](const std::shared_ptr<Chunk>& c) { return c.get() == &chunk; });
        queue_.prepend(waiting);
        pump();
    }

    // Signals only now that the chunk is on the wire again, or done: a slot
    // may start requests or even call setEndpoints().
    if (!switchedTo.isEmpty()) emit endpointChanged(switchedTo);
    if (next.isEmpty())
        emit log(QStringLiteral("%1 RPC: %2 failed (%3); no endpoint left to try").arg(name_, hostOf(failedUrl), reason));
    else
        emit log(QStringLiteral("%1 RPC: %2 failed (%3); retrying on %4").arg(name_, hostOf(failedUrl), reason, hostOf(next)));
}

void EthRpc::finish(Chunk& chunk)
{
    // Off the wire (the handler of its last reply still holds it), then done.
    wire_.removeIf([&chunk](const std::shared_ptr<Chunk>& c) { return c.get() == &chunk; });
    deliver(chunk);
    pump();
}

// The chunk's answer goes into its batch, and the batch to its caller once its
// last chunk is done too.
void EthRpc::deliver(Chunk& chunk)
{
    const std::shared_ptr<Batch> job = chunk.batch;
    for (qsizetype i = 0; i < chunk.ids.size(); ++i) {
        job->results[chunk.first + i] = chunk.results.value(i);
        job->errors[chunk.first + i] = chunk.errors.value(i, QStringLiteral("no reply"));
        job->endpoints[chunk.first + i] = chunk.answeredBy;
    }
    if (--job->chunksLeft == 0) {
        // Handed over from the event loop rather than from inside a
        // QNetworkReply signal, so the caller may start new requests, reject
        // an endpoint or even delete this object from its reply. With `this`
        // as the context it is dropped if EthRpc is destroyed first.
        QTimer::singleShot(0, this, [job] {
            const BatchReply reply = std::move(job->reply);
            if (reply) reply(job->results, job->errors, job->endpoints);
        });
    }
}

// Ends every chunk still waiting or on the wire, without failover: each of its
// calls fails with `why`. Returns how many chunks that was.
int EthRpc::cancelPending(const QString& why)
{
    QList<std::shared_ptr<Chunk>> ended = std::exchange(queue_, {});
    const QList<std::shared_ptr<Chunk>> sending = std::exchange(wire_, {});
    // Every handler is cut off before the first abort(), which emits finished()
    // synchronously: none of them may fail over or deliver in the middle.
    QList<QNetworkReply*> replies;
    for (const std::shared_ptr<Chunk>& chunk : sending) {
        if (QNetworkReply* reply = std::exchange(chunk->reply, nullptr)) {
            reply->disconnect(this);
            replies.append(reply);
        }
    }
    for (QNetworkReply* reply : std::as_const(replies)) {
        reply->abort();
        reply->deleteLater();
    }
    ended += sending;
    for (const std::shared_ptr<Chunk>& chunk : std::as_const(ended)) {
        const qsizetype n = chunk->ids.size();
        chunk->results = QList<QJsonValue>(n);
        chunk->errors = QStringList(n, why);
        chunk->answeredBy.clear(); // no endpoint answered this
        deliver(*chunk);
    }
    return int(ended.size());
}

} // namespace net
