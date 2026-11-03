#include "wallet/WalletStore.h"

#include "chain/Hash.h"

#include <botan/system_rng.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

#if !defined(_WIN32)
#error "WalletStore encrypts wallet.dat with Windows DPAPI"
#endif

// The build sets WIN32_LEAN_AND_MEAN, which leaves wincrypt / dpapi out of
// windows.h, so they are included explicitly.
#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>

namespace wallet {

namespace {

constexpr int kFormatVersion = 4; // what was given out: a build that does not know it would write it away
constexpr int kLegacyFormatVersion = 1;    // one wallet: "label" + "accounts" at the top
constexpr int kMostLockMinutes = 24 * 60;
constexpr qint64 kMaxFileBytes = 16 << 20; // room for the account keys of every network kMaxWallets may hold
constexpr qsizetype kMaxXpubChars = 128;   // a serialized extended key is 111 chars
constexpr qsizetype kDatabaseKeyBytes = 32;

// A server list holds a handful of host:port entries or URLs. The caps bound
// the worst case (32 lists x 100 x 512 chars, at most 6 JSON bytes per char)
// near 10 MB, inside kMaxFileBytes.
constexpr qsizetype kMaxListEntries = 100;
constexpr qsizetype kMaxEntryChars = 512;
constexpr qsizetype kMaxServerLists = 32;

constexpr size_t kBip39Rounds = 2048; // PBKDF2 iterations fixed by BIP39

// Every setting is clamped to these ranges on load and on save.
constexpr int kMinGap = 20;
constexpr int kMaxGap = 5000;
constexpr int kMinRefresh = 15;
constexpr int kMaxRefresh = 3600;

struct StoreData {
    QList<WalletEntry> wallets;
    quint32 nextWalletId = 1;
    ScanSettings settings;
    QByteArray databaseKey;
    int format = 0;
    int lockMinutes = WalletStore::kDefaultLockMinutes;
    qint64 lastBackupMs = 0;
    int lastBackupWallets = 0;
    quint32 lastBackupNextWallet = 0;
};

// What a file under a password says it is, inside DPAPI.
QString sealedFormat()
{
    return QStringLiteral("gitwallet-sealed");
}

QString& dataDirOverride()
{
    static QString dir; // from --data-dir; GUI thread only
    return dir;
}

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

QString nativePath(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

QString windowsError(DWORD code)
{
    return QStringLiteral("Windows error 0x%1")
        .arg(QString::number(code, 16).toUpper().rightJustified(8, QLatin1Char('0')));
}

void scrub(QByteArray& bytes)
{
    // data() on a null array would allocate just to be zeroed.
    if (!bytes.isEmpty()) SecureZeroMemory(bytes.data(), static_cast<size_t>(bytes.size()));
}

// ---------------------------------------------------------------- DPAPI ----

// Owns a buffer DPAPI allocated with LocalAlloc and frees it on every path;
// a plaintext buffer is zeroed first.
class DpapiBuffer
{
public:
    explicit DpapiBuffer(bool plaintext) : plaintext_(plaintext) {}
    ~DpapiBuffer()
    {
        if (!blob.pbData) return;
        if (plaintext_) SecureZeroMemory(blob.pbData, blob.cbData);
        LocalFree(blob.pbData);
    }
    DpapiBuffer(const DpapiBuffer&) = delete;
    DpapiBuffer& operator=(const DpapiBuffer&) = delete;

    DATA_BLOB blob{};

private:
    bool plaintext_ = false;
};

// Optional entropy ties the blob to this file format. It is not a secret (it
// ships in the binary); it only means a generic tool that unprotects every
// DPAPI blob of this user cannot read wallet.dat without GitWallet's value.
DATA_BLOB entropyBlob()
{
    static char text[] = "GitWallet wallet.dat v1"; // DPAPI takes a non-const pointer but only reads
    return DATA_BLOB{static_cast<DWORD>(sizeof(text) - 1), reinterpret_cast<BYTE*>(text)};
}

DATA_BLOB inputBlob(const QByteArray& bytes)
{
    return DATA_BLOB{static_cast<DWORD>(bytes.size()),
                     reinterpret_cast<BYTE*>(const_cast<char*>(bytes.constData()))};
}

// CRYPTPROTECT_UI_FORBIDDEN: fail instead of showing a prompt. No
// CRYPTPROTECT_LOCAL_MACHINE: only the current Windows user can decrypt.
bool protect(const QByteArray& plain, QByteArray* blobOut, DWORD* codeOut)
{
    DATA_BLOB in = inputBlob(plain);
    DATA_BLOB entropy = entropyBlob();
    DpapiBuffer out(false);
    if (!CryptProtectData(&in, L"GitWallet", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &out.blob)) {
        *codeOut = GetLastError();
        return false;
    }
    *blobOut = QByteArray(reinterpret_cast<const char*>(out.blob.pbData),
                          static_cast<qsizetype>(out.blob.cbData));
    return true;
}

bool unprotect(const QByteArray& blob, QByteArray* plainOut, DWORD* codeOut)
{
    DATA_BLOB in = inputBlob(blob);
    DATA_BLOB entropy = entropyBlob();
    DpapiBuffer out(true);
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &out.blob)) {
        *codeOut = GetLastError();
        return false;
    }
    *plainOut = QByteArray(reinterpret_cast<const char*>(out.blob.pbData),
                           static_cast<qsizetype>(out.blob.cbData));
    return true;
}

// ------------------------------------------------------------- accounts ----

QString typeId(chain::ScriptType t)
{
    return QLatin1String(chain::scriptTypeId(t));
}

// The network a UTXO type's key is on, or Bitcoin's when the file does not say, wherever
// the list puts Bitcoin.
const NetworkInfo* utxoNetwork(const QJsonObject& o)
{
    const QString id = o.value(QStringLiteral("network")).toString();
    return findNetwork(id.isEmpty() ? QStringLiteral("bitcoin") : id);
}

QString versionName(uint32_t version)
{
    switch (version) {
    case chain::xpubVersion::kZpub: return QStringLiteral("zpub");
    case chain::xpubVersion::kYpub: return QStringLiteral("ypub");
    case chain::xpubVersion::kXpub: return QStringLiteral("xpub");
    }
    return QStringLiteral("an unknown version");
}

// The key must parse, and its version must be the one its type is stored
// with: other wallets read the script type from that prefix when the user
// copies the key from Settings, so a mismatch would show them other addresses.
bool checkAccount(const AccountKey& k, QString* why)
{
    const QString name = QString::fromLatin1(chain::scriptTypeName(k.type));
    if (k.account >= 0x80000000u)
        return fail(why, QStringLiteral("the %1 account number %2 is out of range").arg(name).arg(k.account));
    if (k.xpub.isEmpty() || k.xpub.size() > kMaxXpubChars)
        return fail(why, QStringLiteral("the %1 account key is missing or malformed").arg(name));

    chain::HdPubKey node;
    uint32_t version = 0;
    std::string parseError;
    bool parsed = false;
    try {
        parsed = chain::HdPubKey::parse(k.xpub.toStdString(), &node, &version, &parseError);
    } catch (const std::exception& e) {
        parseError = e.what();
    }
    if (!parsed)
        return fail(why, QStringLiteral("the %1 account key is invalid (%2)")
                             .arg(name, QString::fromStdString(parseError)));

    const uint32_t expected = chain::xpubVersionFor(k.type);
    if (version != expected)
        return fail(why, QStringLiteral("the %1 account key is serialized as %2 instead of %3")
                             .arg(name, versionName(version), versionName(expected)));
    return true;
}

bool checkAccounts(const QList<AccountKey>& accounts, QString* why)
{
    QSet<QString> seen;
    for (const AccountKey& k : accounts) {
        if (!checkAccount(k, why)) return false;
        const QString id = typeId(k.type) + QLatin1Char('/') + QString::number(int(k.network)) + QLatin1Char('/')
                           + QString::number(k.account);
        if (seen.contains(id))
            return fail(why, QStringLiteral("the %1 account %2 is listed twice")
                                 .arg(QString::fromLatin1(chain::scriptTypeName(k.type)))
                                 .arg(k.account));
        seen.insert(id);
    }
    return true;
}

// Shared by save() and load(), so a file save() writes always loads again. The
// same key in two wallets would make both claim one set of addresses.
bool checkWallets(const QList<WalletEntry>& wallets, QString* why)
{
    if (wallets.size() > WalletStore::kMaxWallets)
        return fail(why, QStringLiteral("it lists more than %1 wallets").arg(WalletStore::kMaxWallets));
    QSet<quint32> ids;
    QSet<QString> keys;
    for (const WalletEntry& w : wallets) {
        if (w.id == 0 || ids.contains(w.id))
            return fail(why, QStringLiteral("a wallet has a missing or repeated number"));
        ids.insert(w.id);
        // One whose keys are all of a kind this version does not know is still a wallet.
        if (w.accounts.isEmpty() && w.keptAccounts.isEmpty())
            return fail(why, QStringLiteral("a wallet has no account keys"));
        if (!checkAccounts(w.accounts, why)) return false;
        for (const AccountKey& k : w.accounts) {
            // With its type: the same text is another set of addresses as a legacy
            // account and as an Ethereum one, or on another network.
            const QString key = typeId(k.type) + QLatin1Char('/') + QString::number(int(k.network)) + QLatin1Char('/') + k.xpub;
            if (keys.contains(key))
                return fail(why, QStringLiteral("an account key is listed in two wallets"));
            keys.insert(key);
        }
    }
    return true;
}

// A whole JSON number in [0, 2^31): the hardened account index.
bool readAccountIndex(const QJsonValue& v, quint32* out)
{
    if (!v.isDouble()) return false;
    const double d = v.toDouble();
    if (!(d >= 0.0 && d < 2147483648.0) || d != std::floor(d)) return false;
    *out = static_cast<quint32>(d);
    return true;
}

// ------------------------------------------------------------- settings ----

QString labelOrDefault(const QString& label)
{
    const QString clean = WalletStore::cleanLabel(label);
    return clean.isEmpty() ? QStringLiteral("Wallet") : clean;
}

// Trimmed, no blanks, no repeats; the order is the user's preference order.
// An entry too long to be a host or URL is dropped rather than cut into a
// different one, and entries past the cap are dropped, so a huge paste can
// neither stall the GUI thread here nor make wallet.dat too large to load.
QStringList cleanList(const QStringList& in)
{
    QStringList out;
    QSet<QString> seen; // case-folded: "Host:1" and "host:1" are one entry
    for (const QString& s : in) {
        if (out.size() >= kMaxListEntries) break;
        const QString t = s.trimmed();
        if (t.isEmpty() || t.size() > kMaxEntryChars) continue;
        const QString key = t.toCaseFolded();
        if (seen.contains(key)) continue;
        seen.insert(key);
        out.append(t);
    }
    return out;
}

ScanSettings sanitized(ScanSettings s)
{
    s.utxoReceiveGap = std::clamp(s.utxoReceiveGap, kMinGap, kMaxGap);
    s.utxoChangeGap = std::clamp(s.utxoChangeGap, kMinGap, kMaxGap);
    s.evmGap = std::clamp(s.evmGap, kMinGap, kMaxGap);
    s.evmRefreshSeconds = std::clamp(s.evmRefreshSeconds, kMinRefresh, kMaxRefresh);
    QMap<QString, QStringList> servers;
    for (auto it = s.servers.cbegin(); it != s.servers.cend() && servers.size() < kMaxServerLists; ++it) {
        const QStringList clean = cleanList(it.value());
        if (!clean.isEmpty() && it.key().size() <= kMaxEntryChars)
            servers.insert(it.key(), clean);
    }
    s.servers = servers;
    return s;
}

// A number field as an int, before the range clamp; `fallback` when the field
// is missing or not a number.
int readInt(const QJsonObject& o, const QString& key, int fallback)
{
    const QJsonValue v = o.value(key);
    if (!v.isDouble()) return fallback;
    const double d = v.toDouble();
    if (!std::isfinite(d)) return fallback;
    return static_cast<int>(std::clamp(d, -1.0e9, 1.0e9)); // keeps the conversion defined
}

// The field under its name, or its name in format 2 and before.
int readSetting(const QJsonObject& o, const QString& key, const QString& oldKey, int fallback)
{
    return readInt(o, key, readInt(o, oldKey, fallback));
}

QStringList readList(const QJsonObject& o, const QString& key)
{
    QStringList out;
    const QJsonArray array = o.value(key).toArray(); // empty if missing or not an array
    for (const QJsonValue& item : array)
        if (item.isString()) out.append(item.toString());
    return out;
}

// ------------------------------------------------------------ file body ----

QByteArray toJson(const QList<WalletEntry>& wallets, quint32 nextWalletId, const ScanSettings& s,
                  const QByteArray& databaseKey, int lockMinutes = WalletStore::kDefaultLockMinutes, qint64 lastBackupMs = 0, int lastBackupWallets = 0, quint32 lastBackupNextWallet = 0)
{
    QJsonArray entries;
    for (const WalletEntry& w : wallets) {
        QJsonArray list;
        for (const AccountKey& k : w.accounts) {
            QJsonObject a;
            a.insert(QStringLiteral("type"), typeId(k.type));
            if (chain::isUtxo(k.type))
                a.insert(QStringLiteral("network"), networkInfo(k.network).id);
            a.insert(QStringLiteral("account"), static_cast<qint64>(k.account));
            a.insert(QStringLiteral("xpub"), k.xpub);
            if (!k.given.isEmpty()) {
                QJsonArray runs;
                for (const GivenOut::Run& run : k.given.runs)
                    runs.append(QJsonArray{static_cast<qint64>(run.from), static_cast<qint64>(run.to)});
                a.insert(QStringLiteral("given"), runs);
            }
            list.append(a);
        }
        for (const QJsonValue& kept : w.keptAccounts)
            list.append(kept);
        QJsonObject entry;
        entry.insert(QStringLiteral("id"), static_cast<qint64>(w.id));
        entry.insert(QStringLiteral("label"), labelOrDefault(w.label));
        entry.insert(QStringLiteral("fromKeys"), w.fromKeys);
        entry.insert(QStringLiteral("accounts"), list);
        entries.append(entry);
    }

    QJsonObject settings;
    settings.insert(QStringLiteral("utxoReceiveGap"), s.utxoReceiveGap);
    settings.insert(QStringLiteral("utxoChangeGap"), s.utxoChangeGap);
    settings.insert(QStringLiteral("evmGap"), s.evmGap);
    settings.insert(QStringLiteral("evmRefreshSeconds"), s.evmRefreshSeconds);
    QJsonObject servers;
    for (auto it = s.servers.cbegin(); it != s.servers.cend(); ++it)
        servers.insert(it.key(), QJsonArray::fromStringList(it.value()));
    settings.insert(QStringLiteral("servers"), servers);

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("gitwallet"));
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("nextWalletId"), static_cast<qint64>(nextWalletId));
    root.insert(QStringLiteral("wallets"), entries);
    root.insert(QStringLiteral("settings"), settings);
    if (!databaseKey.isEmpty())
        root.insert(QStringLiteral("databaseKey"), QString::fromLatin1(databaseKey.toHex()));
    if (lockMinutes != WalletStore::kDefaultLockMinutes)
        root.insert(QStringLiteral("lockMinutes"), lockMinutes);
    if (lastBackupMs > 0) {
        root.insert(QStringLiteral("lastBackupMs"), lastBackupMs);
        root.insert(QStringLiteral("lastBackupWallets"), lastBackupWallets);
        root.insert(QStringLiteral("lastBackupNextWallet"), qint64(lastBackupNextWallet));
    }
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// An "accounts" array as every format writes it, each key marked as `wallet`'s. A key
// of a type or network this version does not know goes to `kept`, to stay in the file
// unread: a newer version wrote it, or a network is gone from the list for now. Before
// format 3 there were none, so then it means damage. False + why when it is not an
// account list.
bool readAccounts(const QJsonValue& value, quint32 wallet, bool keepUnknown, QList<AccountKey>* out, QJsonArray* kept,
                  QString* why)
{
    if (!value.isArray()) return fail(why, QStringLiteral("it has no account list"));
    const QJsonArray entries = value.toArray();
    for (const QJsonValue& entry : entries) {
        const QJsonObject o = entry.toObject(); // empty for a non-object: fails below
        AccountKey k;
        k.wallet = wallet;
        bool known = chain::scriptTypeFromId(o.value(QStringLiteral("type")).toString().toStdString(), &k.type);
        if (known && chain::isUtxo(k.type)) {
            const NetworkInfo* network = utxoNetwork(o);
            known = network && network->family == Family::Utxo;
            if (known)
                k.network = network->network;
        } else if (known && !firstOf(Family::Evm)) {
            known = false; // a key no scanner here would ever follow
        }
        if (!known) {
            if (!keepUnknown || !o.value(QStringLiteral("type")).isString() || !o.value(QStringLiteral("xpub")).isString())
                return fail(why, QStringLiteral("an account has an unknown type"));
            kept->append(o);
            continue;
        }
        if (!readAccountIndex(o.value(QStringLiteral("account")), &k.account))
            return fail(why, QStringLiteral("an account has an invalid number"));
        k.xpub = o.value(QStringLiteral("xpub")).toString();
        if (o.contains(QStringLiteral("given"))) {
            // In order, none empty, none touching the next: what add() makes.
            quint32 last = 0;
            const QJsonArray runs = o.value(QStringLiteral("given")).toArray();
            for (const QJsonValue& value : runs) {
                const QJsonArray pair = value.toArray();
                GivenOut::Run run;
                if (pair.size() != 2 || !readAccountIndex(pair.at(0), &run.from) || !readAccountIndex(pair.at(1), &run.to) || run.from >= run.to
                    || (!k.given.isEmpty() && run.from <= last))
                    return fail(why, QStringLiteral("an account has an invalid number"));
                k.given.runs.append(run);
                last = run.to;
            }
        } else if (o.contains(QStringLiteral("issued"))) {
            // Two builds wrote format 3 with one stretch. It may take in some that were passed over as used,
            // which is no harm: only what has no history counts as waiting.
            GivenOut::Run run;
            if (!readAccountIndex(o.value(QStringLiteral("issued")), &run.to)
                || (o.contains(QStringLiteral("issuedFrom")) && !readAccountIndex(o.value(QStringLiteral("issuedFrom")), &run.from)) || run.from > run.to)
                return fail(why, QStringLiteral("an account has an invalid number"));
            if (run.from < run.to)
                k.given.runs.append(run);
        }
        out->append(k);
    }
    return true;
}

