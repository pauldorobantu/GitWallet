#include "app/WalletController.h"

#include "chain/Bip39.h"
#include "chain/Hash.h"
#include "net/ElectrumClient.h"
#include "net/EthRpc.h"
#include "net/PriceFeed.h"
#include "wallet/BtcScanner.h"
#include "wallet/BulkImport.h"
#include "wallet/EthScanner.h"
#include "wallet/WalletModel.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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
#include <utility>

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

// `base`, then the first number no wallet goes by, always within a name's length.
QString nextFreeName(const QString& base, const QSet<QString>& taken, const QSet<QString>& asked)
{
    for (int number = 1;; ++number) {
        const QString suffix = QLatin1Char(' ') + QString::number(number);
        QString head = base.left(wallet::WalletStore::kMaxLabelChars - suffix.size());
        if (!head.isEmpty() && head.back().isHighSurrogate()) // never half a character
            head.chop(1);
        const QString name = head.trimmed() + suffix;
        const QString key = name.toCaseFolded();
        if (!taken.contains(key) && !asked.contains(key))
            return name;
    }
}

// Older files an add moved aside because they could not be read, by file name.
QStringList keptAsideFiles()
{
    return QDir(wallet::WalletStore::dataDir()).entryList({QStringLiteral("wallet.dat.unread-*")}, QDir::Files);
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
    return !store_.wallets.isEmpty();
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

bool WalletController::addMnemonicWallet(const std::vector<std::string>& words,
                                         const std::string& passphrase, const QString& label,
                                         QString* error)
{
    setError(error, QString());

    // Before the words are touched: a phrase that could not be saved is never
    // turned into keys.
    if (!lockDataDir()) {
        setError(error, QStringLiteral("The seed was not added: ") + inUseElsewhere());
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
    return addWallet(std::move(accounts), label, false, QStringLiteral("The seed was not added"), error);
}

bool WalletController::addKeysWallet(const QList<wallet::AccountKey>& keys, const QString& label,
                                     QString* error)
{
    setError(error, QString());
    if (keys.isEmpty()) {
        setError(error, QStringLiteral("Enter at least one public key."));
        return false;
    }
    return addWallet(keys, label, true, QStringLiteral("The keys were not added"), error);
}

bool WalletController::addWallet(QList<wallet::AccountKey> accounts, const QString& label,
                                 bool fromKeys, const QString& what, QString* error)
{
    QList<wallet::WalletRequest> one(1);
    one.first().accounts = std::move(accounts);
    one.first().label = label;
    one.first().fromKeys = fromKeys;
    QString why;
    if (addWalletsAs(&one, what, &why) == 1)
        return true;
    setError(error, why.isEmpty() ? what + QStringLiteral(": ") + one.first().refused : why);
    return false;
}

int WalletController::addWallets(QList<wallet::WalletRequest>* requests, QString* error)
{
    return addWalletsAs(requests, QStringLiteral("The wallets were not added"), error);
}

int WalletController::addWalletsAs(QList<wallet::WalletRequest>* requests, const QString& what, QString* error)
{
    setError(error, QString());

    // One set of addresses has one owner: the scanners and the totals count each
    // address once. By what decides the addresses, not by the text of the key.
    QHash<QByteArray, QString> owners; // an account's addresses -> its wallet's name
    try {
        for (const wallet::WalletEntry& other : std::as_const(store_.wallets)) {
            for (const wallet::AccountKey& theirs : other.accounts)
                owners.insert(wallet::WalletStore::addressSetId(theirs), other.label);
        }
    } catch (const std::exception&) {
        setError(error, what + QStringLiteral(": a public key could not be read."));
        return 0;
    }

    // Names in use, and those the batch itself asks for, so no number lands on them.
    QSet<QString> taken;
    QSet<QString> asked;
    for (const wallet::WalletEntry& other : std::as_const(store_.wallets))
        taken.insert(other.label.toCaseFolded());
    for (const wallet::WalletRequest& request : std::as_const(*requests)) {
        if (!request.numbered)
            asked.insert(wallet::WalletStore::cleanLabel(request.label).toCaseFolded());
    }

    qsizetype room = wallet::WalletStore::kMaxWallets - store_.wallets.size();
    QList<wallet::WalletEntry> entries;
    for (wallet::WalletRequest& request : *requests) {
        request.refused.clear();
        // Whatever the screen let through: wallet.dat never holds a secret as a name.
        if (wallet::looksLikeSecret(request.label, wallet::WalletStore::kMaxLabelChars)) {
            request.refused = QStringLiteral("its name looks like seed words or a private key.");
            continue;
        }
        QList<QByteArray> ids;
        try {
            for (const wallet::AccountKey& key : std::as_const(request.accounts))
                ids.append(wallet::WalletStore::addressSetId(key));
        } catch (const std::exception&) {
            request.refused = QStringLiteral("a public key could not be read.");
            continue;
        }
        const auto owned = std::find_if(ids.cbegin(), ids.cend(),
                                        [&owners](const QByteArray& id) { return owners.contains(id); });
        if (owned != ids.cend()) {
            request.refused = QStringLiteral("wallet “%1” already has these keys.").arg(owners.value(*owned));
            continue;
        }
        if (room <= 0) {
            request.refused = QStringLiteral("GitWallet keeps at most %1 wallets.").arg(wallet::WalletStore::kMaxWallets);
            continue;
        }

        QString label = wallet::WalletStore::cleanLabel(request.label);
        if (label.isEmpty() || request.numbered) {
            // Only what really goes in takes a number.
            label = nextFreeName(label.isEmpty() ? QStringLiteral("Wallet") : label, taken, asked);
        } else if (taken.contains(label.toCaseFolded())) {
            request.refused = QStringLiteral("a wallet is already named “%1”.").arg(label);
            continue;
        }
        taken.insert(label.toCaseFolded());
        --room;

        wallet::WalletEntry entry;
        entry.id = store_.takeWalletId();
        entry.label = label;
        entry.fromKeys = request.fromKeys;
        entry.accounts = request.accounts;
        for (wallet::AccountKey& key : entry.accounts)
            key.wallet = entry.id;
        for (const QByteArray& id : std::as_const(ids))
            owners.insert(id, entry.label);
        entries.append(entry);
    }
    if (entries.isEmpty())
        return 0;

    // Last before the save: nothing is moved for an add refused above.
    QString movedTo;
    const QString blocker = walletSaveBlocker(&movedTo);
    if (!blocker.isEmpty()) {
        setError(error, what + QStringLiteral(": ") + blocker);
        return 0;
    }

    // Save before touching anything else: on failure the wallets shown so far
    // keep running as they were.
    const qsizetype before = store_.wallets.size();
    store_.wallets.append(entries);
    QString saveError;
    if (!saveStore(&saveError)) {
        store_.wallets.remove(before, entries.size());
        // The file moved aside goes back, so the next start warns about it again.
        if (!movedTo.isEmpty())
            QFile::rename(movedTo, wallet::WalletStore::filePath());
        setError(error, storeMessage(saveError, QStringLiteral("The wallet could not be saved.")));
        return 0;
    }

    restartServices();
    emit walletChanged();
    emit statusChanged();
    if (!movedTo.isEmpty()) {
        emit errorOccurred(wallet::Problem::FileSetAside,
                           QStringLiteral("The old wallet.dat could not be read, so it was kept as %1. "
                                          "Wallets it holds are not shown here.")
                               .arg(QDir::toNativeSeparators(movedTo)),
                           QString());
    }
    return int(entries.size());
}

bool WalletController::removeWallet(quint32 id, QString* error)
{
    setError(error, QString());
    const QString blocker = settingsSaveBlocker();
    if (!blocker.isEmpty()) {
        setError(error, QStringLiteral("The wallet was not removed: ") + blocker);
        return false;
    }
    const qsizetype position = std::find_if(store_.wallets.cbegin(), store_.wallets.cend(),
                                            [id](const wallet::WalletEntry& w) { return w.id == id; })
                               - store_.wallets.cbegin();
    if (position >= store_.wallets.size())
        return true; // gone already

    // Saved first: on failure the wallet stays, on disk and on screen. The file
    // stays too when the last wallet goes, for its settings (a file without
    // wallets loads as "no wallet yet").
    const wallet::WalletEntry removed = store_.wallets.takeAt(position);
    QString why;
    if (!saveStore(&why)) {
        store_.wallets.insert(position, removed);
        setError(error, storeMessage(why, QStringLiteral("The wallet could not be saved.")));
        return false;
    }

    stopServices();
    model_->removeWallet(id);
    startServices();
    emit walletChanged();
    emit statusChanged();
    return true;
}

bool WalletController::renameWallet(quint32 id, const QString& label, QString* error)
{
    setError(error, QString());
    const QString clean = wallet::WalletStore::cleanLabel(label);
    if (clean.isEmpty()) {
        setError(error, QStringLiteral("Enter a name for the wallet."));
        return false;
    }
    if (wallet::looksLikeSecret(label, wallet::WalletStore::kMaxLabelChars)) {
        setError(error, QStringLiteral("That looks like seed words or a private key, so it was not kept as a name."));
        return false;
    }
    const bool nameInUse = std::any_of(store_.wallets.cbegin(), store_.wallets.cend(), [&](const wallet::WalletEntry& w) {
        return w.id != id && w.label.toCaseFolded() == clean.toCaseFolded();
    });
    if (nameInUse) {
        setError(error, QStringLiteral("A wallet is already named “%1”.").arg(clean));
        return false;
    }
    const QString blocker = settingsSaveBlocker();
    if (!blocker.isEmpty()) {
        setError(error, QStringLiteral("The wallet was not renamed: ") + blocker);
        return false;
    }
    for (wallet::WalletEntry& w : store_.wallets) {
        if (w.id != id) continue;
        const QString previous = std::exchange(w.label, clean);
        QString why;
        if (!saveStore(&why)) {
            w.label = previous;
            setError(error, storeMessage(why, QStringLiteral("The wallet could not be saved.")));
            return false;
        }
        emit walletChanged(); // no scanner cares about a name
        return true;
    }
    return true; // gone already
}

bool WalletController::removeAllWallets(QString* error)
{
    setError(error, QString());
    // Checked first, so a refusal leaves the scan running as it was.
    if (!lockDataDir()) {
        setError(error, QStringLiteral("The wallets were not removed: ") + inUseElsewhere());
        return false;
    }
    // Like every other write: what this window never saw is not its to delete.
    const std::optional<QByteArray> disk = diskDigest();
    if (!disk || (!disk->isEmpty() && *disk != fileDigest_)) {
        setError(error, QStringLiteral("The wallets were not removed: wallet.dat was changed outside this "
                                       "window; restart GitWallet to load it."));
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
    // "Every wallet": the unread files an add kept aside hold wallets too.
    const QDir folder(wallet::WalletStore::dataDir());
    const QStringList aside = keptAsideFiles();
    for (const QString& name : aside)
        QFile::remove(folder.filePath(name));
    model_->clear();
    emit walletChanged();
    emit statusChanged();
    return true;
}

int WalletController::walletFilesKeptAside() const
{
    return int(keptAsideFiles().size());
}

QString WalletController::unreadFileNote() const
{
    return diskDigest()
               ? QStringLiteral("The file stays as it is. If you add a wallet now, GitWallet keeps it aside under "
                                "another name and says where.")
               : QStringLiteral("The file stays as it is. GitWallet cannot open it at all, so it cannot keep it "
                                "aside either: to add a wallet, first move it out of %1 by hand.")
                     .arg(QDir::toNativeSeparators(wallet::WalletStore::dataDir()));
}

WalletController::GapImpact WalletController::gapImpact(const wallet::ScanSettings& settings) const
{
    struct Row {
        quint32 index;
        bool funded;
        bool found; // a fresh scan reads it as used, which is what moves its window
        bool bitcoin;
    };
    struct Chain {
        QList<Row> rows;
        int gapNow = 0;
        int gapBefore = 0;
    };
    QHash<QString, Chain> chains;
    const wallet::ScanSettings s = sanitized(settings);
    const wallet::ScanSettings& before = store_.settings;
    for (int i = 0; i < model_->size(); ++i) {
        const wallet::AddressRow& r = model_->at(i);
        const bool funded = r.state() == wallet::AddressState::Funded;
        if (!r.used && !funded)
            continue;
        const QString key = QStringLiteral("%1/%2/%3/%4").arg(r.wallet).arg(int(r.type)).arg(r.account).arg(r.change);
        // Ethereum's `used` is sticky and a fresh scan starts without it: an address
        // that gave its tokens away by signature reads as unused again.
        const bool found = funded || (r.isBitcoin() ? r.used : r.nonce > 0);
        Chain& chain = chains[key];
        chain.rows.append({r.index, funded, found, r.isBitcoin()});
        chain.gapNow = !r.isBitcoin() ? s.ethGap : r.change == 0 ? s.btcReceiveGap : s.btcChangeGap;
        chain.gapBefore = !r.isBitcoin() ? before.ethGap : r.change == 0 ? before.btcReceiveGap : before.btcChangeGap;
    }

    // A fresh scan walks each chain from index 0, up to where the gap ends. What it
    // reaches, and how far that takes the windows past their gaps.
    struct Reach {
        QList<Row> rows;
        qint64 btcGrown = 0;
        qint64 ethGrown = 0;
    };
    GapImpact impact;
    const bool cleared = gapsFall(before, s); // otherwise the rows stay as they are
    const auto walk = [&chains](bool withNewGaps, GapImpact* leftOut) {
        Reach reach;
        for (auto it = chains.begin(); it != chains.end(); ++it) {
            std::sort(it->rows.begin(), it->rows.end(), [](const Row& a, const Row& b) { return a.index < b.index; });
            const qint64 gap = withNewGaps ? it->gapNow : it->gapBefore;
            qint64 last = -1;
            bool cut = false;
            for (const Row& row : std::as_const(it->rows)) {
                cut = cut || qint64(row.index) > last + gap;
                if (!cut) {
                    if (row.found)
                        last = row.index;
                    reach.rows.append(row);
                } else if (leftOut) {
                    ++leftOut->used;
                    if (row.funded)
                        ++leftOut->funded;
                }
            }
            (it->rows.constFirst().bitcoin ? reach.btcGrown : reach.ethGrown) += last + 1;
        }
        return reach;
    };
    Reach reachNow = walk(true, cleared ? &impact : nullptr);
    const Reach reachBefore = walk(false, nullptr);

    // A run of used addresses has no hole for the walk to find, but a scan also stops
    // at a limit over all accounts, which the Bitcoin one falls with the gaps. What
    // these settings want past the limit, beyond what the ones in use do already.
    qint64 btcGapsNow = 0;
    qint64 btcGapsBefore = 0;
    qint64 ethAccounts = 0;
    const QList<wallet::AccountKey> accounts = store_.allAccounts();
    for (const wallet::AccountKey& key : accounts) {
        if (chain::isBitcoin(key.type)) {
            btcGapsNow += qint64(s.btcReceiveGap) + s.btcChangeGap;
            btcGapsBefore += qint64(before.btcReceiveGap) + before.btcChangeGap;
        } else {
            ++ethAccounts;
        }
    }
    const auto overBy = [](qint64 wanted, qint64 limit) { return qMax(qint64(0), wanted - limit); };
    const qint64 btcOver = overBy(btcGapsNow + reachNow.btcGrown, wallet::BtcScanner::rowLimitFor(btcGapsNow))
                           - overBy(btcGapsBefore + reachBefore.btcGrown, wallet::BtcScanner::rowLimitFor(btcGapsBefore));
    const qint64 ethOver = overBy(ethAccounts * s.ethGap + reachNow.ethGrown, wallet::EthScanner::kMaxFollowedRows)
                           - overBy(ethAccounts * before.ethGap + reachBefore.ethGrown, wallet::EthScanner::kMaxFollowedRows);
    impact.overLimit = qMax(qint64(0), btcOver) + qMax(qint64(0), ethOver);
    if (impact.overLimit > 0) {
        // Which ones depends on the order the chains grow in: the furthest out, at
        // most this many, since the ends of the windows go first.
        std::sort(reachNow.rows.begin(), reachNow.rows.end(), [](const Row& a, const Row& b) { return a.index > b.index; });
        qint64 btcLeft = btcOver;
        qint64 ethLeft = ethOver;
        for (const Row& row : std::as_const(reachNow.rows)) {
            qint64& left = row.bitcoin ? btcLeft : ethLeft;
            if (left <= 0)
                continue;
            --left;
            ++impact.used;
            if (row.funded)
                ++impact.funded;
            impact.atMost = true;
        }
    }
    return impact;
}

bool WalletController::gapsFall(const wallet::ScanSettings& from, const wallet::ScanSettings& to)
{
    return to.btcReceiveGap < from.btcReceiveGap || to.btcChangeGap < from.btcChangeGap || to.ethGap < from.ethGap;
}

bool WalletController::applySettings(const wallet::ScanSettings& settings, QString* error)
{
    setError(error, QString());
    const wallet::ScanSettings previous = store_.settings;
    store_.settings = sanitized(settings);

    // Saved with the account keys, or on their own before the first wallet, so
    // that a private server set up first is still the one the first wallet talks
    // to after a restart (loadExisting() reads a file without wallets as "no
    // wallet yet"). Settings alone never replace a wallet.dat this session did
    // not load or write, or one changed since: an unreadable one may still
    // open for the right Windows user, and a newer one holds what someone else
    // saved (settingsSaveBlocker()).
    bool saved = false;
    const QString blocker = settingsSaveBlocker();
    if (blocker.isEmpty()) {
        QString why;
        if (!saveStore(&why)) {
            store_.settings = previous;
            setError(error, storeMessage(why, QStringLiteral("The settings could not be saved.")));
            return false;
        }
        saved = true;
    } else {
        // In memory only, used by a wallet added in this session. Shown in a
        // toast that elides long text, so the outcome comes first.
        setError(error, QStringLiteral("Applied for this session only: ") + blocker);
    }

    // The model keeps its rows: the restarted scanners adopt them instead of
    // deriving them again, and their last known state stays on screen
    // meanwhile; the Bitcoin scanner still checks every status again from
    // index 0. Not after a smaller gap: rows past the new windows would stay on
    // screen unfollowed until the next start drops them, so they go now.
    const bool gapsDown = gapsFall(previous, store_.settings);
    stopServices();
    if (gapsDown)
        model_->clear();
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

double WalletController::totalUsd(quint32 wallet) const
{
    double sum = 0.0;
    for (const wallet::AssetInfo& info : wallet::assets())
        sum += model_->total(info.asset, wallet).toDouble(info.decimals) * prices_->usd(info.symbol);
    return sum;
}

void WalletController::restartServices()
{
    // The model keeps its rows: the restarted scanners adopt them instead of
    // deriving them again, and their last known state stays on screen meanwhile.
    stopServices();
    startServices();
}

void WalletController::startServices()
{
    const QList<wallet::AccountKey> accounts = store_.allAccounts();
    const bool hasBtc = hasAccounts(accounts, true);
    const bool hasEth = hasAccounts(accounts, false);
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

    btcScanner_->setAccounts(accounts);
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
    ethScanner_->setAccounts(accounts);
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
    if (hasAccounts(store_.allAccounts(), true) && !electrum_->isReady())
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
    const QList<wallet::AccountKey> accounts = store_.allAccounts();
    return std::any_of(accounts.cbegin(), accounts.cend(),
                       [this](const wallet::AccountKey& a) { return !accountHasRows(a); });
}

bool WalletController::accountHasRows(const wallet::AccountKey& a) const
{
    const auto hasRows = [this, &a](quint32 change) {
        return model_->find(a.wallet, a.type, a.account, change, 0) >= 0
               || model_->find(a.wallet, a.type, a.account, change, 1) >= 0;
    };
    return hasRows(0) && (!chain::isBitcoin(a.type) || hasRows(1));
}

bool WalletController::walletHasAllRows(const wallet::WalletEntry& entry) const
{
    return std::all_of(entry.accounts.cbegin(), entry.accounts.cend(),
                       [this](const wallet::AccountKey& a) { return accountHasRows(a); });
}

bool WalletController::saveStore(QString* why)
{
    // What was written, not a read of the file afterwards: a program holding the
    // file for a moment must not make every later save look like a foreign change.
    QByteArray digest;
    if (!store_.save(why, &digest))
        return false;
    fileDigest_ = digest;
    return true;
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
    if (!disk) {
        // With wallets loaded this is their file, held for a moment by another program:
        // moved away, they would be gone at the next start.
        if (hasWallet())
            return QStringLiteral("wallet.dat cannot be read right now, so nothing was changed. Try again in a moment.");
        return QStringLiteral("wallet.dat cannot be opened, so it can be neither checked nor kept aside. Move it out "
                              "of %1 by hand, then try again.")
            .arg(QDir::toNativeSeparators(wallet::WalletStore::dataDir()));
    }
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

QString WalletController::walletSaveBlocker(QString* movedTo)
{
    const QString blocker = settingsSaveBlocker();
    if (blocker.isEmpty() || !fileDigest_.isEmpty() || hasWallet() || !lockDataDir())
        return blocker;
    const std::optional<QByteArray> disk = diskDigest();
    if (!disk || disk->isEmpty())
        return blocker;

    // A wallet.dat this session never opened. If it opens now (it was only busy
    // at the start, or a backup was restored since), its wallets are the user's.
    wallet::WalletStore probe;
    QString why;
    if (probe.load(&why))
        return QStringLiteral("wallet.dat can be read now; restart GitWallet to load it.");

    // Another Windows user's, damaged, or from a newer GitWallet: kept beside the
    // new one under a name nothing reads.
    const QString aside = wallet::WalletStore::filePath() + QStringLiteral(".unread-")
                          + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    if (!QFile::rename(wallet::WalletStore::filePath(), aside))
        return QStringLiteral("wallet.dat could not be opened, nor moved aside.");
    *movedTo = aside;
    return QString();
}

} // namespace app
