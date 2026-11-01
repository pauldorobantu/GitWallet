#include "app/WalletController.h"

#include "chain/Bip39.h"
#include "chain/Hash.h"
#include "net/ElectrumClient.h"
#include "net/EthRpc.h"
#include "net/PriceFeed.h"
#include "wallet/BtcScanner.h"
#include "wallet/EthScanner.h"
#include "wallet/WalletModel.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QLockFile>
#include <QLoggingCategory>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
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
// Far above any wallet.dat GitWallet writes (WalletStore stops at 1 MiB): a
// larger file is not one this session loaded or wrote, and is not hashed.
constexpr qint64 kMaxDigestBytes = qint64(16) << 20;

// Beside wallet.dat, so every --data-dir folder locks on its own.
QString lockFilePath()
{
    return wallet::WalletStore::dataDir() + QStringLiteral("/wallet.lock");
}

// SHA-256 of wallet.dat as it is on disk now: an empty array when there is no
// file, nullopt when it is there but cannot be read (or is far too large to be
// a wallet file). The bytes are DPAPI ciphertext; only their digest is kept.
std::optional<QByteArray> diskDigest()
{
    const QString path = wallet::WalletStore::filePath();
    if (!QFileInfo(path).isFile()) // the test WalletStore::exists() makes
        return QByteArray();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxDigestBytes)
        return std::nullopt;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return std::nullopt;
    return hash.result();
}

// Why a write is refused in a second GitWallet on the same data folder.
QString inUseElsewhere()
{
    return QStringLiteral("wallet.dat is in use by another GitWallet window.");
}

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

// Local time: the time alone while it is today, with the date once it is not
// (a session runs for days, and "23:58" alone would read as today).
QString clockTime(const QDateTime& when)
{
    static const QLocale english(QLocale::English, QLocale::UnitedStates);
    const QDateTime local = when.toLocalTime();
    const bool today = local.date() == QDate::currentDate();
    return english.toString(local, today ? QStringLiteral("HH:mm") : QStringLiteral("d MMM HH:mm"));
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

    // Progress is also the cue for the scanners' other states: EthScanner
    // sends it when a pass gives up on addresses, BtcScanner when a failure
    // makes it busy again.
    connect(btcScanner_, &wallet::BtcScanner::progress, this, &WalletController::queueStatus);
    connect(ethScanner_, &wallet::EthScanner::progress, this, &WalletController::queueStatus);
    connect(btcScanner_, &wallet::BtcScanner::scanFinished, this, [this] { onScanFinished(&btcDone_); });
    connect(ethScanner_, &wallet::EthScanner::scanFinished, this, [this] { onScanFinished(&ethDone_); });
    // The only signal of a refresh pass that changed no row, failed or not:
    // ethBalancesAt() and ethRefreshFailing() move then.
    connect(ethScanner_, &wallet::EthScanner::passFinished, this, &WalletController::queueStatus);
    connect(btcScanner_, &wallet::BtcScanner::problem, this, &WalletController::onScannerProblem);
    connect(ethScanner_, &wallet::EthScanner::problem, this, &WalletController::onScannerProblem);
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
    // The first GitWallet on a data folder becomes the one that writes it. A
    // later one still loads and shows the wallet, but saves nothing (and takes
    // the lock over at its next write if the first one has quit by then).
    lockDataDir();
    if (!store_.exists()) {
        fileDigest_.clear();
        return false;
    }

    // Hashed before and after reading: equal digests name exactly the bytes
    // load() parsed. A file replaced meanwhile counts as one never opened.
    const std::optional<QByteArray> before = diskDigest();
    QString why;
    if (!store_.load(&why)) {
        // The file stays as it is: settings alone never replace it (an import
        // does). It may still open later, for the right Windows user.
        fileDigest_.clear();
        setError(error, storeMessage(why, QStringLiteral("The wallet file could not be read.")));
        return false;
    }
    const std::optional<QByteArray> after = diskDigest();
    fileDigest_ = (before && after && *before == *after) ? *after : QByteArray();
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

    // Before the words are touched: a phrase that could not be saved is never
    // turned into keys.
    if (!lockDataDir()) {
        setError(error, QStringLiteral("The seed was not imported: ") + inUseElsewhere());
        return false;
    }

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
    fileDigest_ = diskDigest().value_or(QByteArray()); // the file just written

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
    // Checked first, so a refusal leaves the scan running as it was.
    if (!lockDataDir()) {
        setError(error, QStringLiteral("The wallet was not removed: ") + inUseElsewhere());
        return false;
    }
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
        fileDigest_.clear();
        model_->clear();
        emit walletChanged();
        emit statusChanged();
        return false;
    }

    fileDigest_.clear(); // no file any more
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
    // not load or write, or one changed since: an unreadable one may still
    // open for the right Windows user, a newer one holds what someone else
    // saved, and only an import replaces either (settingsSaveBlocker()).
    bool saved = false;
    const QString blocker = settingsSaveBlocker();
    if (blocker.isEmpty()) {
        QString why;
        if (!store_.save(&why)) {
            store_.settings = previous;
            setError(error, storeMessage(why, QStringLiteral("The settings could not be saved.")));
            return false;
        }
        fileDigest_ = diskDigest().value_or(QByteArray()); // the file just written
        saved = true;
    } else {
        // In memory only, used by an import in this session. Shown in a toast
        // that elides long text, so the outcome comes first.
        setError(error, QStringLiteral("Applied for this session only: ") + blocker);
    }

    // The model keeps its rows: the restarted scanners adopt them instead of
    // deriving them again, and their last known state stays on screen
    // meanwhile; the Bitcoin scanner still checks every status again from
    // index 0.
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
    return btcScanning() || ethScanning();
}

