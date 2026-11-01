#include "net/ElectrumClient.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QSslError>
#include <QSslSocket>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <limits>

namespace net {

namespace {

// DNS + TCP. Qt tries a host's addresses one after another and moves on only
// when it or Windows gives up on one (about 40 s for an address that never
// answers the SYN). A dual-stack server behind a broken IPv6 route is reached
// over IPv4 only after that fallback, so this is a backstop well beyond one
// address, not the usual way a dead server fails.
constexpr int kConnectTimeoutMs = 90'000;
constexpr int kHandshakeTimeoutMs = 15'000; // per phase: the TLS handshake, then the protocol handshake
constexpr int kRequestTimeoutMs = 60'000;   // the oldest unanswered request
constexpr int kPingIntervalMs = 60'000;
constexpr int kPingTimeoutMs = 30'000;
constexpr int kWatchdogIntervalMs = 5'000;
constexpr int kFirstBackoffMs = 1'000;
constexpr int kMaxBackoffMs = 30'000;
// A subscription the server refused is asked again on this many ping ticks
// before the client looks for a server that accepts it.
constexpr int kRefusedRetries = 2;
constexpr qsizetype kMaxLineBytes = qsizetype(64) * 1024 * 1024;
constexpr qsizetype kMaxServerText = 300;

constexpr QLatin1String kScripthashSubscribe("blockchain.scripthash.subscribe");
constexpr QLatin1String kHeadersSubscribe("blockchain.headers.subscribe");

// Timeouts need a clock that never jumps; the wall clock can (time-zone or
// NTP corrections).
qint64 monotonicMs()
{
    static const QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

QJsonObject makeRequest(int id, const QString& method, const QJsonArray& params)
{
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    };
}

// A scripthash status is a hex string, or JSON null for "no history" ("").
QString statusOf(const QJsonValue& value)
{
    return value.isString() ? value.toString() : QString();
}

// Text a server sent ends up in the log and the UI: one line, bounded.
QString clipped(const QString& text)
{
    QString line = text.simplified();
    if (line.size() > kMaxServerText) {
        line.truncate(kMaxServerText);
        line += QStringLiteral("...");
    }
    return line;
}

// The "error" member: an object with "message" (Fulcrum, ElectrumX and electrs
// all answer that way) or a bare string. Never empty, so callers can rely on
// error.isEmpty() meaning success.
QString errorText(const QJsonValue& error)
{
    QString text;
    if (error.isObject()) {
        const QJsonObject object = error.toObject();
        text = object.value(QLatin1String("message")).toString();
        if (text.isEmpty())
            text = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    } else if (error.isString()) {
        text = error.toString();
    }
    text = clipped(text);
    return text.isEmpty() ? QStringLiteral("server error") : text;
}

bool sameServers(const QList<ElectrumServer>& a, const QList<ElectrumServer>& b)
{
    return std::equal(a.cbegin(), a.cend(), b.cbegin(), b.cend(),
                      [](const ElectrumServer& x, const ElectrumServer& y) {
                          return x.port == y.port && x.host.compare(y.host, Qt::CaseInsensitive) == 0;
                      });
}

} // namespace

// ------------------------------------------------------------ servers ----

bool ElectrumServer::fromString(const QString& text, ElectrumServer* out)
{
    const QString trimmed = text.trimmed();
    // The last colon, so the port is always the final field.
    const qsizetype colon = trimmed.lastIndexOf(QLatin1Char(':'));
    if (colon < 0)
        return false;
    const QString host = trimmed.left(colon).trimmed();
    const QString portText = trimmed.mid(colon + 1).trimmed();
    if (host.isEmpty() || portText.isEmpty() || portText.size() > 5)
        return false;
    for (const QChar c : portText)
        if (c.unicode() < u'0' || c.unicode() > u'9')
            return false;
    const int port = portText.toInt();
    if (port < 1 || port > 65535)
        return false;
    if (out) {
        out->host = host;
        out->port = static_cast<quint16>(port);
    }
    return true;
}

QList<ElectrumServer> defaultElectrumServers()
{
    // Preference order; the client rotates through it when a server fails.
    return {
        {QStringLiteral("electrum.jhoenicke.de"), 50002},
        {QStringLiteral("e2.keff.org"), 50002},
        {QStringLiteral("e.keff.org"), 50002},
        {QStringLiteral("fulcrum.grey.pw"), 51002},
        {QStringLiteral("electrum.hodlister.co"), 50002},
        {QStringLiteral("electrum.blockstream.info"), 50002},
        {QStringLiteral("blackie.c3-soft.com"), 57002},
        {QStringLiteral("electrum.diynodes.com"), 50022},
        {QStringLiteral("blockstream.info"), 700},
    };
}

// ------------------------------------------------------------- client ----

ElectrumClient::ElectrumClient(QObject* parent)
    : QObject(parent)
{
    servers_ = defaultElectrumServers();

    pingTimer_ = new QTimer(this);
    pingTimer_->setInterval(kPingIntervalMs);
    connect(pingTimer_, &QTimer::timeout, this, [this] {
        sendPing();
        retryRefused();
    });

    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setSingleShot(true);
    connect(reconnectTimer_, &QTimer::timeout, this, &ElectrumClient::connectNext);

    handshakeTimer_ = new QTimer(this);
    handshakeTimer_->setSingleShot(true);
    connect(handshakeTimer_, &QTimer::timeout, this, [this] {
        if (!socket_ || ready_)
            return;
        if (socket_->isEncrypted())
            failConnection(QStringLiteral("no handshake reply within %1 s").arg(kHandshakeTimeoutMs / 1000));
        else if (socket_->state() == QAbstractSocket::ConnectedState)
            failConnection(QStringLiteral("no TLS handshake within %1 s").arg(kHandshakeTimeoutMs / 1000));
        else
            failConnection(QStringLiteral("no TCP connection within %1 s").arg(kConnectTimeoutMs / 1000));
    });

    watchdogTimer_ = new QTimer(this);
    watchdogTimer_->setInterval(kWatchdogIntervalMs);
    connect(watchdogTimer_, &QTimer::timeout, this, &ElectrumClient::checkTimeouts);
}

ElectrumClient::~ElectrumClient()
{
    // What the callbacks captured (scanners, pages) may already be destroyed:
    // drop them without running them.
    queue_.clear();
    inFlight_.clear();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
    }
}

