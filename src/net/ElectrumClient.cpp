#include "net/ElectrumClient.h"

#include "chain/Hash.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QRandomGenerator>
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
// After a session that had proved itself (it answered a ping) drops in a way
// the network may have caused, the same server is asked again for this long
// before the client moves on: long enough for Wi-Fi to return after sleep, or
// for a server to restart.
constexpr int kStickyWindowMs = 180'000;
// A subscription the server refused is asked again on this many ping ticks
// before the client looks for a server that accepts it.
constexpr int kRefusedRetries = 2;
// The longest line accepted. Every reply and notification of the methods this client
// uses is well under 1 KB. Each line is parsed in full on the GUI thread before its
// meaning is known, so this bounds what one line can cost in memory and in time.
constexpr qsizetype kMaxLineBytes = qsizetype(1) * 1024 * 1024;
// While a history is on its way: about 90 bytes per transaction, and servers stop giving
// one well before this. Long lines are for that, and they are rationed: a server that
// sends them one after another is keeping the window busy, and is left.
constexpr qsizetype kMaxHistoryLineBytes = qsizetype(32) * 1024 * 1024;
constexpr qsizetype kRationedLineBytes = qsizetype(64) * 1024;
constexpr qint64 kMaxLongLineBytesPerMinute = qint64(128) * 1024 * 1024;
constexpr int kLongLineWindowMs = 60'000;
constexpr qsizetype kMaxServerText = 300;
// Unsolicited lines (notifications, blank lines) a server may send per window:
// two for every subscription plus the allowance. An honest server notifies when
// a subscribed status changes or a block arrives, far below that; a flood would
// keep the GUI thread busy and is dropped.
constexpr int kUnsolicitedWindowMs = 60'000;
constexpr qsizetype kUnsolicitedAllowance = 1'000;

constexpr QLatin1String kScripthashSubscribe("blockchain.scripthash.subscribe");
constexpr QLatin1String kHeadersSubscribe("blockchain.headers.subscribe");
constexpr QLatin1String kGetHistory("blockchain.scripthash.get_history");

// What a block header in hex hashes to, as explorers write it. Empty if it is no header.
QString headerHash(const QString& hex)
{
    if (hex.size() != 160)
        return {};
    const QByteArray raw = QByteArray::fromHex(hex.toLatin1());
    if (raw.size() != 80)
        return {};
    chain::Bytes hash = chain::dsha256(chain::Bytes(raw.cbegin(), raw.cend()));
    std::reverse(hash.begin(), hash.end());
    return QString::fromStdString(chain::toHex(hash));
}

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
// Subscribe results reach the callbacks only after parseStatus() accepted them
// (handleMessage()), already in that form.
QString statusOf(const QJsonValue& value)
{
    return value.isString() ? value.toString() : QString();
}

// 64 hex digits: the hash of a scripthash's history.
bool isHex64(const QString& text)
{
    return text.size() == 64 && std::all_of(text.cbegin(), text.cend(), [](QChar c) {
               const char16_t u = c.unicode();
               return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
           });
}

// A scripthash status as a server sends it: JSON null (no history) or the
// 64-hex-digit hash of the history. On success *status is "" or that hash in
// lowercase. Anything else (a number, an object, a string of another shape or
// length) is a protocol violation, never "no history": a status is kept for
// every subscription for the whole run, so an unchecked one would also let a
// server fill memory.
bool parseStatus(const QJsonValue& value, QString* status)
{
    if (value.isNull()) {
        status->clear();
        return true;
    }
    if (!value.isString())
        return false;
    const QString text = value.toString();
    if (!isHex64(text))
        return false;
    *status = text.toLower();
    return true;
}