bool WalletController::btcScanning() const
{
    return btcScanner_->isScanning();
}

bool WalletController::ethScanning() const
{
    return ethScanner_->isScanning();
}

bool WalletController::addressLimitReached() const
{
    return btcScanner_->limitReached() || ethScanner_->limitReached();
}

bool WalletController::ethWrongChain() const
{
    return ethScanner_->wrongChain();
}

QDateTime WalletController::ethBalancesAt() const
{
    return ethScanner_->balancesAt();
}

bool WalletController::ethRefreshFailing() const
{
    return ethScanner_->refreshFailing();
}

bool WalletController::balancesComplete() const
{
    return status() == Status::UpToDate;
}

QString WalletController::statusText() const
{
    const QString rows = addressCount(model_->size());
    switch (status()) {
    case Status::NoWallet:
        return QStringLiteral("No wallet");
    case Status::Connecting:
        return QStringLiteral("Connecting…");
    case Status::Limit:
        return QStringLiteral("Address limit reached · %1").arg(rows);
    case Status::WrongChain:
        return QStringLiteral("Ethereum endpoint not on mainnet · %1").arg(rows);
    case Status::EthStale: {
        // Before the first pass that answered every address there is no time
        // to date the figures by.
        const QDateTime at = ethBalancesAt();
        return at.isValid() ? QStringLiteral("ETH balances from %1 · %2").arg(clockTime(at), rows)
                            : QStringLiteral("ETH lookups failing · %1").arg(rows);
    }
    case Status::Scanning:
        return QStringLiteral("Scanning · %1").arg(rows);
    case Status::Unchecked:
        return QStringLiteral("Balances incomplete · %1 · %2 not checked")
            .arg(rows, grouped(model_->size() - model_->checkedCount()));
    case Status::Unscanned:
        return QStringLiteral("Some addresses not scanned · %1").arg(rows);
    case Status::UpToDate:
        break;
    }
    return QStringLiteral("Up to date · %1 · %2 funded").arg(rows, grouped(model_->fundedCount()));
}

QString WalletController::electrumServer() const
{
    return electrum_->serverName();
}