void ElectrumClient::setServers(const QList<ElectrumServer>& servers)
{
    const QList<ElectrumServer> list = servers.isEmpty() ? defaultElectrumServers() : servers;
    if (sameServers(list, servers_))
        return;
    servers_ = list;
    serverIndex_ = -1; // the next connection starts at the first server
    goodIndex_ = -1;
    refusalSwitches_ = 0;
    // The controller normally sets servers while stopped; a live connection
    // moves to the new list right away.
    if (running_ && socket_)
        failConnection(QStringLiteral("server list changed"));
}

void ElectrumClient::start()
{
    if (running_)
        return;
    running_ = true;
    backoffMs_ = kFirstBackoffMs;
    refusalSwitches_ = 0;
    if (servers_.isEmpty())
        servers_ = defaultElectrumServers();
    // Begin with the server that last completed a handshake (the first one if
    // none has); failures rotate onwards from there.
    serverIndex_ = goodIndex_ >= 0 ? goodIndex_ - 1 : -1;
    connectNext();
}

void ElectrumClient::stop()
{
    running_ = false;
    ready_ = false;
    ++generation_;
    reconnectTimer_->stop();
    handshakeTimer_->stop();
    pingTimer_->stop();
    watchdogTimer_->stop();
    if (socket_) {
        QSslSocket* sock = socket_;
        socket_ = nullptr;
        sock->disconnect(this);
        sock->abort();
        sock->deleteLater();
    }
    resetConnectionState();
    // The scanners subscribe again when they start; keeping the list would
    // re-subscribe addresses of a wallet that may be gone by then.
    subscriptions_.clear();

    // Collected first: a callback may queue new requests or even start() again.
    QList<Pending> dropped = takeInFlight();
    while (!queue_.isEmpty())
        dropped.append(queue_.dequeue());
    const QString why = QStringLiteral("stopped");
    for (Pending& p : dropped)
        if (p.reply && !p.internal) // the client's own re-subscriptions have nobody to tell
            p.reply(QJsonValue(), why);
}