// Parses and validates the decrypted body. Qt's JSON objects keep their own
// copies of the strings; those are public keys and settings, not secrets.
bool parseStore(const QByteArray& json, const QString& path, StoreData* out, QString* error)
{
    const auto damaged = [&](const QString& why) {
        return fail(error, QStringLiteral("The wallet file %1 is damaged: %2.").arg(nativePath(path), why));
    };

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return damaged(QStringLiteral("its decrypted contents are not valid JSON"));
    const QJsonObject root = doc.object();

    if (root.value(QStringLiteral("format")).toString() != QLatin1String("gitwallet"))
        return fail(error, QStringLiteral("The file %1 is not a GitWallet wallet.").arg(nativePath(path)));
    const QJsonValue version = root.value(QStringLiteral("version"));
    if (version.isDouble() && version.toDouble() > kFormatVersion)
        return fail(error, QStringLiteral("The wallet file %1 was written by a newer version of GitWallet "
                                          "(format %2). Update GitWallet to open it.")
                               .arg(nativePath(path), QString::number(version.toDouble())));
    const bool legacy = version.isDouble() && version.toDouble() == kLegacyFormatVersion;
    const bool known = version.isDouble() && version.toDouble() >= kLegacyFormatVersion
                       && version.toDouble() <= kFormatVersion && version.toDouble() == std::floor(version.toDouble());
    if (!known)
        return damaged(QStringLiteral("unsupported format version"));

    QString why;
    QList<WalletEntry> wallets;
    quint32 nextId = 1;
    if (legacy) {
        // One wallet at the top. Settings saved before the first import made a
        // file without accounts: no wallet yet.
        WalletEntry only;
        only.id = 1;
        only.label = labelOrDefault(root.value(QStringLiteral("label")).toString());
        if (!readAccounts(root.value(QStringLiteral("accounts")), only.id, false, &only.accounts, &only.keptAccounts, &why))
            return damaged(why);
        if (!only.accounts.isEmpty()) wallets.append(only);
        nextId = 2;
    } else {
        const QJsonValue list = root.value(QStringLiteral("wallets"));
        if (!list.isArray()) return damaged(QStringLiteral("it has no wallet list"));
        const QJsonArray entries = list.toArray();
        for (const QJsonValue& value : entries) {
            const QJsonObject o = value.toObject();
            WalletEntry w;
            if (!readAccountIndex(o.value(QStringLiteral("id")), &w.id))
                return damaged(QStringLiteral("a wallet has an invalid number"));
            w.label = labelOrDefault(o.value(QStringLiteral("label")).toString());
            w.fromKeys = o.value(QStringLiteral("fromKeys")).toBool();
            if (!readAccounts(o.value(QStringLiteral("accounts")), w.id, version.toDouble() >= 3, &w.accounts,
                              &w.keptAccounts, &why))
                return damaged(why);
            nextId = std::max(nextId, w.id + 1);
            wallets.append(w);
        }
        // Past every id in the file whatever the field says: an id is never reused.
        quint32 stored = 0;
        if (readAccountIndex(root.value(QStringLiteral("nextWalletId")), &stored))
            nextId = std::max(nextId, stored);
    }
    if (!checkWallets(wallets, &why)) return damaged(why);

    // Settings are not critical: a missing or mistyped field takes its default.
    const QJsonObject so = root.value(QStringLiteral("settings")).toObject();
    const ScanSettings defaults;
    ScanSettings s;
    s.utxoReceiveGap = readSetting(so, QStringLiteral("utxoReceiveGap"), QStringLiteral("btcReceiveGap"), defaults.utxoReceiveGap);
    s.utxoChangeGap = readSetting(so, QStringLiteral("utxoChangeGap"), QStringLiteral("btcChangeGap"), defaults.utxoChangeGap);
    s.evmGap = readSetting(so, QStringLiteral("evmGap"), QStringLiteral("ethGap"), defaults.evmGap);
    s.evmRefreshSeconds =
        readSetting(so, QStringLiteral("evmRefreshSeconds"), QStringLiteral("ethRefreshSeconds"), defaults.evmRefreshSeconds);
    const QJsonObject servers = so.value(QStringLiteral("servers")).toObject();
    for (auto it = servers.constBegin(); it != servers.constEnd(); ++it)
        s.servers.insert(it.key(), readList(servers, it.key()));
    // Format 2 and before: one list for each of the two networks there were.
    const QStringList oldElectrum = readList(so, QStringLiteral("electrumServers"));
    if (!oldElectrum.isEmpty())
        s.servers.insert(QStringLiteral("bitcoin"), oldElectrum);
    const QStringList oldEndpoints = readList(so, QStringLiteral("ethEndpoints"));
    if (!oldEndpoints.isEmpty())
        s.servers.insert(QStringLiteral("ethereum"), oldEndpoints);

    // One that cannot be used counts as none: a new one comes with the next save, and
    // a database under another key is kept aside.
    const QByteArray databaseKey =
        QByteArray::fromHex(root.value(QStringLiteral("databaseKey")).toString().toLatin1());

    out->wallets = std::move(wallets);
    out->nextWalletId = nextId;
    out->settings = sanitized(s);
    out->databaseKey = databaseKey.size() == kDatabaseKeyBytes ? databaseKey : QByteArray();
    out->format = int(version.toDouble());
    out->lockMinutes = std::clamp(readInt(root, QStringLiteral("lockMinutes"), WalletStore::kDefaultLockMinutes), 0, kMostLockMinutes);
    out->lastBackupMs = qint64(root.value(QStringLiteral("lastBackupMs")).toDouble());
    out->lastBackupWallets = readInt(root, QStringLiteral("lastBackupWallets"), 0);
    out->lastBackupNextWallet = quint32(std::max(readInt(root, QStringLiteral("lastBackupNextWallet"), 0), 0));
    return true;
}