QString WalletController::ethEndpoint() const
{
    // Host only: a configured endpoint may carry an API key in its path, query
    // or user info, and this name is shown on screen and written to reports.
    return QUrl(eth_->currentEndpoint()).host();
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
                emit errorOccurred(wallet::Problem::BuiltInServers,
                                   QStringLiteral("None of the Electrum servers in Settings is "
                                                  "valid (host:port); using the built-in list."),
                                   QString());
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

void WalletController::onScannerProblem(wallet::Problem kind, const QString& message,
                                        const QString& serverDetail)
{
    // While stopping, the clients fail every pending request ("stopped"):
    // that is the shutdown itself, not something to show the user.
    if (!running_ || message.trimmed().isEmpty())
        return;
    emit errorOccurred(kind, message, serverDetail);
    // The scan state may have moved with the report (a window cut at the
    // limit, an endpoint off mainnet) while no progress() says so.
    queueStatus();
}

WalletController::Status WalletController::status() const
{
    if (!hasWallet())
        return Status::NoWallet;
    if (hasAccounts(store_.accounts, true) && !electrum_->isReady())
        return Status::Connecting;
    // A balance kept missing or stale by something no scan cures comes before
    // "Scanning": a window cut at the limit keeps the Bitcoin scan unfinished
    // for good, and an endpoint off mainnet, or failing, leaves Ethereum
    // addresses unanswered pass after pass.
    if (addressLimitReached())
        return Status::Limit;
    if (ethWrongChain())
        return Status::WrongChain;
    if (ethRefreshFailing())
        return Status::EthStale;
    if (isScanning())
        return Status::Scanning;
    // Nothing is scanning, yet a row has no answer (one no scan follows any
    // more, left from before a server or gap change): its balance is unknown.
    if (model_->checkedCount() < model_->size())
        return Status::Unchecked;
    if (accountUnscanned())
        return Status::Unscanned;
    return Status::UpToDate;
}

bool WalletController::accountUnscanned() const
{
    // An account key that could not be read, or a Bitcoin chain that could
    // not be derived from it, gets no rows at all: the scanners report it
    // (problem(), Skipped) and finish without it. Asked only once nothing is
    // scanning, when every chain that can be scanned has its first rows.
    // Index 1 counts as well, for an index 0 that BIP32 skips for having no key.
    const auto hasRows = [this](const wallet::AccountKey& a, quint32 change) {
        return model_->find(a.type, a.account, change, 0) >= 0
               || model_->find(a.type, a.account, change, 1) >= 0;
    };
    const QList<wallet::AccountKey>& accounts = store_.accounts;
    return std::any_of(accounts.cbegin(), accounts.cend(), [&hasRows](const wallet::AccountKey& a) {
        return !hasRows(a, 0) || (chain::isBitcoin(a.type) && !hasRows(a, 1));
    });
}

bool WalletController::lockDataDir()
{
    const QString path = lockFilePath();
    if (!lock_ || lock_->fileName() != path) {
        lock_ = std::make_unique<QLockFile>(path); // a lock on another folder is let go
        // Held for as long as the window stays open, days included: stale only
        // once the process that took it is gone, never because of its age.
        lock_->setStaleLockTime(std::chrono::milliseconds::zero());
    }
    if (lock_->isLocked() || lock_->tryLock())
        return true;
#ifdef Q_OS_WIN
    // Windows cannot delete the lock file of a live holder (it keeps the file
    // open without FILE_SHARE_DELETE), so this succeeds only for one a crash
    // left behind that Qt cannot prove stale: unreadable, written on another
    // PC, or naming a process id a GitWallet has been given since (this very
    // one included, after a reboot).
    if (lock_->error() == QLockFile::LockFailedError && lock_->removeStaleLockFile() && lock_->tryLock())
        return true;
#endif
    // Only another holder blocks writing. In a folder where no lock file can be
    // made at all, the save itself fails with the better message.
    return lock_->error() != QLockFile::LockFailedError;
}

QString WalletController::settingsSaveBlocker()
{
    if (!lockDataDir())
        return inUseElsewhere();
    const std::optional<QByteArray> disk = diskDigest();
    if (!disk)
        return QStringLiteral("wallet.dat could not be read to check it.");
    if (disk->isEmpty()) {
        // No file: settings alone make one without keys. Keys still shown here
        // came from a file deleted since ("Remove wallet" in another window,
        // Explorer; this window's own Remove forgets the keys): they are never
        // written back.
        return hasWallet() ? QStringLiteral("wallet.dat was deleted outside this window.") : QString();
    }
    if (fileDigest_.isEmpty() && !hasWallet())
        return QStringLiteral("wallet.dat was not opened."); // unreadable, or made since by someone else
    if (*disk != fileDigest_)
        return QStringLiteral("wallet.dat was changed outside this window; restart GitWallet to load it.");
    return QString();
}

} // namespace app
