#pragma once

#include "wallet/Types.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

namespace wallet {

// What GitWallet keeps on disk: every wallet's account-level public keys
// (xpubs) and name, the scan settings and the key of the database beside it (what the
// wallets have shown so far, see store::LedgerDb) — never a seed or any private key.
// The file (wallet.dat, JSON inside) is encrypted for the current Windows user
// with DPAPI (CryptProtectData), so other accounts on the PC cannot read which
// addresses belong to the wallets. A file of an earlier format loads as it was
// and is written in the current format from then on.
class WalletStore
{
public:
    static constexpr int kMaxWallets = 500;
    static constexpr int kMaxLabelChars = 64;

    // --data-dir if set, else QStandardPaths::AppDataLocation (created on demand).
    static void setDataDir(const QString& dir);
    static QString dataDir();
    static QString filePath(); // dataDir() + "/wallet.dat"
    static QString databasePath(); // dataDir() + "/wallet.db"
    static QByteArray newDatabaseKey();

    bool exists() const;
    bool load(QString* error); // false + error on a damaged / foreign file
    // Atomic (QSaveFile). `digest`, if given, receives the SHA-256 of what was written.
    bool save(QString* error, QByteArray* digest = nullptr) const;
    bool remove(QString* error); // deletes wallet.dat and resets this object

    // From a validated mnemonic: derives the accounts the list of coins captures (now
    // m/84'/0'/0', m/49'/0'/0', m/44'/0'/0' and m/44'/60'/0', account 0), keeps only
    // their xpubs and scrubs the seed and every private intermediate before returning.
    static QList<AccountKey> deriveAccounts(const std::vector<std::string>& words,
                                            const std::string& passphrase);

    // Account-level public keys as other wallets export them. Among `bitcoinKeys`
    // a zpub is native SegWit and a ypub nested SegWit; an xpub is read as `xpubType`,
    // since many wallets write SegWit accounts with that prefix too and the prefix
    // cannot tell. `ethereumKeys` are Ethereum accounts, always xpubs. Each key is
    // kept in the form its type is stored with. False + a sentence naming the key
    // by chain and number, never quoting it.
    static bool accountsFromPublicKeys(const QStringList& bitcoinKeys, const QStringList& ethereumKeys,
                                       chain::ScriptType xpubType, QList<AccountKey>* out, QString* why);

    // What decides a key's addresses: its type, public key and chain code. Two keys
    // with the same id show the same addresses, however their text differs (a
    // parent fingerprint or child number is not part of it). Throws on a key that
    // does not parse.
    static QByteArray addressSetId(const AccountKey& key);

    // A name as stored: one line, without control characters, at most
    // kMaxLabelChars long. May come back empty.
    static QString cleanLabel(const QString& label);

    // The wallet.dat a test needs: `json` encrypted as save() would. Used by
    // --store-test only, on a throw-away --data-dir, for a file of an older format.
    static bool writeRawForTest(const QByteArray& json, QString* error);

    QList<AccountKey> allAccounts() const; // every wallet's, in file order
    const WalletEntry* find(quint32 id) const; // nullptr if none
    quint32 takeWalletId();                    // the id for a new wallet

    // The format of the file load() read when it is older than what save() writes, else 0:
    // the build that wrote it cannot read what save() makes of it.
    int olderFormat() const { return olderFormat_; }
    void forgetOlderFormat() { olderFormat_ = 0; }

    QList<WalletEntry> wallets;
    ScanSettings settings;
    QByteArray databaseKey; // empty until one is made

private:
    quint32 nextWalletId_ = 1;
    int olderFormat_ = 0;
};

} // namespace wallet