QString ElectrumClient::serverName() const
{
    return socket_ && socket_->isEncrypted() ? current_.toString() : QString();
}

void ElectrumClient::request(const QString& method, const QJsonArray& params, Reply reply)
{
    Pending p;
    p.id = takeId();
    p.method = method;
    p.params = params;
    p.reply = std::move(reply);
    queue_.enqueue(std::move(p));
    pump();
}

void ElectrumClient::subscribeScripthash(const QString& scripthash, Reply firstStatus)
{
    // One spelling for the list, the request and the notifications, which the
    // servers send in lowercase hex.
    const QString key = scripthash.toLower();
    // Listed now, not on the reply: if the server refuses it, the next
    // connection tries again.
    if (!subscriptions_.contains(key))
        subscriptions_.insert(key, QString());
    request(QString(kScripthashSubscribe), QJsonArray{key},
            [this, key, firstStatus = std::move(firstStatus)](const QJsonValue& result, const QString& error) {
                if (error.isEmpty()) {
                    subscriptions_.insert(key, statusOf(result));
                    refused_.remove(key);
                } else if (subscriptions_.contains(key)) {
                    // Asked again on the ping tick as well. (Not after stop(),
                    // which empties the list before it fails the request.)
                    noteRefused(key, error);
                }
                if (firstStatus)
                    firstStatus(result, error);
            });
}

// --------------------------------------------------------- connection ----

void ElectrumClient::connectNext()
{
    if (!running_ || socket_)
        return;
    if (servers_.isEmpty())
        servers_ = defaultElectrumServers();
    serverIndex_ = (serverIndex_ + 1) % static_cast<int>(servers_.size());
    current_ = servers_.at(serverIndex_);
    ++generation_;

    const quint64 gen = generation_;
    QPointer<ElectrumClient> self(this);
    emit log(QStringLiteral("connecting to %1").arg(current_.toString()));
    if (!self || gen != generation_ || !running_)
        return; // a log listener stopped or restarted the client

    auto* sock = new QSslSocket(this);
    socket_ = sock;
    // The default configuration already verifies against the system CAs with
    // SNI = host; set explicitly so no application-wide default can lower it.
    sock->setPeerVerifyMode(QSslSocket::VerifyPeer);
    // `sock == socket_` guards against signals of a socket already replaced.
    connect(sock, &QAbstractSocket::connected, this, [this, sock] {
        if (sock == socket_)
            handshakeTimer_->start(kHandshakeTimeoutMs); // TCP is up: TLS gets a budget of its own
    });
    connect(sock, &QSslSocket::encrypted, this, [this, sock] {
        if (sock == socket_)
            onEncrypted();
    });
    connect(sock, &QSslSocket::readyRead, this, [this, sock] {
        if (sock == socket_)
            onReadyRead();
    });
    connect(sock, &QSslSocket::sslErrors, this, [this, sock](const QList<QSslError>& errors) {
        if (sock != socket_)
            return;
        QStringList texts;
        for (const QSslError& e : errors)
            texts.append(e.errorString());
        texts.removeDuplicates(); // Schannel repeats an error once per certificate in the chain
        failConnection(texts.isEmpty()
                           ? QStringLiteral("TLS certificate rejected")
                           : QStringLiteral("TLS certificate rejected (%1)").arg(texts.join(QStringLiteral("; "))));
    });
    connect(sock, &QAbstractSocket::errorOccurred, this, [this, sock](QAbstractSocket::SocketError) {
        if (sock == socket_)
            failConnection(sock->errorString());
    });
    connect(sock, &QAbstractSocket::disconnected, this, [this, sock] {
        if (sock == socket_)
            failConnection(QStringLiteral("connection closed"));
    });
    // Before connecting: a cached host lookup can make the socket connect, or
    // fail, inside connectToHostEncrypted(); connected() then restarts this
    // timer, failConnection() stops it.
    handshakeTimer_->start(kConnectTimeoutMs);
    sock->connectToHostEncrypted(current_.host, current_.port);
}