// ----------------------------------------------------------------- seed ----

// The BIP39 seed exactly as chain::bip39::toSeed computes it (PBKDF2-HMAC-
// SHA512 over the space-joined words, salt "mnemonic" + passphrase), minus
// what toSeed leaves in freed memory: the whole sentence, a partial copy of it
// each time the string grows, and the salt with the passphrase. Here both
// buffers are sized up front, so nothing is reallocated, and wiped on every
// path, including a throw.
chain::Bytes bip39Seed(const std::vector<std::string>& words, const std::string& passphrase)
{
    std::string sentence;
    chain::Bytes salt;
    const auto wipe = qScopeGuard([&sentence, &salt] {
        chain::secureClear(sentence);
        chain::secureClear(salt);
    });

    size_t length = words.empty() ? 0 : words.size() - 1; // one space between words
    for (const std::string& w : words) length += w.size();
    sentence.reserve(length);
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) sentence.push_back(' ');
        sentence += words[i];
    }

    constexpr std::string_view kSaltPrefix = "mnemonic";
    salt.reserve(kSaltPrefix.size() + passphrase.size());
    salt.insert(salt.end(), kSaltPrefix.begin(), kSaltPrefix.end());
    salt.insert(salt.end(), passphrase.begin(), passphrase.end());

    return chain::pbkdf2HmacSha512(sentence, salt, kBip39Rounds);
}

} // namespace

