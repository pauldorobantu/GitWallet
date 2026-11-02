#include "app/WalletController.h"

#include "chain/Bip39.h"
#include "chain/Hash.h"
#include "net/PriceFeed.h"
#include "store/Ledger.h"
#include "ui/LanguageManager.h"
#include "wallet/BulkImport.h"
#include "wallet/EvmScanner.h"
#include "wallet/History.h"
#include "wallet/UtxoHistory.h"
#include "wallet/UtxoScanner.h"
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
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace app {

namespace {

// Price refresh failures. Never carries a seed or a key.
Q_STATIC_LOGGING_CATEGORY(lcNet, "gitwallet.net")

constexpr int kMinGap = 20;
constexpr int kMaxGap = 5000;
constexpr int kMinRefreshSeconds = 15;
constexpr int kMaxRefreshSeconds = 3600;
// WalletStore's limits for a server list (it drops what exceeds them when it
// writes wallet.dat); in practice a list holds a handful of entries.
constexpr qsizetype kMaxListEntries = 100;
constexpr qsizetype kMaxEntryChars = 512;
// DefiLlama answered calls seconds apart, each for what its cache held; there is no
// reason to ask more often than prices move on screen.
constexpr std::chrono::minutes kPriceInterval{2};
constexpr std::chrono::seconds kTimesInterval{30};
// The window before this one is usually only finishing: closing can take a moment at scale.
constexpr std::chrono::seconds kLedgerRetryInterval{5};
constexpr int kLedgerRetriesBeforeSaying = 6;
// A price older than this says it did not move on its own.
constexpr qint64 kPricesOldSeconds = 3600;
// Lines of a file of labels that go in per event-loop turn, each lot in one transaction.
constexpr qsizetype kImportChunk = 2000;
// Past anything an export makes of half a million notes: a file this long is a mistake, and not one
// to read into memory.
constexpr qint64 kMaxLabelsFileBytes = qint64(1) << 30;
// Scanners report progress once per address (thousands in a row on a shop
// seed); the title chip needs only a few updates a second.
constexpr std::chrono::milliseconds kStatusThrottle{150};
// Far above any wallet.dat GitWallet writes (WalletStore stops at 16 MiB): a
// larger file is not one this session loaded or wrote, and is not hashed.
constexpr qint64 kMaxDigestBytes = qint64(64) << 20;

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

// Two names that read the same on screen are one name, whatever their case or Unicode form.
QString nameKey(const QString& label)
{
    return label.normalized(QString::NormalizationForm_C).toCaseFolded();
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
        const QString key = nameKey(name);
        if (!taken.contains(key) && !asked.contains(key))
            return name;
    }
}

// Older files beside wallet.dat, by file name: what an add moved aside because it could
// not be read, and the copy of a file of an older format from before its first rewrite.
QStringList keptAsideFiles()
{
    return QDir(wallet::WalletStore::dataDir())
        .entryList({QStringLiteral("wallet.dat.unread-*"), QStringLiteral("wallet.dat.format*")}, QDir::Files);
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
    s.utxoReceiveGap = std::clamp(s.utxoReceiveGap, kMinGap, kMaxGap);
    s.utxoChangeGap = std::clamp(s.utxoChangeGap, kMinGap, kMaxGap);
    s.evmGap = std::clamp(s.evmGap, kMinGap, kMaxGap);
    s.evmRefreshSeconds = std::clamp(s.evmRefreshSeconds, kMinRefreshSeconds, kMaxRefreshSeconds);
    QMap<QString, QStringList> servers;
    for (auto it = s.servers.cbegin(); it != s.servers.cend(); ++it) {
        const QStringList clean = cleanLines(it.value());
        if (!clean.isEmpty())
            servers.insert(it.key(), clean);
    }
    s.servers = servers;
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
    prices_ = new net::PriceFeed(this);
    model_ = new wallet::WalletModel(this);
    scheduler_ = new wallet::Scheduler(model_, this);
    ledger_ = new store::Ledger(model_, this);
    const QList<wallet::HistoryBook*> books = scheduler_->books();
    for (wallet::HistoryBook* book : books)
        ledger_->addBook(book);
    connect(scheduler_, &wallet::Scheduler::historyProgress, this, &WalletController::historyProgress);
    connect(scheduler_, &wallet::Scheduler::historyProgress, this, &WalletController::queueStatus); // an address may not add up

    priceTimer_ = new QTimer(this);
    priceTimer_->setInterval(kPriceInterval);
    connect(priceTimer_, &QTimer::timeout, prices_, &net::PriceFeed::refresh);

    timesTimer_ = new QTimer(this);
    timesTimer_->setInterval(kTimesInterval);
    connect(timesTimer_, &QTimer::timeout, this, &WalletController::saveTimes);
    // Figures age with nothing else happening: what the title says goes by the time too.
    connect(timesTimer_, &QTimer::timeout, this, &WalletController::queueStatus);

    ledgerRetry_ = new QTimer(this);
    ledgerRetry_->setInterval(kLedgerRetryInterval);
    connect(ledgerRetry_, &QTimer::timeout, this, &WalletController::tryLedgerAgain);

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
    connect(prices_, &net::PriceFeed::updated, this, &WalletController::savePrices);
    connect(prices_, &net::PriceFeed::failed, this, [this](const QString& reason) {
        qCInfo(lcNet).noquote() << "a price source failed, keeping the last prices:" << reason;
        emit totalsChanged(); // the prices line says so
    });

    // The Settings page shows the server in use, so a switch counts as a status change too.
    connect(scheduler_, &wallet::Scheduler::serverChanged, this, &WalletController::statusChanged);
    connect(scheduler_, &wallet::Scheduler::progress, this, &WalletController::onScanProgress);
    connect(scheduler_, &wallet::Scheduler::scanFinished, this, &WalletController::onScanFinished);
    connect(scheduler_, &wallet::Scheduler::problem, this, &WalletController::onScannerProblem);

    connect(ledger_, &store::Ledger::loaded, this, &WalletController::onLedgerLoaded);
    connect(ledger_, &store::Ledger::problem, this, [this](const QString& message) {
        emit errorOccurred(wallet::Problem::SavedData, message, QString());
    });
    connect(ledger_, &store::Ledger::labelsProblem, this, [this](const QString& message) {
        emit errorOccurred(wallet::Problem::LabelsNotKept, message, QString());
    });
    connect(ledger_, &store::Ledger::labelsOwedChanged, this, [this](const QString& why) {
        if (ledger_->labelsOwed())
            emit errorOccurred(wallet::Problem::LabelsOwed, QStringLiteral("The labels and notes of a file kept aside could not be written into this one: %1. Until they have, none can be changed. This is tried again every minute.").arg(why), QString());
        emit statusChanged();
    });
    connect(ledger_, &store::Ledger::labelsRead, this, [this](const QHash<QString, QString>& labels) {
        addressLabels_ = labels;
        emit labelsChanged();
    });
    connect(ledger_, &store::Ledger::labelsRemoved, this, [this](const QStringList& addresses) {
        for (const QString& address : addresses)
            addressLabels_.remove(address);
        emit labelsChanged();
    });
    connect(ledger_, &store::Ledger::setAside, this, [this](const QString& file, int recovered, bool whole) {
        const QString shown = QDir::toNativeSeparators(file);
        QString message = QStringLiteral("The saved balances could not be read, so they were kept as %1 and are being read again.").arg(shown);
        if (recovered < 0)
            message += QStringLiteral(" Its labels and notes could not be brought over: only that file, or a copy you exported, holds them.");
        else if (!whole)
            message += QStringLiteral(" %1 of its labels and notes came along; the rest could not be read, and stay in that file.").arg(recovered);
        else if (recovered > 0)
            message += QStringLiteral(" Its %1 labels and notes came along.").arg(recovered);
        emit errorOccurred(wallet::Problem::SavedDataAside, message, QString());
    });
    // The rows they describe are gone.
    connect(model_, &wallet::WalletModel::cleared, this, [this] {
        if (model_->size() == 0 && !waitingForLedger_) {
            showingSaved_ = false;
            savedAt_ = QDateTime();
        }
    });
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
    // A copy of the labels someone is counting on: nothing would otherwise be there, with part of it
    // in the clear beside it.
    for (const QPointer<QThread>& thread : std::as_const(copiesBeingWritten_)) {
        if (thread)
            thread->wait();
    }
    ledger_->close();
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
    ledger_->close();
    model_->clear();
    const QList<wallet::HistoryBook*> books = scheduler_->books();
    for (wallet::HistoryBook* book : books)
        book->clear();
    openLedger();
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
        taken.insert(nameKey(other.label));
    for (const wallet::WalletRequest& request : std::as_const(*requests)) {
        if (!request.numbered && !wallet::looksLikeSecret(request.label))
            asked.insert(nameKey(wallet::WalletStore::cleanLabel(request.label)));
    }

    qsizetype room = wallet::WalletStore::kMaxWallets - store_.wallets.size();
    QList<wallet::WalletEntry> entries;
    for (wallet::WalletRequest& request : *requests) {
        request.refused.clear();
        // Whatever the screen let through: wallet.dat never holds a secret as a name.
        if (wallet::looksLikeSecret(request.label)) {
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
        } else if (taken.contains(nameKey(label))) {
            request.refused = QStringLiteral("a wallet is already named “%1”.").arg(label);
            continue;
        }
        taken.insert(nameKey(label));
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
    // keep running as they were. The first save of a wallet.dat brings the key of its
    // database along.
    const qsizetype before = store_.wallets.size();
    store_.wallets.append(entries);
    const bool newKey = store_.databaseKey.isEmpty();
    if (newKey)
        store_.databaseKey = wallet::WalletStore::newDatabaseKey();
    QString saveError;
    if (!saveStore(&saveError)) {
        store_.wallets.remove(before, entries.size());
        if (newKey)
            store_.databaseKey.clear();
        // The file moved aside goes back, so the next start warns about it again.
        if (!movedTo.isEmpty())
            QFile::rename(movedTo, wallet::WalletStore::filePath());
        setError(error, storeMessage(saveError, QStringLiteral("The wallet could not be saved.")));
        return 0;
    }

    if (ledger_->isOpen())
        restartServices();
    else
        openLedger();
    emit walletChanged();
    emit statusChanged();
    if (!movedTo.isEmpty()) {
        fileSetAside_ = QDir::toNativeSeparators(movedTo);
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
    // What it holds of this wallet is still on its way: it would go on arriving after this.
    if (waitingForLedger_) {
        setError(error, QStringLiteral("The wallet was not removed: what is saved is still being read. Try again in a moment."));
        return false;
    }
    // Its labels would come back with those.
    if (ledger_->labelsOwed()) {
        setError(error, QStringLiteral("The wallet was not removed: ") + labelsBlocker());
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
    // What only this PC knew of its addresses goes with them.
    QHash<QString, QString> labelled;
    QSet<QString> elsewhere;
    for (int row = 0; row < model_->size(); ++row) {
        const wallet::AddressRow& r = model_->at(row);
        if (!addressLabels_.contains(r.address))
            continue;
        if (r.wallet == id)
            labelled.insert(r.address, QString());
        else
            elsewhere.insert(r.address);
    }
    for (const QString& address : std::as_const(elsewhere))
        labelled.remove(address);
    if (!labelled.isEmpty()) {
        for (auto it = labelled.cbegin(); it != labelled.cend(); ++it)
            addressLabels_.remove(it.key());
        // In one go: one by one, each is a flush to disk, and everything else waits behind those.
        ledger_->saveAddressLabels(labelled);
        emit labelsChanged();
    }
    const QList<wallet::HistoryBook*> books = scheduler_->books();
    for (wallet::HistoryBook* book : books)
        book->forgetWallet(id);
    ledger_->forgetWallet(id);
    model_->removeWallet(id);
    startServices();
    emit walletChanged();
    emit statusChanged();
    return true;
}

bool WalletController::renameWallet(quint32 id, const QString& label, QString* error)
{
    setError(error, QString());
    // First: cleaning makes copies.
    if (wallet::looksLikeSecret(label)) {
        setError(error, QStringLiteral("That looks like seed words or a private key, so it was not kept as a name."));
        return false;
    }
    const QString clean = wallet::WalletStore::cleanLabel(label);
    if (clean.isEmpty()) {
        setError(error, QStringLiteral("Enter a name for the wallet."));
        return false;
    }
    const bool nameInUse = std::any_of(store_.wallets.cbegin(), store_.wallets.cend(), [&](const wallet::WalletEntry& w) {
        return w.id != id && nameKey(w.label) == nameKey(clean);
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
    // Nothing could stop what is reading it, and the window would wait for that.
    if (waitingForLedger_) {
        setError(error, QStringLiteral("The wallets were not removed: what is saved is still being read. Try again in a moment."));
        return false;
    }
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
        // WalletStore::remove() resets the store only once the file is gone.
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
    // "Every wallet": the unread files an add kept aside hold wallets too, and the
    // database holds their addresses.
    const QDir folder(wallet::WalletStore::dataDir());
    const QStringList aside = keptAsideFiles();
    QStringList left;
    for (const QString& name : aside) {
        if (!QFile::remove(folder.filePath(name)))
            left.append(QDir::toNativeSeparators(folder.filePath(name)));
    }
    for (const QString& file : ledger_->destroy(wallet::WalletStore::databasePath()))
        left.append(QDir::toNativeSeparators(file));
    showingSaved_ = false;
    savedAt_ = QDateTime();
    model_->clear();
    const QList<wallet::HistoryBook*> books = scheduler_->books();
    for (wallet::HistoryBook* book : books)
        book->clear();
    addressLabels_.clear();
    emit labelsChanged();
    emit walletChanged();
    emit statusChanged();
    if (!left.isEmpty())
        emit errorOccurred(wallet::Problem::FilesLeft,
                           QStringLiteral("The wallets were removed, but these files could not be deleted, and hold "
                                          "their public keys or addresses: %1. Delete them when no program holds them.")
                               .arg(left.join(QStringLiteral(", "))),
                           QString());
    return true;
}

int WalletController::walletFilesKeptAside() const
{
    return int(keptAsideFiles().size());
}

int WalletController::databasesKeptAside() const
{
    QStringList files = store::Ledger::keptAsideFiles(wallet::WalletStore::databasePath());
    files.removeIf([](const QString& name) { return name.endsWith(QLatin1String("-wal")) || name.endsWith(QLatin1String("-shm")) || name.endsWith(QLatin1String(".labels-pending")); });
    return int(files.size());
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
        wallet::Network network;
        bool pastGap = false; // beyond what its chain's gap alone reaches from index 0
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
        const QString key = QStringLiteral("%1/%2/%3/%4/%5").arg(r.wallet).arg(int(r.network)).arg(int(r.type)).arg(r.account).arg(r.change);
        // What moves an account's window is what its row shows: an address that gave its
        // tokens away by signature reads as unused again.
        const bool found = funded || (r.isUtxo() ? r.used : r.nonce > 0);
        Chain& chain = chains[key];
        chain.rows.append({r.index, funded, found, r.network});
        chain.gapNow = !r.isUtxo() ? s.evmGap : r.change == 0 ? s.utxoReceiveGap : s.utxoChangeGap;
        chain.gapBefore = !r.isUtxo() ? before.evmGap : r.change == 0 ? before.utxoReceiveGap : before.utxoChangeGap;
    }

    // A fresh scan walks each chain from index 0, up to where the gap ends. What it
    // reaches, and how far that takes the windows past their gaps, per network.
    struct Reach {
        QList<Row> rows;
        QHash<wallet::Network, qint64> grown;
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
                    Row reached = row;
                    reached.pastGap = qint64(row.index) >= gap;
                    reach.rows.append(reached);
                } else if (leftOut) {
                    ++leftOut->used;
                    if (row.funded)
                        ++leftOut->funded;
                }
            }
            reach.grown[it->rows.constFirst().network] += last + 1;
        }
        return reach;
    };
    Reach reachNow = walk(true, cleared ? &impact : nullptr);
    const Reach reachBefore = walk(false, nullptr);

    // A run of used addresses has no hole for the walk to find, but a scan also stops
    // at a limit over all accounts, which a UTXO network's falls with the gaps. What
    // these settings want past the limit: all of it if the rows go, as nothing they
    // hold then stays; else beyond what the settings in use want already.
    const QList<wallet::AccountKey> accounts = store_.allAccounts();
    const auto overBy = [](qint64 wanted, qint64 limit) { return qMax(qint64(0), wanted - limit); };
    QHash<wallet::Network, qint64> over;
    QHash<wallet::Network, bool> noGrowth; // the gaps alone fill the limit
    for (const wallet::NetworkInfo& network : wallet::networks()) {
        qint64 gapsNow = 0;
        qint64 gapsBefore = 0;
        for (const wallet::AccountKey& key : accounts) {
            if (!key.serves(network.network))
                continue;
            const bool utxo = network.family == wallet::Family::Utxo;
            gapsNow += utxo ? qint64(s.utxoReceiveGap) + s.utxoChangeGap : s.evmGap;
            gapsBefore += utxo ? qint64(before.utxoReceiveGap) + before.utxoChangeGap : before.evmGap;
        }
        const bool utxo = network.family == wallet::Family::Utxo;
        const qint64 limitNow = utxo ? wallet::UtxoScanner::rowLimitFor(gapsNow) : wallet::EvmScanner::kMaxFollowedRows;
        const qint64 limitBefore = utxo ? wallet::UtxoScanner::rowLimitFor(gapsBefore) : wallet::EvmScanner::kMaxFollowedRows;
        const qint64 wanted = overBy(gapsNow + reachNow.grown.value(network.network), limitNow);
        const qint64 wantedBefore = overBy(gapsBefore + reachBefore.grown.value(network.network), limitBefore);
        over.insert(network.network, cleared ? wanted : qMax(qint64(0), wanted - wantedBefore));
        noGrowth.insert(network.network, gapsNow >= limitNow);
        impact.overLimit += over.value(network.network);
    }
    if (impact.overLimit > 0) {
        // When the gaps alone fill the limit no window grows at all, so every address past
        // its gap goes. Otherwise which ones depends on the order the chains grow in: the
        // furthest out, at most this many, since the ends of the windows go first.
        std::sort(reachNow.rows.begin(), reachNow.rows.end(), [](const Row& a, const Row& b) { return a.index > b.index; });
        QHash<wallet::Network, qint64> left = over;
        for (const Row& row : std::as_const(reachNow.rows)) {
            qint64& leftHere = left[row.network];
            const bool nothingGrows = cleared && noGrowth.value(row.network);
            if (nothingGrows ? !row.pastGap : leftHere <= 0)
                continue;
            --leftHere;
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
    return to.utxoReceiveGap < from.utxoReceiveGap || to.utxoChangeGap < from.utxoChangeGap || to.evmGap < from.evmGap;
}

bool WalletController::applySettings(const wallet::ScanSettings& settings, QString* error)
{
    setError(error, QString());
    // Nothing changed: no reason to drop a connection and ask a large wallet everything
    // again. Unless what is in use could not be written: then this is what tries again.
    if (!settingsOnlyHere_ && sanitized(settings) == store_.settings)
        return true;
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
        settingsOnlyHere_ = false;
    } else {
        settingsOnlyHere_ = true;
        // In memory only, used by a wallet added in this session. Shown in a
        // toast that elides long text, so the outcome comes first.
        setError(error, QStringLiteral("Applied for this session only: ") + blocker);
    }

    // The model keeps its rows: the restarted scanners adopt them instead of
    // deriving them again, and their last known state stays on screen
    // meanwhile; the Bitcoin scanner still checks every status again from
    // index 0. Not after a smaller gap: rows past the new windows would stay on
    // screen unfollowed until the scan has found those windows, so they go now.
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
    scheduler_->refreshNow();
}

bool WalletController::isScanning() const
{
    return scheduler_->isScanning();
}

bool WalletController::addressLimitReached() const
{
    return scheduler_->limitReached();
}

wallet::NetworkStatus WalletController::networkStatus(wallet::Network network) const
{
    return scheduler_->status(network);
}

bool WalletController::balancesComplete() const
{
    return status() == Status::UpToDate;
}

bool WalletController::figuresCurrent() const
{
    const Status now = status();
    return now == Status::UpToDate || (now == Status::Unscanned && hasUnreadKeys());
}

bool WalletController::keepingSaved() const
{
    return ledger_->isOpen();
}

bool WalletController::labelsOwed() const
{
    return ledger_->labelsOwed();
}

bool WalletController::historiesRead() const
{
    if (!scheduler_->allAnswered())
        return false;
    const QList<wallet::UtxoHistory*> histories = scheduler_->histories();
    return std::all_of(histories.cbegin(), histories.cend(), [](const wallet::UtxoHistory* history) {
        const wallet::HistoryProgress progress = history->progress();
        return history->running() && history->connected() && !progress.working() && progress.refused == 0;
    });
}

bool WalletController::hasUnreadKeys() const
{
    return std::any_of(store_.wallets.cbegin(), store_.wallets.cend(),
                       [](const wallet::WalletEntry& w) { return !w.keptAccounts.isEmpty(); });
}

QString WalletController::statusText() const
{
    const QString rows = addressCount(model_->size());
    switch (status()) {
    case Status::NoWallet:
        return QStringLiteral("No wallet");
    case Status::Loading:
        return QStringLiteral("Loading saved balances · %1").arg(rows);
    case Status::Connecting:
        return showingSaved_ ? QStringLiteral("Saved balances · connecting…") : QStringLiteral("Connecting…");
    case Status::Limit:
        return QStringLiteral("Address limit reached · %1").arg(rows);
    case Status::WrongChain: {
        const QList<wallet::NetworkStatus> all = scheduler_->statuses();
        for (const wallet::NetworkStatus& one : all) {
            if (one.wrongChain)
                return QStringLiteral("%1 endpoint answers for another network · %2")
                    .arg(wallet::networkInfo(one.network).name, rows);
        }
        break;
    }
    case Status::Stale: {
        const QList<wallet::NetworkStatus> all = scheduler_->statuses();
        for (const wallet::NetworkStatus& one : all) {
            if (!one.failing)
                continue;
            // Before the first pass that answered every address there is no time
            // to date the figures by.
            const wallet::NetworkInfo& info = wallet::networkInfo(one.network);
            return one.balancesAt.isValid()
                       ? QStringLiteral("%1 balances from %2 · %3").arg(wallet::assetInfo(info.coin).symbol, clockTime(one.balancesAt), rows)
                       : QStringLiteral("%1 lookups failing · %2").arg(wallet::assetInfo(info.coin).symbol, rows);
        }
        break;
    }
    case Status::Scanning:
        return showingSaved_ ? QStringLiteral("Checking saved balances · %1").arg(rows)
                             : QStringLiteral("Scanning · %1").arg(rows);
    case Status::Unchecked:
        return QStringLiteral("Balances incomplete · %1 · %2 not checked")
            .arg(rows, grouped(model_->size() - model_->checkedCount()));
    case Status::Unscanned:
        return QStringLiteral("Some addresses not scanned · %1").arg(rows);
    case Status::DoNotAddUp:
        return QStringLiteral("Balances in doubt · %1 · %2 do not add up to their transactions").arg(rows, grouped(differingAddresses()));
    case Status::UpToDate:
        break;
    }
    return QStringLiteral("Up to date · %1 · %2 funded").arg(rows, grouped(model_->fundedCount()));
}

QString WalletController::pricesLine() const
{
    // What the figures on screen go by: what the wallets hold, or, while they hold
    // nothing, what there is.
    QList<wallet::Asset> shown;
    QStringList unpriced;
    for (const wallet::AssetInfo& info : wallet::assets()) {
        if (model_->total(info.asset).isZero())
            continue;
        shown.append(info.asset);
        if (prices_->usd(info.asset) <= 0.0)
            unpriced.append(info.symbol);
    }
    if (shown.isEmpty()) {
        for (const wallet::AssetInfo& info : wallet::assets())
            shown.append(info.asset);
    }
    const QDateTime oldest = prices_->oldest(shown);
    QStringList parts;
    if (oldest.isValid()) {
        parts << ui::trText(QStringLiteral("Prices: %1 · %2")).arg(prices_->sources(shown), clockTime(oldest));
        if (oldest.secsTo(QDateTime::currentDateTimeUtc()) > kPricesOldSeconds)
            parts << ui::trText(QStringLiteral("not updated since"));
        else if (prices_->lastRefreshFailed())
            parts << ui::trText(QStringLiteral("refresh failing"));
    } else {
        parts << (prices_->lastRefreshFailed() ? ui::trText(QStringLiteral("Prices: could not be loaded"))
                                               : ui::trText(QStringLiteral("Prices: not loaded yet")));
    }
    if (!unpriced.isEmpty() && oldest.isValid())
        parts << ui::trText(QStringLiteral("no price for %1")).arg(unpriced.join(QStringLiteral(", ")));
    QStringList doubted;
    for (const wallet::AssetInfo& info : wallet::assets()) {
        if (shown.contains(info.asset) && prices_->inDoubt(info.asset))
            doubted.append(info.symbol);
    }
    if (!doubted.isEmpty())
        parts << ui::trText(QStringLiteral("sources disagree on %1")).arg(doubted.join(QStringLiteral(", ")));
    return parts.join(QLatin1String(" · "));
}

wallet::UtxoHistory* WalletController::historyOf(wallet::Network network) const
{
    const QList<wallet::UtxoHistory*> all = scheduler_->histories();
    for (wallet::UtxoHistory* history : all) {
        if (history->network() == network)
            return history;
    }
    return nullptr;
}

QString WalletController::labelsBlocker() const
{
    if (!hasWallet())
        return ui::trText(QStringLiteral("Add a wallet first: labels and notes are kept with it."));
    if (waitingForLedger_ || ledger_->isLoading())
        return ui::trText(QStringLiteral("What is saved is still being read. Try again in a moment."));
    if (importingLabels_)
        return ui::trText(QStringLiteral("A file of labels is still going in. Try again in a moment."));
    if (!ledger_->isOpen())
        return ui::trText(QStringLiteral("Labels and notes are not being kept: %1.")).arg(ledgerClosedWhy_.isEmpty() ? ui::trText(QStringLiteral("another GitWallet window holds this folder")) : ledgerClosedWhy_);
    if (ledger_->labelsOwed())
        return ui::trText(QStringLiteral("The labels and notes of an older file have yet to come over, and until they have none can be changed. This is tried again every minute: free some disk space if the disk is full."));
    return {};
}

bool WalletController::holdsSecret(const QString& text, const QSet<chain::Hash256>& alsoKnown) const
{
    if (!wallet::looksLikeSecret(text))
        return false;
    // Copy gives a transaction's id, and a note on a refund names one. That many hex digits read as a
    // key, unless they are an id these wallets know.
    static const QRegularExpression hex(QStringLiteral("\\b[0-9a-fA-F]{64}\\b"));
    QString rest = text;
    const QList<wallet::UtxoHistory*> histories = scheduler_->histories();
    for (auto it = hex.globalMatch(text); it.hasNext();) {
        const QString token = it.next().captured();
        chain::Hash256 txid{};
        chain::hashFromHex(token.toStdString(), &txid);
        if (alsoKnown.contains(txid) || std::any_of(histories.cbegin(), histories.cend(), [&txid](const wallet::UtxoHistory* history) { return history->book()->find(txid) >= 0; }))
            rest.replace(token, QStringLiteral(" "));
    }
    return wallet::looksLikeSecret(rest);
}

bool WalletController::setTxNote(wallet::Network network, const chain::Hash256& txid, const wallet::TxNote& note, QString* error)
{
    setError(error, QString());
    wallet::UtxoHistory* history = historyOf(network);
    if (!history)
        return false;
    wallet::TxNote clean{note.label.trimmed(), note.note.trimmed()};
    // Whatever goes into a label ends up in an export. Like a wallet's name, it never holds a secret.
    if (holdsSecret(clean.label) || holdsSecret(clean.note)) {
        setError(error, QStringLiteral("That looks like seed words or a private key, so it was not kept."));
        return false;
    }
    const QString blocker = labelsBlocker();
    if (!blocker.isEmpty()) {
        setError(error, blocker);
        return false;
    }
    ledger_->saveNote(network, txid, clean);
    history->book()->setNote(txid, clean);
    return true;
}

bool WalletController::setAddressLabel(const QString& address, const QString& label, QString* error)
{
    setError(error, QString());
    const QString clean = label.trimmed();
    if (holdsSecret(clean)) {
        setError(error, QStringLiteral("That looks like seed words or a private key, so it was not kept."));
        return false;
    }
    const QString blocker = labelsBlocker();
    if (!blocker.isEmpty()) {
        setError(error, blocker);
        return false;
    }
    ledger_->saveAddressLabel(address, clean);
    if (clean.isEmpty())
        addressLabels_.remove(address);
    else
        addressLabels_.insert(address, clean);
    emit labelsChanged();
    return true;
}

bool WalletController::hasLabels() const
{
    return labelCount() > 0;
}

int WalletController::labelCount() const
{
    int count = int(addressLabels_.size());
    const QList<wallet::UtxoHistory*> histories = scheduler_->histories();
    for (const wallet::UtxoHistory* history : histories)
        count += int(history->book()->notes().size());
    return count;
}

int WalletController::labelCount(quint32 wallet) const
{
    int count = 0;
    for (int row = 0; row < model_->size(); ++row) {
        const wallet::AddressRow& r = model_->at(row);
        if (r.wallet == wallet && addressLabels_.contains(r.address))
            ++count;
    }
    const QList<wallet::UtxoHistory*> histories = scheduler_->histories();
    for (const wallet::UtxoHistory* history : histories) {
        const wallet::HistoryBook* book = history->book();
        const QHash<chain::Hash256, wallet::TxNote>& notes = book->notes();
        for (auto it = notes.cbegin(); it != notes.cend(); ++it) {
            const int position = book->find(it.key());
            if (position < 0)
                continue;
            const QVector<wallet::TxEffect>& effects = book->effectsOf(position);
            if (!effects.isEmpty() && std::all_of(effects.cbegin(), effects.cend(), [book, wallet](const wallet::TxEffect& effect) { return book->addressAt(effect.address).wallet == wallet; }))
                ++count;
        }
    }
    return count;
}

namespace {

// Everything a file of labels says, as it stands this moment: shared, so that it can be written
// elsewhere while the window goes on.
struct LabelsNow {
    QList<QPair<QString, QHash<chain::Hash256, wallet::TxNote>>> notes;
    QHash<QString, QString> addresses;
};

QByteArray labelsText(const LabelsNow& now)
{
    QByteArray out;
    for (const auto& network : now.notes) {
        for (auto it = network.second.cbegin(); it != network.second.cend(); ++it) {
            wallet::LabelLine line;
            line.txid = it.key();
            line.network = network.first;
            line.label = it->label;
            line.note = it->note;
            out += wallet::writeLabelLine(line);
        }
    }
    for (auto it = now.addresses.cbegin(); it != now.addresses.cend(); ++it) {
        wallet::LabelLine line;
        line.isTransaction = false;
        line.address = it.key();
        line.label = it.value();
        out += wallet::writeLabelLine(line);
    }
    return out;
}

// Off this thread: what `work` makes of its own copies comes back to `owner`.
template <typename Work, typename Done>
QThread* offThisThread(QObject* owner, Work work, Done done)
{
    using Result = std::invoke_result_t<Work>;
    auto result = std::make_shared<Result>();
    QThread* thread = QThread::create([work = std::move(work), result]() mutable { *result = work(); });
    QObject::connect(thread, &QThread::finished, owner, [result, done = std::move(done)]() mutable { done(*result); });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    return thread;
}

// What it would be without any run of hex digits long enough to be a transaction's id.
QString withoutIds(const QString& text)
{
    static const QRegularExpression hex(QStringLiteral("\\b[0-9a-fA-F]{64}\\b"));
    QString rest = text;
    return rest.replace(hex, QStringLiteral(" "));
}

} // namespace

QByteArray WalletController::labelsFile() const
{
    LabelsNow now;
    const QList<wallet::UtxoHistory*> histories = scheduler_->histories();
    for (const wallet::UtxoHistory* history : histories)
        now.notes.append({wallet::networkInfo(history->network()).id, history->book()->notes()});
    now.addresses = addressLabels_;
    return labelsText(now);
}

void WalletController::writeLabelsFile(const QString& path, QObject* guard, const std::function<void(bool, const QString&)>& done)
{
    LabelsNow now;
    const QList<wallet::UtxoHistory*> histories = scheduler_->histories();
    for (const wallet::UtxoHistory* history : histories)
        now.notes.append({wallet::networkInfo(history->network()).id, history->book()->notes()});
    now.addresses = addressLabels_;
    struct Written {
        bool ok = false;
        QString error;
    };
    copiesBeingWritten_.removeIf([](const QPointer<QThread>& thread) { return thread.isNull(); });
    copiesBeingWritten_ += offThisThread(guard, [now, path] {
        Written written;
        const QByteArray body = labelsText(now);
        QSaveFile file(path);
        written.ok = file.open(QIODevice::WriteOnly) && file.write(body) == body.size() && file.commit();
        if (!written.ok)
            written.error = file.errorString();
        return written;
    }, [done](const Written& written) { done(written.ok, written.error); });
}

// One file of labels on its way in.
struct LabelImport {
    QVector<wallet::LabelLine> lines;
    QSet<int> suspect; // a field looks like a secret, unless what looks so is an id these wallets know
    QSet<chain::Hash256> named; // what the lines themselves are about: no secret either
    qsizetype at = 0;
    int added = 0;
    int left = 0;
    int later = 0;
    bool anyAddress = false;
    QPointer<QObject> guard;
    std::function<void(int, int, int, const QString&)> done;
};

void WalletController::importLabelsFile(const QString& path, QObject* guard, const std::function<void(int, int, int, const QString&)>& done)
{
    const QString blocker = labelsBlocker();
    if (!blocker.isEmpty()) {
        done(-1, 0, 0, blocker);
        return;
    }
    importingLabels_ = true;
    struct Read {
        QVector<wallet::LabelLine> lines;
        QSet<int> suspect;
        QSet<chain::Hash256> named;
        int left = 0;
        QString error;
    };
    const QPointer<QObject> stillThere(guard);
    offThisThread(this, [path] {
        Read read;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            read.error = QStringLiteral("That file could not be read.");
            return read;
        }
        if (file.size() > kMaxLabelsFileBytes) {
            read.error = QStringLiteral("That file is too long to be a file of labels.");
            return read;
        }
        read.lines = wallet::readLabelLines(file.readAll(), &read.left);
        for (int i = 0; i < read.lines.size(); ++i) {
            const wallet::LabelLine& line = read.lines.at(i);
            if (line.isTransaction)
                read.named.insert(line.txid);
            if (wallet::looksLikeSecret(line.label) || wallet::looksLikeSecret(line.note))
                read.suspect.insert(i);
        }
        return read;
    }, [this, stillThere, done](const Read& read) {
        auto import = std::make_shared<LabelImport>();
        import->guard = stillThere;
        import->done = done;
        if (!read.error.isEmpty()) {
            importingLabels_ = false;
            if (stillThere)
                done(-1, 0, 0, read.error);
            return;
        }
        import->lines = read.lines;
        import->suspect = read.suspect;
        import->named = read.named;
        import->left = read.left;
        importNextPart(import);
    });
}

void WalletController::importNextPart(const std::shared_ptr<LabelImport>& import)
{
    const wallet::NetworkInfo* firstUtxo = wallet::firstOf(wallet::Family::Utxo);
    QHash<wallet::Network, QHash<chain::Hash256, wallet::TxNote>> notes;
    QHash<QString, QString> labels;
    const qsizetype to = qMin(import->at + kImportChunk, import->lines.size());
    const bool allRead = historiesRead();
    for (; import->at < to; ++import->at) {
        const wallet::LabelLine& line = import->lines.at(import->at);
        const QString label = line.label.trimmed();
        const QString note = line.note.trimmed();
        // Whatever goes into a label ends up in an export. Like a wallet's name, it never holds a secret.
        if (import->suspect.contains(int(import->at)) && (holdsSecret(label, import->named) || holdsSecret(note, import->named))) {
            // If all that looks like one is an id, it may be a transaction these wallets have yet to
            // read. Once they have read all there is, it is not going to be.
            if (!allRead && !wallet::looksLikeSecret(withoutIds(label)) && !wallet::looksLikeSecret(withoutIds(note)))
                ++import->later;
            else
                ++import->left;
            continue;
        }
        if (line.isTransaction) {
            const wallet::NetworkInfo* network = line.network.isEmpty() ? firstUtxo : wallet::findNetwork(line.network);
            wallet::UtxoHistory* history = network ? historyOf(network->network) : nullptr;
            if (!history || (label.isEmpty() && note.isEmpty())) {
                ++import->left;
                continue;
            }
            // Over what is there only what the line says: most wallets' files have labels alone.
            wallet::TxNote kept = history->book()->noteOf(line.txid);
            if (!label.isEmpty())
                kept.label = label;
            if (!note.isEmpty())
                kept.note = note;
            notes[network->network].insert(line.txid, kept);
            history->book()->setNote(line.txid, kept);
        } else {
            // All an address has is its label: a line without one says nothing of it.
            if (label.isEmpty()) {
                ++import->left;
                continue;
            }
            labels.insert(line.address, label);
            addressLabels_.insert(line.address, label);
            import->anyAddress = true;
        }
        ++import->added;
    }
    for (auto it = notes.cbegin(); it != notes.cend(); ++it)
        ledger_->saveNotes(it.key(), *it);
    if (!labels.isEmpty())
        ledger_->saveAddressLabels(labels);
    if (import->at < import->lines.size()) {
        QTimer::singleShot(0, this, [this, import] { importNextPart(import); });
        return;
    }
    importingLabels_ = false;
    if (import->anyAddress)
        emit labelsChanged();
    if (import->guard)
        import->done(import->added, import->left, import->later, QString());
}

int WalletController::differingAddresses() const
{
    int count = 0;
    const QList<wallet::UtxoHistory*> histories = scheduler_->histories();
    for (const wallet::UtxoHistory* history : histories)
        count += history->progress().differing;
    return count;
}

double WalletController::usdValue(wallet::Asset asset) const
{
    const wallet::AssetInfo& info = wallet::assetInfo(asset);
    return model_->total(asset).toDouble(info.decimals) * prices_->usd(asset);
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
        sum += model_->total(info.asset, wallet).toDouble(info.decimals) * prices_->usd(info.asset);
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
    // What the database kept comes first; onLedgerLoaded() is back for this.
    if (waitingForLedger_)
        return;
    // A copy: signals emitted below may re-enter the controller.
    const wallet::ScanSettings settings = store_.settings;
    scheduler_->configure(store_.allAccounts(), settings);
    if (!scheduler_->hasWork()) {
        running_ = false;
        return; // no wallet: no connections at all
    }
    running_ = true;
    scheduler_->start();
    prices_->refresh();
    priceTimer_->start();
    timesTimer_->start();
}

void WalletController::stopServices()
{
    saveTimes();
    running_ = false;
    scheduler_->stop();
    priceTimer_->stop();
    timesTimer_->stop();
}

void WalletController::openLedger()
{
    // The window that holds the lock is the one that writes; another shows the
    // wallet and reads what the servers say, and asks again: it may be the last
    // window, still closing.
    if (!hasWallet()) {
        startServices();
        return;
    }
    if (!lockDataDir()) {
        startServices();
        ledgerRetries_ = 0;
        ledgerRetry_->start();
        return;
    }
    ledgerClosedWhy_.clear();
    if (store_.databaseKey.isEmpty()) {
        // A wallet.dat from before the database: the key joins it, if it may be written.
        const QString blocker = settingsSaveBlocker();
        QString why = blocker;
        if (blocker.isEmpty()) {
            store_.databaseKey = wallet::WalletStore::newDatabaseKey();
            if (!saveStore(&why))
                store_.databaseKey.clear();
        }
        if (store_.databaseKey.isEmpty()) {
            // Every start is then a whole scan, and at scale that takes a while.
            ledgerClosedWhy_ = why;
            emit errorOccurred(wallet::Problem::SavedData,
                               QStringLiteral("Saved balances are not being kept: %1").arg(why), QString());
            startServices();
            return;
        }
    }
    QSet<quint32> ids;
    for (const wallet::WalletEntry& w : std::as_const(store_.wallets))
        ids.insert(w.id);
    waitingForLedger_ = true;
    // Rows a scan brought have answered; what comes is only what it did not reach.
    rowsBeforeLedger_ = model_->checkedCount();
    showingSaved_ = rowsBeforeLedger_ == 0; // part of what it kept, as it arrives
    savedAt_ = QDateTime();
    ledger_->open(wallet::WalletStore::databasePath(), store_.databaseKey, ids, store_.nextWalletId());
}

void WalletController::tryLedgerAgain()
{
    if (ledger_->isOpen() || waitingForLedger_ || !hasWallet()) {
        ledgerRetry_->stop();
        return;
    }
    if (lockDataDir()) {
        ledgerRetry_->stop();
        stopServices();
        openLedger();
        return;
    }
    if (++ledgerRetries_ == kLedgerRetriesBeforeSaying) {
        emit errorOccurred(wallet::Problem::FolderHeld,
                           QStringLiteral("Saved balances are not being kept: another GitWallet window holds this folder."),
                           QString());
    }
}

void WalletController::onLedgerLoaded(const QList<store::SavedPrice>& prices, const QList<store::SavedTime>& times)
{
    if (!ledger_->isOpen()) {
        // It could not be read after all: what it handed over is no more to go by than the rest.
        ledgerClosedWhy_ = QStringLiteral("the database could not be opened");
        addressLabels_.clear();
        emit labelsChanged();
    }
    // What a wallet removed while the file could not be written left behind.
    QSet<quint32> wallets;
    for (const wallet::WalletEntry& w : std::as_const(store_.wallets))
        wallets.insert(w.id);
    const QList<wallet::HistoryBook*> books = scheduler_->books();
    for (wallet::HistoryBook* book : books)
        book->keepWallets(wallets, store_.nextWalletId());

    QList<net::PriceQuote> quotes;
    for (const store::SavedPrice& saved : prices) {
        if (const wallet::AssetInfo* asset = wallet::findAsset(saved.asset)) {
            net::PriceQuote quote;
            quote.asset = asset->asset;
            quote.usd = saved.usd;
            quote.at = saved.at;
            quote.source = saved.source;
            quotes.append(quote);
        }
    }
    prices_->restore(quotes);

    for (const store::SavedTime& time : times) {
        if (const wallet::NetworkInfo* network = wallet::findNetwork(time.network))
            scheduler_->restoreTimes(network->network, time.at, time.noncesAt, time.sweep);
    }

    // The oldest of what the rows on screen come from.
    showingSaved_ = model_->checkedCount() > rowsBeforeLedger_;
    savedAt_ = QDateTime();
    if (showingSaved_) {
        bool whole = true;
        for (const wallet::NetworkInfo& network : wallet::networks()) {
            if (model_->rowsOn(network.network) == 0)
                continue;
            const auto found = std::find_if(times.cbegin(), times.cend(),
                                            [&network](const store::SavedTime& t) { return t.network == network.id; });
            if (found == times.cend() || !found->at.isValid()) {
                whole = false;
            } else if (!savedAt_.isValid() || found->at < savedAt_) {
                savedAt_ = found->at;
            }
        }
        if (!whole)
            savedAt_ = QDateTime();
    }

    waitingForLedger_ = false;
    startServices();
    emit statusChanged();
    emit totalsChanged();
}

void WalletController::saveTimes()
{
    if (!running_)
        return;
    QList<store::SavedTime> times;
    const QList<wallet::NetworkStatus> all = scheduler_->statuses();
    for (const wallet::NetworkStatus& one : all) {
        // Past what a saved figure can say: the database keeps the rows, not the promise. A
        // sweep of nonces under way counts too: a first scan cut short goes on where it stopped.
        if (one.watched && (one.balancesAt.isValid() || one.sweep.from.isValid()))
            times.append({wallet::networkInfo(one.network).id, one.balancesAt, one.noncesAt, one.sweep});
    }
    if (!times.isEmpty())
        ledger_->saveTimes(times);
}

void WalletController::savePrices()
{
    QList<store::SavedPrice> saved;
    const QList<net::PriceQuote> quotes = prices_->quotes();
    for (const net::PriceQuote& quote : quotes)
        saved.append({wallet::assetInfo(quote.asset).key, quote.usd, quote.at, quote.source});
    if (!saved.isEmpty())
        ledger_->savePrices(saved);
}

void WalletController::queueStatus()
{
    // A throttle, not a debounce: during a long scan the chip still moves.
    if (!statusTimer_->isActive())
        statusTimer_->start();
}

void WalletController::onScanProgress()
{
    // A window cut at the limit keeps a scan from finishing, and everything it does
    // follow has answered all the same.
    if (showingSaved_ && running_ && scheduler_->allAnswered()) {
        showingSaved_ = false;
        saveTimes();
    }
    queueStatus();
}

void WalletController::onScanFinished()
{
    if (!running_)
        return; // a late signal from a run that stopServices() ended
    showingSaved_ = false;
    saveTimes();
    emit statusChanged();
    emit scanFinished();
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
    // limit, an endpoint on another chain) while no progress() says so.
    queueStatus();
}

WalletController::Status WalletController::status() const
{
    if (!hasWallet())
        return Status::NoWallet;
    if (waitingForLedger_)
        return Status::Loading;
    if (scheduler_->anyConnecting())
        return Status::Connecting;
    // A balance kept missing or stale by something no scan cures comes before
    // "Scanning": a window cut at the limit keeps a scan unfinished
    // for good, and a server on another chain, or failing, leaves addresses
    // unanswered round after round.
    if (addressLimitReached())
        return Status::Limit;
    const QList<wallet::NetworkStatus> all = scheduler_->statuses();
    if (std::any_of(all.cbegin(), all.cend(), [](const wallet::NetworkStatus& s) { return s.wrongChain; }))
        return Status::WrongChain;
    if (std::any_of(all.cbegin(), all.cend(), [](const wallet::NetworkStatus& s) { return s.failing; }))
        return Status::Stale;
    if (isScanning())
        return Status::Scanning;
    // Nothing is scanning, yet a row has no answer: its balance is unknown.
    if (model_->checkedCount() < model_->size())
        return Status::Unchecked;
    if (accountUnscanned())
        return Status::Unscanned;
    if (differingAddresses() > 0)
        return Status::DoNotAddUp;
    return Status::UpToDate;
}

bool WalletController::accountUnscanned() const
{
    // An account key that could not be read, or a Bitcoin chain that could
    // not be derived from it, gets no rows at all: the scanners report it
    // (problem(), Skipped) and finish without it. Asked only once nothing is
    // scanning, when every chain that can be scanned has its first rows.
    // Index 1 counts as well, for an index 0 that BIP32 skips for having no key.
    // A key this version does not know is not read at all.
    if (hasUnreadKeys())
        return true;
    const QList<wallet::AccountKey> accounts = store_.allAccounts();
    return std::any_of(accounts.cbegin(), accounts.cend(),
                       [this](const wallet::AccountKey& a) { return !accountHasRows(a); });
}

bool WalletController::accountHasRows(const wallet::AccountKey& a) const
{
    for (const wallet::NetworkInfo& network : wallet::networks()) {
        if (!a.serves(network.network))
            continue;
        const auto hasRows = [&](quint32 change) {
            return model_->find(a.wallet, network.network, a.type, a.account, change, 0) >= 0
                   || model_->find(a.wallet, network.network, a.type, a.account, change, 1) >= 0;
        };
        if (!hasRows(0) || (chain::isUtxo(a.type) && !hasRows(1)))
            return false;
    }
    return true;
}

bool WalletController::walletHasAllRows(const wallet::WalletEntry& entry) const
{
    return entry.keptAccounts.isEmpty() && std::all_of(entry.accounts.cbegin(), entry.accounts.cend(),
                       [this](const wallet::AccountKey& a) { return accountHasRows(a); });
}

bool WalletController::saveStore(QString* why)
{
    // The build that wrote an older file cannot read what comes next: the way back stays,
    // or it is not written. The time in its name: a user who went back and came forward
    // again has another file to keep.
    if (const int older = store_.olderFormat(); older > 0 && !olderCopied_) {
        const QString path = wallet::WalletStore::filePath();
        const QString copy = path + QStringLiteral(".format%1-%2").arg(older).arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
        if (QFile::exists(path) && !QFile::copy(path, copy)) {
            setError(why, QStringLiteral("wallet.dat is from an older version, and a copy of it could not be made, so it was not rewritten."));
            return false;
        }
        olderCopied_ = true;
    }
    // What was written, not a read of the file afterwards: a program holding the
    // file for a moment must not make every later save look like a foreign change.
    QByteArray digest;
    if (!store_.save(why, &digest))
        return false;
    store_.forgetOlderFormat();
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