void ElectrumClient::onEncrypted()
{
    // Requests are small and pipelined; Nagle would only hold them back.
    socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    handshakeTimer_->start(kHandshakeTimeoutMs);
    versionId_ = takeId();
    sendJson(makeRequest(versionId_, QStringLiteral("server.version"),
                         QJsonArray{QStringLiteral("GitWallet 0.1"), QStringLiteral("1.4")}));
}

void ElectrumClient::handleVersionReply(const QJsonValue& result, const QString& error)
{
    versionId_ = 0;
    if (!error.isEmpty()) {
        failConnection(QStringLiteral("server.version refused: %1").arg(error));
        return;
    }
    const QJsonArray version = result.toArray(); // [software, protocol]
    serverSoftware_ = version.isEmpty() ? QString() : clipped(version.at(0).toString());
    headersId_ = takeId();
    sendJson(makeRequest(headersId_, QString(kHeadersSubscribe), QJsonArray()));
}

void ElectrumClient::handleHeadersReply(const QJsonValue& result, const QString& error)
{
    headersId_ = 0;
    if (!error.isEmpty()) {
        failConnection(QStringLiteral("blockchain.headers.subscribe refused: %1").arg(error));
        return;
    }
    const int height = result.toObject().value(QLatin1String("height")).toInt(-1);
    if (height < 0) {
        failConnection(QStringLiteral("blockchain.headers.subscribe returned no height"));
        return;
    }
    becomeReady(height);
}

void ElectrumClient::becomeReady(int height)
{
    handshakeTimer_->stop();
    ready_ = true;
    backoffMs_ = kFirstBackoffMs;
    goodIndex_ = serverIndex_;
    pingTimer_->start(kPingIntervalMs);
    watchdogTimer_->start(kWatchdogIntervalMs);
    const bool tipMoved = height != tip_;
    tip_ = height;

    // Signals before the restore: a scanner that subscribes on ready() has its
    // requests queued by then, and restoreSubscriptions() skips those. Any
    // slot may stop or restart the client; the rest is then skipped.
    const quint64 gen = generation_;
    QPointer<ElectrumClient> self(this);
    const auto stillOurs = [&] { return self && gen == generation_; };
    // One arg() call: a "%1" in the server's own text stays as it is.
    emit log(QStringLiteral("connected to %1 (%2), tip %3")
                 .arg(current_.toString(),
                      serverSoftware_.isEmpty() ? QStringLiteral("unknown software") : serverSoftware_,
                      QString::number(height)));
    if (!stillOurs())
        return;
    if (tipMoved) {
        emit tipChanged(height);
        if (!stillOurs())
            return;
    }
    emit ready(serverName());
    if (!stillOurs())
        return;
    restoreSubscriptions();
    pump();
}

void ElectrumClient::restoreSubscriptions()
{
    if (subscriptions_.isEmpty())
        return;
    // Subscribe requests carried over from the lost connection (or queued by a
    // ready() listener) re-subscribe their scripthash anyway; not twice.
    const QHash<QString, int> pending = pendingSubscribes();
    for (auto it = subscriptions_.cbegin(); it != subscriptions_.cend(); ++it)
        if (!pending.contains(it.key()))
            queueRestore(it.key());
}

