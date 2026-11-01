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
// (xpubs) and name, plus the scan settings — never a seed or any private key.
// The file (wallet.dat, JSON inside) is encrypted for the current Windows user
// with DPAPI (CryptProtectData), so other accounts on the PC cannot read which
// addresses belong to the wallets. A file of the first format (one wallet) loads
// as one entry and is written in the current format from then on.
class WalletStore
{
public:
    static constexpr int kMaxWallets = 500;
    static constexpr int kMaxLabelChars = 64;

    // --data-dir if set, else QStandardPaths::AppDataLocation (created on demand).
    static void setDataDir(const QString& dir);
    static QString dataDir();
    static QString filePath(); // dataDir() + "/wallet.dat"

    bool exists() const;
    bool load(QString* error);       // false + error on a damaged / foreign file
    bool save(QString* error) const; // atomic (QSaveFile)
    bool remove(QString* error);     // deletes wallet.dat and resets this object

    // From a validated mnemonic: derives m/84'/0'/0', m/49'/0'/0', m/44'/0'/0'
    // and m/44'/60'/0' (account 0), keeps only their xpubs and scrubs the seed
    // and every private intermediate before returning.
    static QList<AccountKey> deriveAccounts(const std::vector<std::string>& words,
                                            const std::string& passphrase);

    // Account-level public keys as other wallets export them: zpub is native
    // SegWit, ypub nested SegWit, and an xpub is Bitcoin legacy or, with
    // `xpubIsEthereum`, an Ethereum account (its prefix cannot tell the two
    // apart). False + a sentence naming the 1-based key, never quoting it.
    static bool accountsFromPublicKeys(const QStringList& keys, bool xpubIsEthereum,
                                       QList<AccountKey>* out, QString* why);

    // A name as stored: one line, without control characters, at most
    // kMaxLabelChars long. May come back empty.
    static QString cleanLabel(const QString& label);

    // The wallet.dat a test needs: `json` encrypted as save() would. Used by
    // --store-test only, on a throw-away --data-dir, for a file of an older format.
    static bool writeRawForTest(const QByteArray& json, QString* error);

    QList<AccountKey> allAccounts() const; // every wallet's, in file order
    const WalletEntry* find(quint32 id) const; // nullptr if none
    quint32 takeWalletId();                    // the id for a new wallet

    QList<WalletEntry> wallets;
    ScanSettings settings;

private:
    quint32 nextWalletId_ = 1;
};

} // namespace wallet
