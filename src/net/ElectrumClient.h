#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QString>

#include <functional>

class QSslSocket;
class QTimer;

namespace net {

struct ElectrumServer {
    QString host;
    quint16 port = 50002; // TLS

    QString toString() const { return host + QLatin1Char(':') + QString::number(port); }
    // Parses "host:port" (port required). False on bad input.
    static bool fromString(const QString& text, ElectrumServer* out);
};

// Public servers whose TLS certificate verifies against the Windows CA store
// and that answered Electrum protocol 1.4 with batch support in a probe on
// 2026-10-31. Self-signed servers are deliberately not used: GitWallet always
// verifies TLS.
QList<ElectrumServer> defaultElectrumServers();

// Electrum protocol client: JSON-RPC 2.0 over TLS, one JSON message per line.
// Talks to one server at a time and rotates through the list when a server
// fails (connect error, TLS error, timeout, disconnect, a malformed or
// unexpected message). A server whose session had proved itself is asked again
// first for a while when that session drops, since the network may have caused
// it (sleep, Wi-Fi): moving on would give one more operator every subscribed
// scripthash. Requests are pipelined (at most kMaxInFlight unanswered at once);
// requests that were pending when the connection dropped are re-sent on the
// next server, and every scripthash subscription is restored. Pings the server
// every 60 s. GUI thread only.
class ElectrumClient : public QObject
{
    Q_OBJECT
public:
    using Reply = std::function<void(const QJsonValue& result, const QString& error)>;
    static constexpr int kMaxInFlight = 64;

    explicit ElectrumClient(QObject* parent = nullptr);
    ~ElectrumClient() override;

    void setServers(const QList<ElectrumServer>& servers); // empty = defaultElectrumServers()
    void start(); // connect (async); ready() after the server.version handshake
    void stop();  // disconnect; pending replies get the error "stopped"

    bool isReady() const { return ready_; }
    QString serverName() const; // "host:port" of the connected server, empty if none
    int tipHeight() const { return tip_; }

    // Queues a request; sent at once when ready, otherwise after (re)connection.
    // `reply` runs exactly once — with the result, or with an error text if the
    // server answered with an error or the request was dropped by stop().
    void request(const QString& method, const QJsonArray& params, Reply reply);

    // blockchain.scripthash.subscribe. `firstStatus` receives the current status
    // (JSON null = no history; otherwise the 64-hex-digit hash of the history,
    // in lowercase: a server that sends anything else is dropped and the request
    // goes to the next one). Later changes arrive through scripthashChanged().
    // Restored after a reconnect, and a status that changed while disconnected
    // is reported through scripthashChanged() as well.
    void subscribeScripthash(const QString& scripthash, Reply firstStatus);

signals:
    void ready(const QString& server);
    void connectionLost(const QString& reason);
    void scripthashChanged(const QString& scripthash, const QString& status); // "" = no history
    void tipChanged(int height);
    void log(const QString& line); // diagnostics; never carries keys or seeds

private:
    struct Pending {
        int id = 0;
        QString method;
        QJsonArray params;
        Reply reply;
        qint64 sentAtMs = 0; // monotonic time it was written (meaningful while in flight)
        bool internal = false; // a re-subscription of the client's own: nobody waits for its reply
    };

    // Why a connection is given up. Link: the network may have caused it, so a
    // server whose session had proved itself is asked again first. Server: the
    // server misbehaved (or must be left anyway); the next attempt moves on.
    enum class Blame { Link, Server };

    // --- implementation (ElectrumClient.cpp may add private members) ---
    void connectNext();
    void sendJson(const QJsonObject& message);
    void pump();
    void handleMessage(const QJsonObject& message);
    void failConnection(const QString& reason, Blame blame = Blame::Link);

    int takeId();
    void onEncrypted();
    void onReadyRead();
    void handleLine(const QByteArray& line);
    void handleNotification(const QString& method, const QJsonArray& params);
    void handleVersionReply(const QJsonValue& result, const QString& error);
    void handleHeadersReply(const QJsonValue& result, const QString& error);
    void becomeReady(int height);
    void restoreSubscriptions();
    void sendPing();
    void checkTimeouts();
    void dropSocket();
    void resetConnectionState();
    QList<Pending> takeInFlight(); // empties inFlight_, in the order the requests were sent
    QHash<QString, int> pendingSubscribes() const; // scripthash -> subscribes queued or in flight
    void queueRestore(const QString& scripthash);   // internal blockchain.scripthash.subscribe
    int noteRefused(const QString& scripthash, const QString& error); // -> refusals of it on this connection
    void retryRefused(); // on the ping tick
    void noteStable();      // the session answered a ping
    bool noteUnsolicited(); // a notification or a blank line; false = one too many, the connection was failed

    QList<ElectrumServer> servers_;
    int serverIndex_ = -1;
    QSslSocket* socket_ = nullptr;
    QTimer* pingTimer_ = nullptr;
    QTimer* reconnectTimer_ = nullptr;
    QTimer* handshakeTimer_ = nullptr;
    QByteArray buffer_;
    bool running_ = false;
    bool ready_ = false;
    int nextId_ = 1;
    int tip_ = 0;
    QQueue<Pending> queue_;                 // not yet sent
    QHash<int, Pending> inFlight_;          // sent, awaiting a reply
    QHash<QString, QString> subscriptions_; // lowercase scripthash -> last known status ("" = null)

    QTimer* watchdogTimer_ = nullptr; // request + ping timeouts, every 5 s while ready
    ElectrumServer current_;          // the server socket_ talks to
    QString serverSoftware_;          // from the server.version reply, for the log
    qsizetype scanFrom_ = 0;          // bytes of buffer_ already searched for a newline
    quint64 generation_ = 0;          // bumped whenever the connection is torn down or replaced
    int backoffMs_ = 1000;            // delay before the next reconnect (jittered); reset by a stable session
    int goodIndex_ = -1;              // last server that completed a handshake
    bool stable_ = false;             // this session answered a ping: the server works
    qint64 stickyUntilMs_ = 0;        // until then, reconnects go to the same server
    qint64 lastReceivedMs_ = 0;       // monotonic time data last arrived on this connection
    qint64 unsolicitedSinceMs_ = 0;   // start of the window unsolicitedCount_ counts in
    qsizetype unsolicitedCount_ = 0;  // notifications and blank lines in that window
    int versionId_ = 0;               // ids of the internal requests in flight; 0 = none
    int headersId_ = 0;
    int pingId_ = 0;
    qint64 pingSentAtMs_ = 0;
    QHash<QString, int> refused_; // scripthash -> subscribes this server refused; asked again on the ping tick
    QString lastRefusal_;         // the newest refusal text, for the log
    int refusalSwitches_ = 0;     // server switches forced by refusals since a server last watched everything
    bool refusalLogged_ = false;  // "still refused, staying" already logged on this connection
};

} // namespace net