// ------------------------------------------------------------ WalletStore ----

void WalletStore::setDataDir(const QString& dir)
{
    const QString d = dir.trimmed();
    dataDirOverride() = d.isEmpty() ? QString() : QDir::cleanPath(QDir(d).absolutePath());
}

QString WalletStore::dataDir()
{
    QString dir = dataDirOverride();
    if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    // An empty location would make filePath() relative to the working directory.
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/GitWallet");
    QDir().mkpath(dir); // created on demand; save() reports it if this failed
    return dir;
}

QString WalletStore::filePath()
{
    return dataDir() + QStringLiteral("/wallet.dat");
}

QString WalletStore::databasePath()
{
    return dataDir() + QStringLiteral("/wallet.db");
}

QByteArray WalletStore::newDatabaseKey()
{
    QByteArray key(kDatabaseKeyBytes, Qt::Uninitialized);
    Botan::system_rng().randomize(reinterpret_cast<uint8_t*>(key.data()), size_t(key.size()));
    return key;
}

bool WalletStore::exists() const
{
    return QFileInfo(filePath()).isFile();
}

bool WalletStore::load(QString* error)
{
    const QString path = filePath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("Cannot open the wallet file %1: %2")
                               .arg(nativePath(path), file.errorString()));
    const qint64 size = file.size();
    if (size <= 0 || size > kMaxFileBytes)
        return fail(error, QStringLiteral("The wallet file %1 is damaged: it is empty or far too large.")
                               .arg(nativePath(path)));
    const QByteArray blob = file.readAll();
    if (blob.size() != size)
        return fail(error, QStringLiteral("Cannot read the wallet file %1: %2")
                               .arg(nativePath(path), file.errorString()));
    file.close();

    StoreData data;
    {
        QByteArray json;
        const auto wipe = qScopeGuard([&json] { scrub(json); });
        DWORD code = 0;
        // A blob of another user, a truncated or a modified file all fail here:
        // DPAPI authenticates what it decrypts.
        if (!unprotect(blob, &json, &code))
            return fail(error, QStringLiteral("This wallet file belongs to another Windows user or is "
                                              "damaged (%1).")
                                   .arg(windowsError(code)));
        // Under a password: it stays as it came until the key of one opens it.
        const QJsonObject outer = QJsonDocument::fromJson(json).object();
        if (outer.value(QStringLiteral("format")).toString() == sealedFormat()) {
            seal::Cost cost;
            if (outer.value(QStringLiteral("version")).toInt() != 1 || !seal::costOf(outer, &cost))
                return fail(error, QStringLiteral("The wallet file %1 was written by a newer version of GitWallet, or is damaged.").arg(nativePath(path)));
            const QJsonObject sealed = outer;
            *this = WalletStore();
            sealed_ = sealed;
            return fail(error, QStringLiteral("The wallet file %1 is under a password.").arg(nativePath(path)));
        }
        if (!parseStore(json, path, &data, error)) return false;
    } // the plaintext is zeroed here

    // Only a fully valid file replaces what this object holds.
    wallets = std::move(data.wallets);
    nextWalletId_ = data.nextWalletId;
    settings = std::move(data.settings);
    databaseKey = std::move(data.databaseKey);
    lockMinutes = data.lockMinutes;
    lastBackupMs = data.lastBackupMs;
    lastBackupWallets = data.lastBackupWallets;
    lastBackupNextWallet = data.lastBackupNextWallet;
    olderFormat_ = data.format < kFormatVersion ? data.format : 0;
    sealed_ = {};
    passwordKey_.clear();
    return true;
}