void ElectrumClient::queueRestore(const QString& scripthash)
{
    Pending p;
    p.id = takeId();
    p.method = QString(kScripthashSubscribe);
    p.params = QJsonArray{scripthash};
    p.internal = true;
    // No caller waits: a status that moved while nobody watched is reported
    // like a notification. stop() drops this request without calling it.
    p.reply = [this, scripthash](const QJsonValue& result, const QString& error) {
        const auto found = subscriptions_.find(scripthash);
        if (found == subscriptions_.end())
            return;
        if (!error.isEmpty()) {
            // Refused; retryRefused() asks again. The first refusal on this
            // connection also has the consumer refresh from the last known
            // status, as the history may have grown while nobody watched it.
            // Only a used address: "" can also mean no server answered yet.
            const QString last = found.value();
            if (noteRefused(scripthash, error) == 1 && !last.isEmpty())
                emit scripthashChanged(scripthash, last);
            return;
        }
        refused_.remove(scripthash);
        const QString status = statusOf(result);
        if (found.value() == status)
            return;
        found.value() = status;
        emit scripthashChanged(scripthash, status);
    };
    queue_.enqueue(std::move(p));
}

int ElectrumClient::noteRefused(const QString& scripthash, const QString& error)
{
    lastRefusal_ = error;
    return ++refused_[scripthash];
}

QHash<QString, int> ElectrumClient::pendingSubscribes() const
{
    QHash<QString, int> pending;
    const auto note = [&pending](const Pending& p) {
        if (p.method == kScripthashSubscribe && !p.params.isEmpty())
            ++pending[p.params.at(0).toString().toLower()];
    };
    for (const Pending& p : queue_)
        note(p);
    for (const Pending& p : inFlight_)
        note(p);
    return pending;
}

void ElectrumClient::retryRefused()
{
    if (!ready_ || !socket_ || (refused_.isEmpty() && refusalSwitches_ == 0))
        return;
    const QHash<QString, int> pending = pendingSubscribes();
    if (refused_.isEmpty()) {
        // Once this server has answered every subscribe, a later refusal may
        // move the client again.
        if (pending.isEmpty())
            refusalSwitches_ = 0;
        return;
    }

    QStringList again;
    qsizetype stuck = 0;
    for (auto it = refused_.cbegin(); it != refused_.cend(); ++it) {
        if (pending.contains(it.key()))
            continue; // asked again already, or a caller's own subscribe is on its way
        if (it.value() <= kRefusedRetries)
            again.append(it.key());
        else
            ++stuck;
    }
    // Refused every time: another server may watch it (ElectrumX limits
    // history size and subscription counts, Blockstream's electrs limits
    // history length, Fulcrum's limits are higher). Each server in the list
    // gets one chance, so a scripthash that every server refuses cannot keep
    // the client hopping.
    if (stuck > 0 && refusalSwitches_ < servers_.size() - 1) {
        ++refusalSwitches_;
        failConnection(QStringLiteral("refused %1 subscription(s) (%2)").arg(QString::number(stuck), lastRefusal_));
        return;
    }
    for (const QString& scripthash : std::as_const(again))
        queueRestore(scripthash);
    pump();
    if (stuck > 0 && !refusalLogged_) {
        refusalLogged_ = true;
        // Last: a log listener may stop or restart the client.
        emit log(QStringLiteral("%1: still refused %2 subscription(s) (%3) and no other server to try; "
                                "asked again after the next reconnect")
                     .arg(current_.toString(), QString::number(stuck), lastRefusal_));
    }
}

void ElectrumClient::failConnection(const QString& reason)
{
    if (!running_ || !socket_)
        return; // stopped, or this connection has already been given up
    const QString what = current_.toString() + QStringLiteral(": ") + reason;
    ready_ = false;
    ++generation_;
    handshakeTimer_->stop();
    pingTimer_->stop();
    watchdogTimer_->stop();

    // Unanswered requests go out again first, in their original order and with
    // their ids, on the next server; each caller still gets exactly one reply.
    QList<Pending> again = takeInFlight();
    for (qsizetype i = again.size(); i-- > 0;)
        queue_.prepend(std::move(again[i]));

    dropSocket();
    resetConnectionState();

    const int delay = backoffMs_;
    backoffMs_ = std::min(backoffMs_ * 2, kMaxBackoffMs);
    reconnectTimer_->start(delay);

    // Signals last, on a consistent state: a slot may stop() or restart us.
    const quint64 gen = generation_;
    QPointer<ElectrumClient> self(this);
    emit log(QStringLiteral("%1; next attempt in %2 s").arg(what, QString::number(delay / 1000)));
    if (!self || gen != generation_)
        return;
    emit connectionLost(what);
}

