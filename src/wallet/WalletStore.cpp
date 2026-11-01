#include "wallet/WalletStore.h"

#include "chain/Hash.h"

#include <QByteArray>
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

constexpr int kFormatVersion = 2;
constexpr int kLegacyFormatVersion = 1;   // one wallet: "label" + "accounts" at the top
constexpr qint64 kMaxFileBytes = 1 << 20; // under 1 KB per wallet, so kMaxWallets fit with room to spare
constexpr qsizetype kMaxXpubChars = 128;  // a serialized extended key is 111 chars

// A server list holds a handful of host:port entries or URLs. The caps bound
// the worst case (2 lists x 100 x 512 chars, at most 6 JSON bytes per char)
// near 600 KB, well inside kMaxFileBytes.
constexpr qsizetype kMaxListEntries = 100;
constexpr qsizetype kMaxEntryChars = 512;

constexpr size_t kBip39Rounds = 2048; // PBKDF2 iterations fixed by BIP39

// Every setting is clamped to these ranges on load and on save.
constexpr int kMinGap = 20;
constexpr int kMaxGap = 5000;
constexpr int kMinRefresh = 15;
constexpr int kMaxRefresh = 3600;

// The accounts an import derives, in the order they are stored and shown.
constexpr chain::ScriptType kAccountTypes[] = {
    chain::ScriptType::P2WPKH,
    chain::ScriptType::P2SH_P2WPKH,
    chain::ScriptType::P2PKH,
    chain::ScriptType::EthAccount,
};

struct StoreData {
    QList<WalletEntry> wallets;
    quint32 nextWalletId = 1;
    ScanSettings settings;
};

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
    switch (t) {
    case chain::ScriptType::P2WPKH:      return QStringLiteral("p2wpkh");
    case chain::ScriptType::P2SH_P2WPKH: return QStringLiteral("p2sh-p2wpkh");
    case chain::ScriptType::P2PKH:       return QStringLiteral("p2pkh");
    case chain::ScriptType::EthAccount:  return QStringLiteral("ethereum");
    }
    return QStringLiteral("unknown");
}