bool WalletStore::openSealed(const QByteArray& key, QString* error)
{
    QByteArray json;
    const auto wipe = qScopeGuard([&json] { scrub(json); });
    if (!seal::open(key, sealed_, &json))
        return fail(error, QStringLiteral("Wrong password."));
    StoreData data;
    if (!parseStore(json, filePath(), &data, error))
        return false;
    seal::costOf(sealed_, &passwordCost_);
    passwordKey_ = key;
    sealed_ = {};
    wallets = std::move(data.wallets);
    nextWalletId_ = data.nextWalletId;
    settings = std::move(data.settings);
    databaseKey = std::move(data.databaseKey);
    lockMinutes = data.lockMinutes;
    lastBackupMs = data.lastBackupMs;
    lastBackupWallets = data.lastBackupWallets;
    lastBackupNextWallet = data.lastBackupNextWallet;
    olderFormat_ = data.format < kFormatVersion ? data.format : 0;
    return true;
}

void WalletStore::setPasswordKey(const QByteArray& key, const seal::Cost& cost)
{
    passwordKey_ = key;
    passwordCost_ = cost;
}

bool WalletStore::isPasswordKey(const QByteArray& key) const
{
    if (key.size() != passwordKey_.size() || key.isEmpty())
        return false;
    char differs = 0;
    for (qsizetype i = 0; i < key.size(); ++i)
        differs = char(differs | (key.at(i) ^ passwordKey_.at(i)));
    return differs == 0;
}