QString tooLongText(qsizetype limit)
{
    return QStringLiteral("reply longer than %1 MB").arg(limit / (1024 * 1024));
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

// ------------------------------------------------------------- client ----

ElectrumClient::ElectrumClient(QObject* parent)
    : QObject(parent)
{
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
    if (sameServers(servers, servers_))
        return;
    servers_ = servers;
    movesAsked_ = 0;
    serverIndex_ = -1; // the next connection starts at the first server
    goodIndex_ = -1;
    stickyUntilMs_ = 0;
    refusalSwitches_ = 0;
    // The controller normally sets servers while stopped; a live connection
    // moves to the new list right away (Server: never back to the old one).
    if (running_ && socket_)
        failConnection(QStringLiteral("server list changed"), Blame::Server);
}

void ElectrumClient::start()
{
    if (running_)
        return;
    running_ = true;
    movesAsked_ = 0;
    backoffMs_ = kFirstBackoffMs;
    stickyUntilMs_ = 0;
    refusalSwitches_ = 0;
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
    return socket_ && (socket_->isEncrypted() || (plainForTest_ && ready_)) ? current_.toString() : QString();
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

void ElectrumClient::unsubscribeScripthash(const QString& scripthash)
{
    const QString key = scripthash.toLower();
    subscriptions_.remove(key);
    refused_.remove(key);
    if (!ready_)
        return; // a new connection starts without it anyway
    letGo_.insert(key);
    // Nobody waits for the answer: an old server's "unknown method" changes nothing.
    request(QStringLiteral("blockchain.scripthash.unsubscribe"), QJsonArray{key}, [](const QJsonValue&, const QString&) {});
}

void ElectrumClient::forgetScripthash(const QString& scripthash)
{
    const QString key = scripthash.toLower();
    subscriptions_.remove(key);
    refused_.remove(key);
}

bool ElectrumClient::tryAnotherServer(const QString& why)
{
    if (moving_)
        return true;
    if (!ready_ || !socket_ || movesAsked_ >= servers_.size() - 1)
        return false;
    ++movesAsked_;
    moving_ = true;
    // From the event loop: the caller is in the middle of one of this connection's replies.
    QTimer::singleShot(0, this, [this, why, gen = generation_] {
        if (gen == generation_)
            failConnection(why, Blame::Server);
    });
    return true;
}

void ElectrumClient::subscribeScripthash(const QString& scripthash, Reply firstStatus)
{
    // One spelling for the list, the request and the notifications, which the
    // servers send in lowercase hex.
    const QString key = scripthash.toLower();
    letGo_.remove(key);
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
        return; // the scanner sets its network's list before it starts this
    const int count = static_cast<int>(servers_.size());
    // The same server again while the window failConnection() opened after a
    // working session lasts; otherwise the next one in the list.
    const bool again = serverIndex_ >= 0 && serverIndex_ < count && monotonicMs() < stickyUntilMs_;
    if (!again)
        serverIndex_ = (serverIndex_ + 1) % count;
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
        if (sock != socket_)
            return;
        handshakeTimer_->start(kHandshakeTimeoutMs); // TCP is up: TLS gets a budget of its own
        if (plainForTest_)
            onEncrypted();
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
    if (plainForTest_)
        sock->connectToHost(current_.host, current_.port);
    else
        sock->connectToHostEncrypted(current_.host, current_.port);
}

void ElectrumClient::onEncrypted()
{
    // Requests are small and pipelined; Nagle would only hold them back.
    socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    handshakeTimer_->start(kHandshakeTimeoutMs);
    versionId_ = takeId();
    // A range: 1.4.2 is where blockchain.scripthash.unsubscribe comes in.
    sendJson(makeRequest(versionId_, QStringLiteral("server.version"),
                         QJsonArray{QStringLiteral("GitWallet 0.2"), QJsonArray{QStringLiteral("1.4"), QStringLiteral("1.4.2")}}));
}

void ElectrumClient::handleVersionReply(const QJsonValue& result, const QString& error)
{
    versionId_ = 0;
    if (!error.isEmpty()) {
        failConnection(QStringLiteral("server.version refused: %1").arg(error), Blame::Server);
        return;
    }
    const QJsonArray version = result.toArray(); // [software, protocol]
    serverSoftware_ = version.isEmpty() ? QString() : clipped(version.at(0).toString());
    if (genesis_.isEmpty()) {
        askHeaders();
        return;
    }
    // Is this server on our chain at all? Another chain's server (a line in Settings,
    // testnet or a fork) has a valid certificate too, and would call every address empty.
    genesisId_ = takeId();
    sendJson(makeRequest(genesisId_, QStringLiteral("blockchain.block.header"), QJsonArray{0}));
}

void ElectrumClient::handleGenesisReply(const QJsonValue& result, const QString& error)
{
    genesisId_ = 0;
    if (!error.isEmpty()) {
        failConnection(QStringLiteral("blockchain.block.header refused: %1").arg(error), Blame::Server);
        return;
    }
    if (headerHash(result.toString()) != genesis_) {
        const QString server = current_.toString();
        failConnection(QStringLiteral("it answers for another chain"), Blame::Server);
        emit wrongChain(server);
        return;
    }
    askHeaders();
}

void ElectrumClient::askHeaders()
{
    headersId_ = takeId();
    sendJson(makeRequest(headersId_, QString(kHeadersSubscribe), QJsonArray()));
}

void ElectrumClient::handleHeadersReply(const QJsonValue& result, const QString& error)
{
    headersId_ = 0;
    if (!error.isEmpty()) {
        failConnection(QStringLiteral("blockchain.headers.subscribe refused: %1").arg(error), Blame::Server);
        return;
    }
    const int height = result.toObject().value(QLatin1String("height")).toInt(-1);
    if (height < 0) {
        failConnection(QStringLiteral("blockchain.headers.subscribe returned no height"), Blame::Server);
        return;
    }
    becomeReady(height);
}

void ElectrumClient::becomeReady(int height)
{
    handshakeTimer_->stop();
    ready_ = true;
    lastHeardAt_ = QDateTime::currentDateTime();
    // The backoff is not reset here but once the session proves stable
    // (noteStable()): a server that completes the handshake and then drops the
    // session would otherwise get a new connection, and every subscription
    // again, each second.
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
    // What it restored by itself, refused every time: another server may watch it
    // (ElectrumX limits history size and subscription counts, Blockstream's electrs
    // limits history length, Fulcrum's limits are higher). Each server in the list gets
    // one chance, so a scripthash that every server refuses cannot keep the client
    // hopping. What a caller subscribed is the caller's to judge: tryAnotherServer().
    if (stuck > 0 && refusalSwitches_ < servers_.size() - 1) {
        ++refusalSwitches_;
        failConnection(QStringLiteral("refused %1 subscription(s) (%2)").arg(QString::number(stuck), lastRefusal_),
                       Blame::Server);
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

void ElectrumClient::failConnection(const QString& reason, Blame blame)
{
    if (!running_ || !socket_)
        return; // stopped, or this connection has already been given up
    const QString what = current_.toString() + QStringLiteral(": ") + reason;
    const bool wasStable = stable_;
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

    // Where the next attempt goes (connectNext()). A server whose session had
    // proved itself is asked again for kStickyWindowMs when that session ends
    // in a way the network may have caused (sleep, Wi-Fi, a server restart):
    // moving on would hand every subscribed scripthash, linked by this IP, to
    // one more operator. Attempts that fail meanwhile keep the window, so an
    // outage shorter than it does not move the client. A server that
    // misbehaved is left at once, and so is one that never held a session
    // long enough to prove itself (no window is opened for it).
    if (blame == Blame::Server)
        stickyUntilMs_ = 0;
    else if (wasStable)
        stickyUntilMs_ = monotonicMs() + kStickyWindowMs;
    const bool sameServer = monotonicMs() < stickyUntilMs_;

    // The backoff doubles on every failure and is reset only by a stable
    // session (noteStable()). Equal jitter: between half and all of it, so
    // clients that lost the same server do not all return in the same second.
    const int delay = backoffMs_ / 2 + QRandomGenerator::global()->bounded(backoffMs_ / 2 + 1);
    backoffMs_ = std::min(backoffMs_ * 2, kMaxBackoffMs);
    reconnectTimer_->start(delay);

    // Signals last, on a consistent state: a slot may stop() or restart us.
    const quint64 gen = generation_;
    QPointer<ElectrumClient> self(this);
    emit log(QStringLiteral("%1; next attempt in %2 s%3")
                 .arg(what, QString::number(delay / 1000.0, 'f', 1),
                      sameServer ? QStringLiteral(", same server") : QString()));
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
    genesisId_ = 0;
    headersId_ = 0;
    pingId_ = 0;
    serverSoftware_.clear();
    stable_ = false;
    moving_ = false;
    letGo_.clear();
    longLineBytes_ = 0;
    longLinesSinceMs_ = monotonicMs();
    lastReceivedMs_ = 0;
    unsolicitedSinceMs_ = monotonicMs();
    unsolicitedCount_ = 0;
    // Refusals belong to one server; the next one is asked everything afresh.
    refused_.clear();
    lastRefusal_.clear();
    refusalLogged_ = false;
}

void ElectrumClient::noteStable()
{
    if (stable_)
        return;
    // A minute after the handshake (the first ping goes out kPingIntervalMs
    // after it) and still answering: a working server. Only now is the backoff
    // reset, and only a stable session has failConnection() ask the same
    // server again first.
    stable_ = true;
    backoffMs_ = kFirstBackoffMs;
}

bool ElectrumClient::noteUnsolicited()
{
    const qint64 now = monotonicMs();
    if (now - unsolicitedSinceMs_ >= kUnsolicitedWindowMs) {
        unsolicitedSinceMs_ = now;
        unsolicitedCount_ = 0;
    }
    const qsizetype budget = 2 * subscriptions_.size() + kUnsolicitedAllowance;
    if (++unsolicitedCount_ <= budget)
        return true;
    failConnection(QStringLiteral("more than %1 notifications or blank lines within %2 s")
                       .arg(QString::number(budget), QString::number(kUnsolicitedWindowMs / 1000)),
                   Blame::Server);
    return false;
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
    // Nothing arrived since the request went out: the network may be down
    // (sleep, Wi-Fi), so the server is not to blame. A server that kept
    // sending while it let the request wait is.
    if (pingId_ != 0 && now - pingSentAtMs_ > kPingTimeoutMs) {
        failConnection(QStringLiteral("no reply to server.ping within %1 s").arg(kPingTimeoutMs / 1000),
                       lastReceivedMs_ > pingSentAtMs_ ? Blame::Server : Blame::Link);
        return;
    }
    qint64 oldest = now;
    for (const Pending& p : std::as_const(inFlight_))
        oldest = std::min(oldest, p.sentAtMs);
    if (now - oldest > kRequestTimeoutMs)
        failConnection(QStringLiteral("no reply to a request within %1 s").arg(kRequestTimeoutMs / 1000),
                       lastReceivedMs_ > oldest ? Blame::Server : Blame::Link);
}

// ------------------------------------------------------------ replies ----

void ElectrumClient::onReadyRead()
{
    const QByteArray data = socket_->readAll();
    if (!data.isEmpty()) {
        lastReceivedMs_ = monotonicMs();
        if (ready_)
            lastHeardAt_ = QDateTime::currentDateTime();
    }
    buffer_.append(data);

    const quint64 gen = generation_;
    QPointer<ElectrumClient> self(this);
    for (;;) {
        const qsizetype newline = buffer_.indexOf('\n', scanFrom_);
        if (newline < 0)
            break;
        const qsizetype limit = newline > kMaxLineBytes ? lineLimit() : kMaxLineBytes;
        if (newline > limit) {
            failConnection(tooLongText(limit), Blame::Server);
            return;
        }
        if (newline > kRationedLineBytes) {
            const qint64 now = monotonicMs();
            if (now - longLinesSinceMs_ >= kLongLineWindowMs) {
                longLinesSinceMs_ = now;
                longLineBytes_ = 0;
            }
            longLineBytes_ += newline;
            if (longLineBytes_ > kMaxLongLineBytesPerMinute) {
                failConnection(QStringLiteral("too many long replies"), Blame::Server);
                return;
            }
        }
        // Each line leaves the buffer before it is handled, so whatever its
        // callbacks do, it is never handled twice.
        const QByteArray line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1); // O(1) at the front of a Qt 6 array
        scanFrom_ = 0;
        if (line.isEmpty()) {
            // Not a message. Counted like a notification, so a stream of
            // newlines cannot keep this loop busy for free.
            if (!noteUnsolicited())
                return;
            continue;
        }
        handleLine(line);
        if (!self || gen != generation_)
            return; // the connection was dropped or replaced meanwhile
    }
    scanFrom_ = buffer_.size(); // the next search starts at the new data only
    if (buffer_.size() > kMaxLineBytes) {
        const qsizetype limit = lineLimit();
        if (buffer_.size() > limit) {
            failConnection(tooLongText(limit), Blame::Server);
            return;
        }
    }
    pump();
}

qsizetype ElectrumClient::lineLimit() const
{
    for (const Pending& p : inFlight_) {
        if (p.method == kGetHistory)
            return kMaxHistoryLineBytes;
    }
    return kMaxLineBytes;
}

void ElectrumClient::handleLine(const QByteArray& line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        failConnection(QStringLiteral("malformed reply (%1)").arg(parseError.errorString()), Blame::Server);
        return;
    }
    // One JSON-RPC object per line. A batch (an array) only ever answers a
    // batch request, and this client sends none.
    if (!doc.isObject()) {
        failConnection(QStringLiteral("malformed reply (not a JSON-RPC object)"), Blame::Server);
        return;
    }
    handleMessage(doc.object());
}

void ElectrumClient::handleMessage(const QJsonObject& message)
{
    // Every message must answer a request of ours or be a notification this
    // client subscribed to. Anything else fails the connection: ignored, junk
    // could be sent forever, each line parsed in full on the GUI thread.
    const QJsonValue idValue = message.value(QLatin1String("id"));
    if (idValue.isUndefined() || idValue.isNull()) {
        const QJsonValue method = message.value(QLatin1String("method"));
        if (method.isString()) {
            handleNotification(method.toString(), message.value(QLatin1String("params")).toArray());
            return;
        }
        // Not a notification. An error without an id answers nothing we sent
        // (JSON-RPC uses it for a request the server could not read).
        const QJsonValue errorValue = message.value(QLatin1String("error"));
        failConnection(errorValue.isUndefined() || errorValue.isNull()
                           ? QStringLiteral("malformed reply (no id)")
                           : QStringLiteral("server error: %1").arg(errorText(errorValue)),
                       Blame::Server);
        return;
    }
    if (message.contains(QLatin1String("method"))) {
        // A request from the server: not part of the Electrum protocol.
        failConnection(QStringLiteral("malformed reply (a request from the server)"), Blame::Server);
        return;
    }

    const int id = idValue.isString() ? idValue.toString().toInt() : idValue.toInt(0);
    if (id <= 0) {
        failConnection(QStringLiteral("malformed reply (bad id)"), Blame::Server);
        return;
    }
    const QJsonValue errorValue = message.value(QLatin1String("error"));
    const bool failed = !errorValue.isUndefined() && !errorValue.isNull();
    const QString error = failed ? errorText(errorValue) : QString();
    QJsonValue result = failed ? QJsonValue() : message.value(QLatin1String("result"));

    if (id == pingId_) {
        pingId_ = 0; // any answer, even an error, shows the server is alive
        noteStable();
        return;
    }
    // A reply carries "result" or "error". Read as null, a missing result
    // would say "no history" for a subscription.
    if (result.isUndefined()) {
        failConnection(QStringLiteral("malformed reply (no result)"), Blame::Server);
        return;
    }
    if (id == versionId_) {
        handleVersionReply(result, error);
        return;
    }
    if (id == genesisId_) {
        handleGenesisReply(result, error);
        return;
    }
    if (id == headersId_) {
        handleHeadersReply(result, error);
        return;
    }
    const auto it = inFlight_.find(id);
    if (it == inFlight_.end()) {
        // Every id on the wire is in flight until answered: this answers
        // nothing, or answers a request twice.
        failConnection(QStringLiteral("malformed reply (answers no request)"), Blame::Server);
        return;
    }
    if (!failed && it->method == kScripthashSubscribe) {
        QString status;
        if (!parseStatus(result, &status)) {
            // Left in flight: failConnection() sends it again on the next
            // server, so its caller still gets exactly one, valid, reply.
            failConnection(QStringLiteral("malformed scripthash status"), Blame::Server);
            return;
        }
        result = status.isEmpty() ? QJsonValue() : QJsonValue(status);
    }
    // Out of the table before the callback runs: it may re-enter the client.
    Pending done = std::move(it.value());
    inFlight_.erase(it);
    if (done.reply)
        done.reply(result, error);
}

void ElectrumClient::handleNotification(const QString& method, const QJsonArray& params)
{
    if (!noteUnsolicited())
        return;
    if (method == kScripthashSubscribe) {
        // [scripthash, status or null]
        QString status;
        if (params.size() < 2 || !params.at(0).isString() || !parseStatus(params.at(1), &status)) {
            failConnection(QStringLiteral("malformed scripthash notification"), Blame::Server);
            return;
        }
        const QString scripthash = params.at(0).toString().toLower();
        const auto it = subscriptions_.find(scripthash);
        if (it == subscriptions_.end()) {
            // Let go on this connection: a server too old for that goes on telling, and
            // one that knows the call may have sent this before it read it.
            if (letGo_.contains(scripthash))
                return;
            // Never subscribed: a server must not grow the list restored on the next one.
            failConnection(QStringLiteral("notification for a scripthash never subscribed"), Blame::Server);
            return;
        }
        it.value() = status;
        emit scripthashChanged(scripthash, status);
    } else if (method == kHeadersSubscribe) {
        // [{height, hex}]
        int height = -1;
        for (const QJsonValue& header : params)
            height = std::max(height, header.toObject().value(QLatin1String("height")).toInt(-1));
        if (height < 0) {
            failConnection(QStringLiteral("malformed headers notification"), Blame::Server);
            return;
        }
        tip_ = height;
        emit tipChanged(height);
    } else {
        // Protocol 1.4 has no other notification, and nothing else was subscribed.
        failConnection(QStringLiteral("unexpected notification %1").arg(clipped(method)), Blame::Server);
    }
}

} // namespace net
