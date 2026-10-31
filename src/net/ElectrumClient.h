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
// fails (connect error, TLS error, timeout, disconnect). Requests are pipelined
// (at most kMaxInFlight unanswered at once); requests that were pending when
// the connection dropped are re-sent on the next server, and every scripthash
// subscription is restored. Pings the server every 60 s. GUI thread only.
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
    // (JSON null = no history; otherwise a string hash of the history). Later
    // changes arrive through scripthashChanged(). Restored after a reconnect, and
    // a status that changed while disconnected is reported through
    // scripthashChanged() as well.
    void subscribeScripthash(const QString& scripthash, Reply firstStatus);

signals:
    void ready(const QString& server);
    void connectionLost(const QString& reason);
    void scripthashChanged(const QString& scripthash, const QString& status);
    void tipChanged(int height);
    void log(const QString& line); // diagnostics; never carries keys or seeds

private:
    struct Pending {
        int id = 0;
        QString method;
        QJsonArray params;
        Reply reply;
    };

    // --- implementation (ElectrumClient.cpp may add private members) ---
    void connectNext();
    void sendJson(const QJsonObject& message);
    void pump();
    void handleMessage(const QJsonObject& message);
    void failConnection(const QString& reason);

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
    QHash<QString, QString> subscriptions_; // scripthash -> last known status ("" = null)
};

} // namespace net