QByteArray WalletStore::backupBody() const
{
    return toJson(wallets, nextWalletId_, sanitized(settings), QByteArray());
}

bool WalletStore::readBackupBody(const QByteArray& json, const QString& name, WalletStore* into, QString* error)
{
    StoreData data;
    if (!parseStore(json, name, &data, error))
        return false;
    // Nothing this program wrote goes so far, and whatever goes through what was given out would have to walk it.
    for (const WalletEntry& entry : std::as_const(data.wallets)) {
        for (const AccountKey& key : entry.accounts) {
            if (key.given.end() > quint32(kMaxChainRows))
                return fail(error, QStringLiteral("%1 is damaged: it marks more addresses as given out than a wallet can have.").arg(name));
        }
    }
    into->wallets = std::move(data.wallets);
    into->settings = std::move(data.settings);
    return true;
}

bool WalletStore::save(QString* error, QByteArray* digest) const
{
    const QString path = filePath();
    const QString dir = QFileInfo(path).absolutePath();

    // What this holds is nothing yet: written now, it would take the place of all that is under the password.
    if (needsPassword())
        return fail(error, QStringLiteral("The wallet was not saved: wallet.dat is under a password, and not open yet."));
    QString why;
    if (!checkWallets(wallets, &why))
        return fail(error, QStringLiteral("The wallet was not saved: %1.").arg(why));

    QByteArray blob;
    {
        QByteArray json = toJson(wallets, nextWalletId_, sanitized(settings), databaseKey, lockMinutes, lastBackupMs, lastBackupWallets, lastBackupNextWallet);
        const auto wipe = qScopeGuard([&json] { scrub(json); });
        if (!passwordKey_.isEmpty()) {
            try {
                QByteArray sealed = QJsonDocument(seal::lock(passwordKey_, passwordCost_, json, sealedFormat())).toJson(QJsonDocument::Compact);
                scrub(json);
                json = std::move(sealed);
            } catch (const std::exception&) {
                return fail(error, QStringLiteral("The wallet was not saved: it could not be locked under the password."));
            }
        }
        DWORD code = 0;
        if (!protect(json, &blob, &code))
            return fail(error, QStringLiteral("Windows could not encrypt the wallet file (%1).")
                                   .arg(windowsError(code)));
    } // the plaintext is zeroed here

    // load() rejects a larger file as damaged: writing one would lock the user
    // out until the seed is imported again. The old file stays as it was.
    if (blob.size() > kMaxFileBytes)
        return fail(error, QStringLiteral("The wallet was not saved: the wallet file would be too large "
                                          "to open again."));

    if (!QFileInfo(dir).isDir())
        return fail(error, QStringLiteral("Cannot create the folder %1.").arg(nativePath(dir)));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, QStringLiteral("Cannot write the wallet file %1: %2")
                               .arg(nativePath(path), file.errorString()));
    if (file.write(blob) != blob.size()) {
        const QString reason = file.errorString();
        file.cancelWriting(); // the old wallet.dat stays as it was
        return fail(error, QStringLiteral("Cannot write the wallet file %1: %2").arg(nativePath(path), reason));
    }
    if (!file.commit())
        return fail(error, QStringLiteral("Cannot write the wallet file %1: %2")
                               .arg(nativePath(path), file.errorString()));
    if (digest) *digest = QCryptographicHash::hash(blob, QCryptographicHash::Sha256);
    return true;
}

