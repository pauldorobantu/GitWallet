#pragma once

#include "wallet/Seal.h"
#include "wallet/Types.h"

#include <QByteArray>
#include <QJsonObject>
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
// and is written in the current format from then on. A password, if there is one, goes on
// top of DPAPI: then the file says nothing to a program running under this Windows account
// either, and neither does the database, whose key is inside.
class WalletStore
{
public:
    static constexpr int kMaxWallets = 500;
    static constexpr int kMaxLabelChars = 64;
    static constexpr int kDefaultLockMinutes = 15;

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

    // load() read a file under a password, which this object holds nothing else of yet: load() was
    // false, and the key of the right password opens it.
    bool needsPassword() const { return !sealed_.isEmpty(); }
    bool sealedCost(seal::Cost* cost) const { return seal::costOf(sealed_, cost); }
    bool openSealed(const QByteArray& key, QString* error);
    // What is saved from now on goes under this key; none: under none.
    bool hasPassword() const { return !passwordKey_.isEmpty(); }
    void setPasswordKey(const QByteArray& key, const seal::Cost& cost);
    const QByteArray& passwordKey() const { return passwordKey_; }
    const seal::Cost& passwordCost() const { return passwordCost_; }
    bool isPasswordKey(const QByteArray& key) const; // in the same time, whatever it is

    // What a backup holds of this: all of the file but the key of the database. And that read back,
    // as far as it holds: `name` is what a problem calls it.
    QByteArray backupBody() const;
    static bool readBackupBody(const QByteArray& json, const QString& name, WalletStore* into, QString* error);

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
    // What is inside DPAPI as it is on disk, for a test to look at.
    static QByteArray readRawForTest(QString* error);

    QList<AccountKey> allAccounts() const; // every wallet's, in file order
    const WalletEntry* find(quint32 id) const; // nullptr if none
    quint32 takeWalletId();                    // the id for a new wallet
    quint32 nextWalletId() const { return nextWalletId_; } // below it, one not in the file was removed from it

    // The format of the file load() read when it is older than what save() writes, else 0:
    // the build that wrote it cannot read what save() makes of it.
    int olderFormat() const { return olderFormat_; }
    void forgetOlderFormat() { olderFormat_ = 0; }

    QList<WalletEntry> wallets;
    ScanSettings settings;
    QByteArray databaseKey; // empty until one is made
    int lockMinutes = kDefaultLockMinutes; // with a password: how long left alone before it locks; 0: never
    qint64 lastBackupMs = 0;  // when a backup was last made here, ms since the epoch; 0: never
    int lastBackupWallets = 0; // how many wallets that held
    quint32 lastBackupNextWallet = 0; // the next id then: a wallet at or past it is not in that backup; 0: not known

private:
    quint32 nextWalletId_ = 1;
    int olderFormat_ = 0;
    QJsonObject sealed_;
    QByteArray passwordKey_;
    seal::Cost passwordCost_;
};

} // namespace wallet