void ElectrumClient::dropSocket()
{
    if (!socket_)
        return;
    QSslSocket* sock = socket_;
    socket_ = nullptr;
    sock->disconnect(this);
    // No abort() here: this runs inside the socket's own signals (sslErrors,
    // errorOccurred) while Qt is still using it. Deleting it from the event
    // loop closes the connection a moment later.
    sock->deleteLater();
}

void ElectrumClient::resetConnectionState()
{
    buffer_.clear();
    scanFrom_ = 0;
    versionId_ = 0;
    headersId_ = 0;
    pingId_ = 0;
    serverSoftware_.clear();
    // Refusals belong to one server; the next one is asked everything afresh.
    refused_.clear();
    lastRefusal_.clear();
    refusalLogged_ = false;
}

QList<ElectrumClient::Pending> ElectrumClient::takeInFlight()
{
    QList<Pending> sent;
    sent.reserve(inFlight_.size());
    for (auto it = inFlight_.begin(); it != inFlight_.end(); ++it)
        sent.append(std::move(it.value()));
    inFlight_.clear();
    // Ids grow in request order and the queue is always sent from the front
    // (requeued requests go back to the front), so id order is send order.
    std::sort(sent.begin(), sent.end(), [](const Pending& a, const Pending& b) { return a.id < b.id; });
    return sent;
}

// ----------------------------------------------------------- requests ----

int ElectrumClient::takeId()
{
    const int id = nextId_;
    nextId_ = nextId_ == std::numeric_limits<int>::max() ? 1 : nextId_ + 1;
    return id;
}

void ElectrumClient::sendJson(const QJsonObject& message)
{
    if (!socket_)
        return;
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');
    socket_->write(line);
}

void ElectrumClient::pump()
{
    while (ready_ && socket_ && !queue_.isEmpty() && inFlight_.size() < kMaxInFlight) {
        Pending p = queue_.dequeue();
        const int id = p.id;
        const QJsonObject message = makeRequest(id, p.method, p.params);
        p.sentAtMs = monotonicMs();
        inFlight_.emplace(id, std::move(p));
        sendJson(message);
    }
}

void ElectrumClient::sendPing()
{
    if (!ready_ || !socket_ || pingId_ != 0)
        return;
    pingId_ = takeId();
    pingSentAtMs_ = monotonicMs();
    // Sent directly, outside the queue: it must not wait behind a long scan.
    sendJson(makeRequest(pingId_, QStringLiteral("server.ping"), QJsonArray()));
}

void ElectrumClient::checkTimeouts()
{
    if (!ready_ || !socket_)
        return;
    const qint64 now = monotonicMs();
    if (pingId_ != 0 && now - pingSentAtMs_ > kPingTimeoutMs) {
        failConnection(QStringLiteral("no reply to server.ping within %1 s").arg(kPingTimeoutMs / 1000));
        return;
    }
    qint64 oldest = now;
    for (const Pending& p : std::as_const(inFlight_))
        oldest = std::min(oldest, p.sentAtMs);
    if (now - oldest > kRequestTimeoutMs)
        failConnection(QStringLiteral("no reply to a request within %1 s").arg(kRequestTimeoutMs / 1000));
}

// ------------------------------------------------------------ replies ----