bool WalletStore::remove(QString* error)
{
    const QString path = filePath();
    QFile file(path);
    if (file.exists() && !file.remove())
        return fail(error, QStringLiteral("Cannot delete the wallet file %1: %2")
                               .arg(nativePath(path), file.errorString()));
    seal::wipe(passwordKey_);
    *this = WalletStore(); // wallets and settings back to their defaults
    return true;
}

bool WalletStore::writeRawForTest(const QByteArray& json, QString* error)
{
    QByteArray blob;
    DWORD code = 0;
    if (!protect(json, &blob, &code))
        return fail(error, windowsError(code));
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly) || file.write(blob) != blob.size() || !file.commit())
        return fail(error, file.errorString());
    return true;
}

QByteArray WalletStore::readRawForTest(QString* error)
{
    QFile file(filePath());
    QByteArray plain;
    DWORD code = 0;
    if (!file.open(QIODevice::ReadOnly) || !unprotect(file.readAll(), &plain, &code)) {
        fail(error, file.errorString());
        return {};
    }
    return plain;
}

QList<AccountKey> WalletStore::allAccounts() const
{
    QList<AccountKey> out;
    for (const WalletEntry& w : wallets)
        out.append(w.accounts);
    return out;
}

const WalletEntry* WalletStore::find(quint32 id) const
{
    for (const WalletEntry& w : wallets) {
        if (w.id == id) return &w;
    }
    return nullptr;
}

quint32 WalletStore::takeWalletId()
{
    return nextWalletId_++;
}

QString WalletStore::cleanLabel(const QString& label)
{
    QString line;
    line.reserve(label.size());
    for (const QChar c : label) {
        if (c.isSpace())
            line.append(QLatin1Char(' '));
        else if (c.category() != QChar::Other_Control && c.category() != QChar::Other_Format)
            line.append(c);
    }
    line = line.simplified();
    if (line.size() > kMaxLabelChars) {
        qsizetype cut = kMaxLabelChars;
        if (line.at(cut - 1).isHighSurrogate()) --cut; // never half a character
        line.truncate(cut);
    }
    return line;
}

