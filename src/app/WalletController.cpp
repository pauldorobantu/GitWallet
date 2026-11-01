#include "app/WalletController.h"

#include "chain/Bip39.h"
#include "chain/Hash.h"
#include "net/ElectrumClient.h"
#include "net/EthRpc.h"
#include "net/PriceFeed.h"
#include "wallet/BtcScanner.h"
#include "wallet/EthScanner.h"
#include "wallet/WalletModel.h"

#include <QLocale>
#include <QLoggingCategory>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <exception>
#include <string_view>

namespace app {

namespace {

// Server rotation, endpoint failover and price refresh failures. Never carries
// a seed or a key: only server names, addresses and counts reach it.
Q_STATIC_LOGGING_CATEGORY(lcNet, "gitwallet.net")

constexpr int kMinGap = 20;
constexpr int kMaxGap = 5000;
constexpr int kMinRefreshSeconds = 15;
constexpr int kMaxRefreshSeconds = 3600;
// WalletStore's limits for a server list (it drops what exceeds them when it
// writes wallet.dat); in practice a list holds a handful of entries.
constexpr qsizetype kMaxListEntries = 100;
constexpr qsizetype kMaxEntryChars = 512;
constexpr std::chrono::minutes kPriceInterval{5};
// Scanners report progress once per address (thousands in a row on a shop
// seed); the title chip needs only a few updates a second.
constexpr std::chrono::milliseconds kStatusThrottle{150};

void setError(QString* error, const QString& message)
{
    if (error)
        *error = message;
}

QString withReason(const QString& what, const QString& why)
{
    const QString reason = why.trimmed();
    return reason.isEmpty() ? what + QLatin1Char('.') : what + QStringLiteral(": ") + reason;
}

// WalletStore already words its failures as full sentences naming the file.
QString storeMessage(const QString& why, const QString& fallback)
{
    return why.trimmed().isEmpty() ? fallback : why;
}

bool hasAccounts(const QList<wallet::AccountKey>& accounts, bool bitcoin)
{
    return std::any_of(accounts.cbegin(), accounts.cend(), [bitcoin](const wallet::AccountKey& a) {
        return chain::isBitcoin(a.type) == bitcoin;
    });
}

// Server lists cleaned the way WalletStore cleans them before writing: each
// entry trimmed; blank, over-long and case-insensitively repeated entries
// dropped; at most kMaxListEntries kept. The clients then use, and the
// Settings page shows after Save, exactly what wallet.dat holds. An entry is
// never split or rewritten: "host : 50002" is one valid server to
// ElectrumServer::fromString().
QStringList cleanLines(const QStringList& lines)
{
    QStringList out;
    QSet<QString> seen; // case-folded entries
    for (const QString& line : lines) {
        if (out.size() >= kMaxListEntries)
            break;
        const QString entry = line.trimmed();
        if (entry.isEmpty() || entry.size() > kMaxEntryChars)
            continue;
        const QString key = entry.toCaseFolded();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        out.append(entry);
    }
    return out;
}

// Also applied to settings read from disk, so a file written by another
// version can never hand the scanners a gap of 0 or a million.
wallet::ScanSettings sanitized(wallet::ScanSettings s)
{
    s.btcReceiveGap = std::clamp(s.btcReceiveGap, kMinGap, kMaxGap);
    s.btcChangeGap = std::clamp(s.btcChangeGap, kMinGap, kMaxGap);
    s.ethGap = std::clamp(s.ethGap, kMinGap, kMaxGap);
    s.ethRefreshSeconds = std::clamp(s.ethRefreshSeconds, kMinRefreshSeconds, kMaxRefreshSeconds);
    s.electrumServers = cleanLines(s.electrumServers);
    s.ethEndpoints = cleanLines(s.ethEndpoints);
    return s;
}

QString grouped(int n)
{
    // The UI is English-first: "12,345" whatever the Windows number format.
    static const QLocale english(QLocale::English, QLocale::UnitedStates);
    return english.toString(n);
}

QString addressCount(int n)
{
    return grouped(n) + (n == 1 ? QStringLiteral(" address") : QStringLiteral(" addresses"));
}

// chain::bip39::validate() answers in terse developer English. The user gets a
// sentence that names the unknown word's position, never the word itself, so
// no part of a seed can reach a label, a toast or the log.
QString mnemonicProblem(const std::vector<std::string>& words, const std::string& why)
{
    const size_t n = words.size();
    if (n == 0)
        return QStringLiteral("Enter the words of your recovery phrase.");
    if (n != 12 && n != 15 && n != 18 && n != 21 && n != 24)
        return QStringLiteral("The phrase has %1 %2; a BIP39 phrase has 12, 15, 18, 21 or 24 words.")
            .arg(QString::number(n), n == 1 ? QStringLiteral("word") : QStringLiteral("words"));

    if (why.find("not in the BIP39 list") != std::string::npos) {
        for (size_t i = 0; i < n; ++i) {
            if (!chain::bip39::isWord(words[i]))
                return QStringLiteral("Word %1 is not in the BIP39 word list.")
                    .arg(static_cast<int>(i + 1));
        }
        return QStringLiteral("A word is not in the BIP39 word list.");
    }
    if (why.find("checksum") != std::string::npos)
        return QStringLiteral("The checksum does not match: a word is wrong or out of order.");
    if (why.find("wordlist") != std::string::npos)
        return QStringLiteral("The BIP39 word list is missing from this build.");
    return QStringLiteral("This is not a valid BIP39 recovery phrase.");
}

} // namespace

WalletController::WalletController(QObject* parent) : QObject(parent)
{
    // QObject deletes children in creation order. The clients go first, so a
    // client that fails its pending requests while being destroyed still finds
    // the scanners, and the model they write to, alive.
    electrum_ = new net::ElectrumClient(this);
    eth_ = new net::EthRpc(this);
    prices_ = new net::PriceFeed(this);
    model_ = new wallet::WalletModel(this);
    btcScanner_ = new wallet::BtcScanner(electrum_, model_, this);
    ethScanner_ = new wallet::EthScanner(eth_, model_, this);

    priceTimer_ = new QTimer(this);
    priceTimer_->setInterval(kPriceInterval);
    connect(priceTimer_, &QTimer::timeout, prices_, &net::PriceFeed::refresh);

    statusTimer_ = new QTimer(this);
    statusTimer_->setSingleShot(true);
    statusTimer_->setInterval(kStatusThrottle);
    connect(statusTimer_, &QTimer::timeout, this, &WalletController::statusChanged);

    connect(model_, &wallet::WalletModel::totalsChanged, this, &WalletController::totalsChanged);
    // statusText() counts funded addresses, and an Ethereum refresh pass that
    // only moves balances reports no progress. Any change of that count moves
    // some total, so balance changes queue a status update as well.
    connect(model_, &wallet::WalletModel::totalsChanged, this, &WalletController::queueStatus);
    connect(prices_, &net::PriceFeed::updated, this, &WalletController::totalsChanged);
    connect(prices_, &net::PriceFeed::failed, this, [](const QString& reason) {
        qCInfo(lcNet).noquote() << "price refresh failed, keeping the last prices:" << reason;
    });

    // The Settings page shows the server / endpoint in use, so a switch counts
    // as a status change too.
    connect(electrum_, &net::ElectrumClient::ready, this, &WalletController::statusChanged);
    connect(electrum_, &net::ElectrumClient::connectionLost, this, &WalletController::statusChanged);
    connect(eth_, &net::EthRpc::endpointChanged, this, &WalletController::statusChanged);
    // Nobody else can reach the clients, so their diagnostics end up here.
    connect(electrum_, &net::ElectrumClient::log, this,
            [](const QString& line) { qCDebug(lcNet).noquote() << "[electrum]" << line; });
    connect(eth_, &net::EthRpc::log, this,
            [](const QString& line) { qCDebug(lcNet).noquote() << "[eth]" << line; });

    connect(btcScanner_, &wallet::BtcScanner::progress, this, &WalletController::queueStatus);
    connect(ethScanner_, &wallet::EthScanner::progress, this, &WalletController::queueStatus);
    connect(btcScanner_, &wallet::BtcScanner::scanFinished, this, [this] { onScanFinished(&btcDone_); });
    connect(ethScanner_, &wallet::EthScanner::scanFinished, this, [this] { onScanFinished(&ethDone_); });
    connect(btcScanner_, &wallet::BtcScanner::error, this, &WalletController::onScannerError);
    connect(ethScanner_, &wallet::EthScanner::error, this, &WalletController::onScannerError);
}

WalletController::~WalletController()
{
    // The owning window deletes its children from its Qt base-class
    // destructor, after its own destructor has run, so a slot reached from
    // here would work on a half-destroyed window (the pages are still alive
    // and connected): shut down without telling anyone. Qt still emits
    // destroyed().
    blockSignals(true);
    stopServices();
}

bool WalletController::hasWallet() const
{
    return !store_.accounts.isEmpty();
}

bool WalletController::loadExisting(QString* error)
{
    setError(error, QString());
    if (!store_.exists()) {
        fileLoaded_ = false;
        return false;
    }

    QString why;
    if (!store_.load(&why)) {
        // The file stays as it is: settings alone never replace it (an import
        // does). It may still open later, for the right Windows user.
        fileLoaded_ = false;
        setError(error, storeMessage(why, QStringLiteral("The wallet file could not be read.")));
        return false;
    }
    fileLoaded_ = true;
    store_.settings = sanitized(store_.settings);

    // Nothing runs yet at startup; a second call starts again from a clean slate.
    stopServices();
    model_->clear();
    startServices();
    emit walletChanged();
    emit statusChanged();
    // Settings saved before the first import make a file without accounts:
    // that is still "no wallet yet" (false, empty error), with the settings
    // loaded for the import.
    return hasWallet();
}

bool WalletController::importMnemonic(const std::vector<std::string>& words,
                                      const std::string& passphrase, QString* error)
{
    setError(error, QString());

    // `words` and `passphrase` are only ever passed on by reference: no copy is
    // made here. `why` can quote a mistyped seed word, so it is scrubbed.
    std::string why;
    const bool valid = chain::bip39::validate(words, &why);
    if (!valid)
        setError(error, mnemonicProblem(words, why));
    chain::secureClear(why);
    if (!valid)
        return false;

    QList<wallet::AccountKey> accounts;
    try {
        accounts = wallet::WalletStore::deriveAccounts(words, passphrase);
    } catch (const std::exception& e) {
        setError(error, withReason(QStringLiteral("Could not derive the wallet keys"),
                                   QString::fromUtf8(e.what())));
        return false;
    }
    if (accounts.isEmpty()) {
        setError(error, QStringLiteral("No account keys could be derived from this phrase."));
        return false;
    }

    // Save before touching anything else: on failure the wallet shown so far
    // (if any) keeps running as it was.
    const QList<wallet::AccountKey> previous = store_.accounts;
    store_.accounts = accounts;
    QString saveError;
    if (!store_.save(&saveError)) {
        store_.accounts = previous;
        setError(error, storeMessage(saveError, QStringLiteral("The wallet could not be saved.")));
        return false;
    }
    fileLoaded_ = true;

    stopServices();
    model_->clear();
    startServices();
    emit walletChanged();
    emit statusChanged();
    return true;
}

bool WalletController::removeWallet(QString* error)
{
    setError(error, QString());
    stopServices();

    QString why;
    if (!store_.remove(&why)) {
        setError(error, storeMessage(why, QStringLiteral("The wallet file could not be deleted.")));
        if (hasWallet()) {
            // Still on disk and still loaded: keep showing and watching it.
            startServices();
            emit statusChanged();
            return false;
        }
        // The store forgot the wallet although the file stayed: show no wallet,
        // and never write settings alone over the keys still in that file.
        fileLoaded_ = false;
        model_->clear();
        emit walletChanged();
        emit statusChanged();
        return false;
    }

    fileLoaded_ = false; // no file any more
    model_->clear();
    emit walletChanged();
    emit statusChanged();
    return true;
}

bool WalletController::applySettings(const wallet::ScanSettings& settings, QString* error)
{
    setError(error, QString());
    const wallet::ScanSettings previous = store_.settings;
    store_.settings = sanitized(settings);

    // Saved with the account keys, or on their own before the first import, so
    // that a private server set up first is still the one the import talks to
    // after a restart (loadExisting() reads a file without accounts as "no
    // wallet yet"). Settings alone never replace a wallet.dat this session did
    // not load or write: an unreadable one may still open for the right
    // Windows user, and only an import replaces it.
    bool saved = false;
    if (fileLoaded_ || !store_.exists()) {
        QString why;
        if (!store_.save(&why)) {
            store_.settings = previous;
            setError(error, storeMessage(why, QStringLiteral("The settings could not be saved.")));
            return false;
        }
        fileLoaded_ = true;
        saved = true;
    } else {
        // In memory only, used by an import in this session. Shown in a toast
        // that elides long text, so the outcome comes first.
        setError(error, QStringLiteral("Applied for this session only: wallet.dat was not opened."));
    }

    // The model keeps its rows: the restarted scanners reuse them instead of
    // rescanning from index 0.
    stopServices();
    startServices();
    emit statusChanged();
    return saved;
}

void WalletController::refresh()
{
    prices_->refresh();
    if (priceTimer_->isActive())
        priceTimer_->start(); // the next automatic refresh is a full interval away
    if (running_)
        ethScanner_->refreshNow();
}

bool WalletController::isScanning() const
{
    return btcScanner_->isScanning() || ethScanner_->isScanning();
}

QString WalletController::statusText() const
{
    if (!hasWallet())
        return QStringLiteral("No wallet");
    if (hasAccounts(store_.accounts, true) && !electrum_->isReady())
        return QStringLiteral("Connecting…");
    if (isScanning())
        return QStringLiteral("Scanning · %1").arg(addressCount(model_->size()));
    return QStringLiteral("Up to date · %1 · %2 funded")
        .arg(addressCount(model_->size()), grouped(model_->fundedCount()));
}

QString WalletController::electrumServer() const
{
    return electrum_->serverName();
}

QString WalletController::ethEndpoint() const
{
    return eth_->currentEndpoint();
}

double WalletController::usdValue(wallet::Asset asset) const
{
    const wallet::AssetInfo& info = wallet::assetInfo(asset);
    return model_->total(asset).toDouble(info.decimals) * prices_->usd(info.symbol);
}

double WalletController::totalUsd() const
{
    double sum = 0.0;
    for (const wallet::AssetInfo& info : wallet::assets())
        sum += usdValue(info.asset);
    return sum;
}

void WalletController::startServices()
{
    const bool hasBtc = hasAccounts(store_.accounts, true);
    const bool hasEth = hasAccounts(store_.accounts, false);
    // Set before any scanner starts, in case one finishes synchronously. A
    // chain without accounts has nothing to scan and counts as finished.
    btcDone_ = !hasBtc;
    ethDone_ = !hasEth;
    scanFinishedSent_ = false;
    if (!hasBtc && !hasEth)
        return; // no wallet: no connections at all

    running_ = true;
    // A copy: signals emitted below may re-enter the controller.
    const wallet::ScanSettings settings = store_.settings;

    btcScanner_->setAccounts(store_.accounts);
    btcScanner_->setGaps(settings.btcReceiveGap, settings.btcChangeGap);
    bool noValidServer = false;
    if (hasBtc) {
        QList<net::ElectrumServer> servers;
        int lines = 0;
        for (const QString& line : settings.electrumServers) {
            const QString text = line.trimmed();
            if (text.isEmpty())
                continue;
            ++lines;
            net::ElectrumServer server;
            if (net::ElectrumServer::fromString(text, &server))
                servers.append(server);
            else
                qCInfo(lcNet).noquote() << "ignoring Electrum server" << text << "(expected host:port)";
        }
        noValidServer = lines > 0 && servers.isEmpty();
        electrum_->setServers(servers); // empty = net::defaultElectrumServers()
        // The scanner is set up first so it is listening when ready() arrives.
        btcScanner_->start();
        electrum_->start();
    }

    eth_->setEndpoints(settings.ethEndpoints); // empty = net::defaultEthEndpoints()
    ethScanner_->setAccounts(store_.accounts);
    ethScanner_->setGap(settings.ethGap);
    ethScanner_->setRefreshSeconds(settings.ethRefreshSeconds);
    if (hasEth)
        ethScanner_->start();

    prices_->refresh();
    priceTimer_->start();

    if (noValidServer) {
        // Posted, so it lands after the caller's own feedback ("Settings saved")
        // instead of being replaced by it.
        QTimer::singleShot(0, this, [this] {
            if (running_)
                emit errorOccurred(QStringLiteral("None of the Electrum servers in Settings is "
                                                  "valid (host:port); using the built-in list."));
        });
    }
}

void WalletController::stopServices()
{
    running_ = false;
    // Scanners first: when the client then fails its pending requests with
    // "stopped", nobody acts on those replies any more.
    btcScanner_->stop();
    ethScanner_->stop();
    electrum_->stop();
    priceTimer_->stop();
}

void WalletController::queueStatus()
{
    // A throttle, not a debounce: during a long scan the chip still moves.
    if (!statusTimer_->isActive())
        statusTimer_->start();
}

void WalletController::onScanFinished(bool* chainDone)
{
    if (!running_)
        return; // a late signal from a run that stopServices() ended
    *chainDone = true;
    emit statusChanged();
    if (btcDone_ && ethDone_ && !scanFinishedSent_) {
        scanFinishedSent_ = true; // before emitting: a listener may restart the services
        emit scanFinished();
    }
}

void WalletController::onScannerError(const QString& message)
{
    // While stopping, the clients fail every pending request ("stopped"):
    // that is the shutdown itself, not something to show the user.
    if (!running_ || message.trimmed().isEmpty())
        return;
    emit errorOccurred(message);
}

} // namespace app