void ElectrumClient::onReadyRead()
{
    buffer_.append(socket_->readAll());

    const quint64 gen = generation_;
    QPointer<ElectrumClient> self(this);
    for (;;) {
        const qsizetype newline = buffer_.indexOf('\n', scanFrom_);
        if (newline < 0)
            break;
        if (newline > kMaxLineBytes) {
            failConnection(QStringLiteral("reply larger than 64 MB"));
            return;
        }
        // Each line leaves the buffer before it is handled, so whatever its
        // callbacks do, it is never handled twice.
        const QByteArray line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1); // O(1) at the front of a Qt 6 array
        scanFrom_ = 0;
        if (line.isEmpty())
            continue;
        handleLine(line);
        if (!self || gen != generation_)
            return; // the connection was dropped or replaced meanwhile
    }
    scanFrom_ = buffer_.size(); // the next search starts at the new data only
    if (buffer_.size() > kMaxLineBytes) {
        failConnection(QStringLiteral("reply larger than 64 MB"));
        return;
    }
    pump();
}

void ElectrumClient::handleLine(const QByteArray& line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        failConnection(QStringLiteral("malformed reply (%1)").arg(parseError.errorString()));
        return;
    }
    if (doc.isObject()) {
        handleMessage(doc.object());
        return;
    }
    // A batch reply: an array of reply objects.
    const QJsonArray batch = doc.array();
    if (!doc.isArray()
        || std::any_of(batch.begin(), batch.end(), [](const QJsonValue& v) { return !v.isObject(); })) {
        failConnection(QStringLiteral("malformed reply (not a JSON-RPC object)"));
        return;
    }
    const quint64 gen = generation_;
    QPointer<ElectrumClient> self(this);
    for (const QJsonValue& item : batch) {
        handleMessage(item.toObject());
        if (!self || gen != generation_)
            return;
    }
}

void ElectrumClient::handleMessage(const QJsonObject& message)
{
    const QJsonValue idValue = message.value(QLatin1String("id"));
    if (idValue.isUndefined() || idValue.isNull()) {
        // A notification. (An error without an id answers nothing we sent.)
        const QString method = message.value(QLatin1String("method")).toString();
        if (!method.isEmpty())
            handleNotification(method, message.value(QLatin1String("params")).toArray());
        return;
    }
    if (message.contains(QLatin1String("method")))
        return; // a request from the server: not part of the Electrum protocol

    const int id = idValue.isString() ? idValue.toString().toInt() : idValue.toInt(0);
    if (id <= 0)
        return;
    const QJsonValue errorValue = message.value(QLatin1String("error"));
    const bool failed = !errorValue.isUndefined() && !errorValue.isNull();
    const QString error = failed ? errorText(errorValue) : QString();
    QJsonValue result = failed ? QJsonValue() : message.value(QLatin1String("result"));
    if (result.isUndefined())
        result = QJsonValue();

    if (id == versionId_) {
        handleVersionReply(result, error);
        return;
    }
    if (id == headersId_) {
        handleHeadersReply(result, error);
        return;
    }
    if (id == pingId_) {
        pingId_ = 0; // any answer, even an error, shows the server is alive
        return;
    }
    const auto it = inFlight_.find(id);
    if (it == inFlight_.end())
        return; // not ours, or already answered
    // Out of the table before the callback runs: it may re-enter the client.
    Pending done = std::move(it.value());
    inFlight_.erase(it);
    if (done.reply)
        done.reply(result, error);
}

void ElectrumClient::handleNotification(const QString& method, const QJsonArray& params)
{
    if (method == kScripthashSubscribe) {
        // [scripthash, status or null]
        if (params.isEmpty() || !params.at(0).isString())
            return;
        const QString scripthash = params.at(0).toString().toLower();
        const auto it = subscriptions_.find(scripthash);
        if (it == subscriptions_.end())
            return; // not subscribed here: a server must not grow the list restored on the next one
        const QString status = params.size() > 1 ? statusOf(params.at(1)) : QString();
        it.value() = status;
        emit scripthashChanged(scripthash, status);
    } else if (method == kHeadersSubscribe) {
        // [{height, hex}]
        int height = -1;
        for (const QJsonValue& header : params)
            height = std::max(height, header.toObject().value(QLatin1String("height")).toInt(-1));
        if (height < 0)
            return;
        tip_ = height;
        emit tipChanged(height);
    }
}

} // namespace net