QByteArray WalletStore::addressSetId(const AccountKey& key)
{
    const chain::HdPubKey node = key.node();
    QByteArray id;
    id.append(char(static_cast<int>(key.type)));
    id.append(char(static_cast<int>(key.network) & 0xFF)); // the same key is other addresses on another network
    id.append(char(static_cast<int>(key.network) >> 8));
    id.append(reinterpret_cast<const char*>(node.publicKey().data()), qsizetype(node.publicKey().size()));
    id.append(reinterpret_cast<const char*>(node.chainCode().data()), qsizetype(node.chainCode().size()));
    return id;
}

bool WalletStore::accountsFromPublicKeys(const QStringList& bitcoinKeys, const QStringList& ethereumKeys,
                                         chain::ScriptType xpubType, QList<AccountKey>* out, QString* why)
{
    QList<AccountKey> accounts;
    QSet<QByteArray> seen;
    const auto add = [&](const QString& raw, const QString& name, bool ethereum) {
        const QString text = raw.trimmed();
        if (text.size() > kMaxXpubChars)
            return fail(why, QStringLiteral("%1 is too long to be a public key.").arg(name));

        chain::HdPubKey node;
        uint32_t version = 0;
        bool parsed = false;
        try {
            parsed = chain::HdPubKey::parse(text.toStdString(), &node, &version);
        } catch (const std::exception&) {
            parsed = false;
        }
        if (!parsed)
            return fail(why, QStringLiteral("%1 is not a valid %2.")
                                 .arg(name, ethereum ? QStringLiteral("xpub") : QStringLiteral("zpub, ypub or xpub")));

        // Account level is m/purpose'/coin'/account': a key from any other depth
        // would show addresses no wallet of this seed ever handed out.
        constexpr uint32_t kHardened = 0x80000000u;
        if (node.depth() != 3 || (node.childNumber() & kHardened) == 0)
            return fail(why, QStringLiteral("%1 is not an account key (m/purpose'/coin'/account'). "
                                            "Export the account's public key from the other wallet.")
                                 .arg(name));

        AccountKey key;
        key.account = node.childNumber() & ~kHardened;
        const NetworkInfo* utxo = findNetwork(QStringLiteral("bitcoin"));
        if (ethereum ? !firstOf(Family::Evm) : !utxo)
            return fail(why, QStringLiteral("%1: this build follows no network for it.").arg(name));
        if (!ethereum)
            key.network = utxo->network;
        if (ethereum) {
            if (version != chain::xpubVersion::kXpub)
                return fail(why, QStringLiteral("%1 must be an xpub: an Ethereum account has no zpub or ypub.").arg(name));
            key.type = chain::ScriptType::EthAccount;
        } else {
            switch (version) {
            case chain::xpubVersion::kZpub: key.type = chain::ScriptType::P2WPKH; break;
            case chain::xpubVersion::kYpub: key.type = chain::ScriptType::P2SH_P2WPKH; break;
            case chain::xpubVersion::kXpub: key.type = xpubType; break;
            default:
                return fail(why, QStringLiteral("%1 is not a mainnet zpub, ypub or xpub.").arg(name));
            }
        }
        // The form its type is stored with: an xpub of a SegWit account becomes a
        // zpub or ypub, so a copy from the Public keys dialog says what it is.
        key.xpub = QString::fromStdString(node.serialize(chain::xpubVersionFor(key.type)));

        const QByteArray id = addressSetId(key);
        if (seen.contains(id))
            return fail(why, QStringLiteral("%1 repeats another key here.").arg(name));
        seen.insert(id);
        accounts.append(key);
        return true;
    };
    for (qsizetype i = 0; i < bitcoinKeys.size(); ++i) {
        if (!add(bitcoinKeys.at(i), QStringLiteral("Bitcoin key %1").arg(i + 1), false)) return false;
    }
    for (qsizetype i = 0; i < ethereumKeys.size(); ++i) {
        if (!add(ethereumKeys.at(i), QStringLiteral("Ethereum key %1").arg(i + 1), true)) return false;
    }
    QString problem;
    if (!checkAccounts(accounts, &problem)) {
        if (why) *why = QStringLiteral("These keys cannot be added together: %1.").arg(problem);
        return false;
    }
    *out = accounts;
    return true;
}

QList<AccountKey> WalletStore::deriveAccounts(const std::vector<std::string>& words,
                                              const std::string& passphrase)
{
    chain::Bytes seed = bip39Seed(words, passphrase);
    // Runs on return and when a derivation throws. `master` and every node
    // scrub their own private key when destroyed.
    const auto wipe = qScopeGuard([&seed] { chain::secureClear(seed); });
    const chain::HdKey master = chain::HdKey::fromSeed(seed);

    QList<AccountKey> out;
    for (const CapturedAccount& captured : capturedAccounts()) {
        const chain::AddressFormat& format =
            chain::isUtxo(captured.type) ? networkInfo(captured.network).format : chain::defaultFormat(captured.type);
        const chain::HdKey node = master.derivePath(chain::accountRootPath(captured.type, 0, format));
        AccountKey key;
        key.type = captured.type;
        key.network = captured.network;
        key.account = 0;
        key.xpub = QString::fromStdString(node.neuter().serialize(chain::xpubVersionFor(captured.type)));
        out.append(key);
    }
    return out;
}

} // namespace wallet
