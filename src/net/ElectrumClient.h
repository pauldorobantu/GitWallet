#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QSet>
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

// Electrum protocol client: JSON-RPC 2.0 over TLS, one JSON message per line, for
// one network: the list it is given says which. The built-in lists hold public
// servers whose TLS certificate verifies against the Windows CA store. Self-signed
// servers are deliberately not used: GitWallet always verifies TLS.
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

    void setServers(const QList<ElectrumServer>& servers);
    // The hash of the network's first block, as block explorers show it. A server that
    // answers with another one is on another chain: it is left, and wrongChain() says so.
    void setGenesis(const QString& hash) { genesis_ = hash; }
    // Plain TCP, for a test's server on this PC. Everything else always verifies TLS.
    void usePlainTcpForTest() { plainForTest_ = true; }
    void start(); // connect (async); ready() after the server.version handshake
    void stop();  // disconnect; pending replies get the error "stopped"

    bool isReady() const { return ready_; }
    // A server is telling of this address as it moves: what it last said is as good as what it would
    // say now. One read in rounds says what it was when it was read.
    bool isFollowing(const QString& scripthash) const { return subscriptions_.contains(scripthash) && !refused_.contains(scripthash); }
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
    // Lets one go. A server too old to know the call goes on sending what changes,
    // which is ignored; it costs a place there until the connection ends. So does a
    // notification already on its way.
    void unsubscribeScripthash(const QString& scripthash);
    // One the server refused: nothing to let go there, and no new connection is to ask for it.
    void forgetScripthash(const QString& scripthash);

    // This server will not do what it is asked: the next on the list gets the chance, each
    // of them once, so what no server will do cannot keep the client hopping. False when
    // none is left, and the client stays where it is.
    bool tryAnotherServer(const QString& why);
    // It does what it is asked: the next difficulty may move the client again.
    void noteServerWorks() { movesAsked_ = 0; }
    int serverCount() const { return int(servers_.size()); }
    // When a server that could say anything of an address was last heard from, on the wall
    // clock: a sleep counts, and a handshake with one on the way out does not.
    QDateTime lastHeardAt() const { return lastHeardAt_; }

signals:
    void ready(const QString& server);
    void connectionLost(const QString& reason);
    void scripthashChanged(const QString& scripthash, const QString& status); // "" = no history
    void wrongChain(const QString& server);
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
    void handleGenesisReply(const QJsonValue& result, const QString& error);
    void handleHeadersReply(const QJsonValue& result, const QString& error);
    void askHeaders();
    qsizetype lineLimit() const; // the longest line that may come now
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
    QSet<QString> letGo_;             // unsubscribed on this connection: what still comes for them is ignored
    QString genesis_;                 // what the first block's hash must be; empty: not checked
    bool plainForTest_ = false;
    qint64 longLinesSinceMs_ = 0;     // start of the window longLineBytes_ counts in
    qint64 longLineBytes_ = 0;        // bytes of long lines in that window
    QDateTime lastHeardAt_;           // see lastHeardAt()
    int movesAsked_ = 0;              // tryAnotherServer() since a server last did all it was asked
    bool moving_ = false;             // this connection is about to be left for that

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
    int genesisId_ = 0;
    int headersId_ = 0;
    int pingId_ = 0;
    qint64 pingSentAtMs_ = 0;
    QHash<QString, int> refused_; // scripthash -> subscribes this server refused; asked again on the ping tick
    QString lastRefusal_;         // the newest refusal text, for the log
    int refusalSwitches_ = 0;     // server switches forced by refusals since a server last watched everything
    bool refusalLogged_ = false;  // "still refused, staying" already logged on this connection
};

} // namespace net