bool typeFromId(const QString& id, chain::ScriptType* out)
{
    for (const chain::ScriptType t : kAccountTypes) {
        if (id == typeId(t)) {
            *out = t;
            return true;
        }
    }
    return false;
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
        const QString id = typeId(k.type) + QLatin1Char('/') + QString::number(k.account);
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
        if (w.accounts.isEmpty())
            return fail(why, QStringLiteral("a wallet has no account keys"));
        if (!checkAccounts(w.accounts, why)) return false;
        for (const AccountKey& k : w.accounts) {
            if (keys.contains(k.xpub))
                return fail(why, QStringLiteral("an account key is listed in two wallets"));
            keys.insert(k.xpub);
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
    s.btcReceiveGap = std::clamp(s.btcReceiveGap, kMinGap, kMaxGap);
    s.btcChangeGap = std::clamp(s.btcChangeGap, kMinGap, kMaxGap);
    s.ethGap = std::clamp(s.ethGap, kMinGap, kMaxGap);
    s.ethRefreshSeconds = std::clamp(s.ethRefreshSeconds, kMinRefresh, kMaxRefresh);
    s.electrumServers = cleanList(s.electrumServers);
    s.ethEndpoints = cleanList(s.ethEndpoints);
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

QStringList readList(const QJsonObject& o, const QString& key)
{
    QStringList out;
    const QJsonArray array = o.value(key).toArray(); // empty if missing or not an array
    for (const QJsonValue& item : array)
        if (item.isString()) out.append(item.toString());
    return out;
}

// ------------------------------------------------------------ file body ----

QByteArray toJson(const QList<WalletEntry>& wallets, quint32 nextWalletId, const ScanSettings& s)
{
    QJsonArray entries;
    for (const WalletEntry& w : wallets) {
        QJsonArray list;
        for (const AccountKey& k : w.accounts) {
            QJsonObject a;
            a.insert(QStringLiteral("type"), typeId(k.type));
            a.insert(QStringLiteral("account"), static_cast<qint64>(k.account));
            a.insert(QStringLiteral("xpub"), k.xpub);
            list.append(a);
        }
        QJsonObject entry;
        entry.insert(QStringLiteral("id"), static_cast<qint64>(w.id));
        entry.insert(QStringLiteral("label"), labelOrDefault(w.label));
        entry.insert(QStringLiteral("fromKeys"), w.fromKeys);
        entry.insert(QStringLiteral("accounts"), list);
        entries.append(entry);
    }

    QJsonObject settings;
    settings.insert(QStringLiteral("btcReceiveGap"), s.btcReceiveGap);
    settings.insert(QStringLiteral("btcChangeGap"), s.btcChangeGap);
    settings.insert(QStringLiteral("ethGap"), s.ethGap);
    settings.insert(QStringLiteral("ethRefreshSeconds"), s.ethRefreshSeconds);
    settings.insert(QStringLiteral("electrumServers"), QJsonArray::fromStringList(s.electrumServers));
    settings.insert(QStringLiteral("ethEndpoints"), QJsonArray::fromStringList(s.ethEndpoints));

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("gitwallet"));
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("nextWalletId"), static_cast<qint64>(nextWalletId));
    root.insert(QStringLiteral("wallets"), entries);
    root.insert(QStringLiteral("settings"), settings);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// An "accounts" array as both formats write it, each key marked as `wallet`'s.
// False + why when it is not one.
bool readAccounts(const QJsonValue& value, quint32 wallet, QList<AccountKey>* out, QString* why)
{
    if (!value.isArray()) return fail(why, QStringLiteral("it has no account list"));
    const QJsonArray entries = value.toArray();
    for (const QJsonValue& entry : entries) {
        const QJsonObject o = entry.toObject(); // empty for a non-object: fails below
        AccountKey k;
        k.wallet = wallet;
        if (!typeFromId(o.value(QStringLiteral("type")).toString(), &k.type))
            return fail(why, QStringLiteral("an account has an unknown type"));
        if (!readAccountIndex(o.value(QStringLiteral("account")), &k.account))
            return fail(why, QStringLiteral("an account has an invalid number"));
        k.xpub = o.value(QStringLiteral("xpub")).toString();
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
    if (!version.isDouble() || (!legacy && version.toDouble() != kFormatVersion))
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
        if (!readAccounts(root.value(QStringLiteral("accounts")), only.id, &only.accounts, &why))
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
            if (!readAccounts(o.value(QStringLiteral("accounts")), w.id, &w.accounts, &why))
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
    s.btcReceiveGap = readInt(so, QStringLiteral("btcReceiveGap"), defaults.btcReceiveGap);
    s.btcChangeGap = readInt(so, QStringLiteral("btcChangeGap"), defaults.btcChangeGap);
    s.ethGap = readInt(so, QStringLiteral("ethGap"), defaults.ethGap);
    s.ethRefreshSeconds = readInt(so, QStringLiteral("ethRefreshSeconds"), defaults.ethRefreshSeconds);
    s.electrumServers = readList(so, QStringLiteral("electrumServers"));
    s.ethEndpoints = readList(so, QStringLiteral("ethEndpoints"));

    out->wallets = std::move(wallets);
    out->nextWalletId = nextId;
    out->settings = sanitized(s);
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
        if (!parseStore(json, path, &data, error)) return false;
    } // the plaintext is zeroed here

    // Only a fully valid file replaces what this object holds.
    wallets = std::move(data.wallets);
    nextWalletId_ = data.nextWalletId;
    settings = std::move(data.settings);
    return true;
}

bool WalletStore::save(QString* error) const
{
    const QString path = filePath();
    const QString dir = QFileInfo(path).absolutePath();

    QString why;
    if (!checkWallets(wallets, &why))
        return fail(error, QStringLiteral("The wallet was not saved: %1.").arg(why));

    QByteArray blob;
    {
        QByteArray json = toJson(wallets, nextWalletId_, sanitized(settings));
        const auto wipe = qScopeGuard([&json] { scrub(json); });
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
    return true;
}

bool WalletStore::remove(QString* error)
{
    const QString path = filePath();
    QFile file(path);
    if (file.exists() && !file.remove())
        return fail(error, QStringLiteral("Cannot delete the wallet file %1: %2")
                               .arg(nativePath(path), file.errorString()));
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

bool WalletStore::accountsFromPublicKeys(const QStringList& keys, bool xpubIsEthereum,
                                         QList<AccountKey>* out, QString* why)
{
    QList<AccountKey> accounts;
    for (qsizetype i = 0; i < keys.size(); ++i) {
        const QString number = QString::number(i + 1);
        const QString text = keys.at(i).trimmed();
        if (text.size() > kMaxXpubChars)
            return fail(why, QStringLiteral("Key %1 is too long to be a public key.").arg(number));

        chain::HdPubKey node;
        uint32_t version = 0;
        bool parsed = false;
        try {
            parsed = chain::HdPubKey::parse(text.toStdString(), &node, &version);
        } catch (const std::exception&) {
            parsed = false;
        }
        if (!parsed)
            return fail(why, QStringLiteral("Key %1 is not a valid zpub, ypub or xpub.").arg(number));

        // Account level is m/purpose'/coin'/account': a key from any other depth
        // would show addresses no wallet of this seed ever handed out.
        constexpr uint32_t kHardened = 0x80000000u;
        if (node.depth() != 3 || (node.childNumber() & kHardened) == 0)
            return fail(why, QStringLiteral("Key %1 is not an account key (m/purpose'/coin'/account'). "
                                            "Export the account's public key from the other wallet.")
                                 .arg(number));

        AccountKey key;
        key.account = node.childNumber() & ~kHardened;
        key.xpub = text;
        switch (version) {
        case chain::xpubVersion::kZpub: key.type = chain::ScriptType::P2WPKH; break;
        case chain::xpubVersion::kYpub: key.type = chain::ScriptType::P2SH_P2WPKH; break;
        case chain::xpubVersion::kXpub:
            key.type = xpubIsEthereum ? chain::ScriptType::EthAccount : chain::ScriptType::P2PKH;
            break;
        default:
            return fail(why, QStringLiteral("Key %1 is not a mainnet zpub, ypub or xpub.").arg(number));
        }
        accounts.append(key);
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
    for (const chain::ScriptType t : kAccountTypes) {
        const chain::HdKey node = master.derivePath(chain::accountRootPath(t, 0));
        AccountKey key;
        key.type = t;
        key.account = 0;
        key.xpub = QString::fromStdString(node.neuter().serialize(chain::xpubVersionFor(t)));
        out.append(key);
    }
    return out;
}

} // namespace wallet
