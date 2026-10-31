#pragma once

#include "wallet/Types.h"

#include <QList>
#include <QString>

#include <string>
#include <vector>

namespace wallet {

// What GitWallet keeps on disk: the account-level public keys (xpubs), the scan
// settings and a label — never the seed or any private key. The file
// (wallet.dat, JSON inside) is encrypted for the current Windows user with
// DPAPI (CryptProtectData), so other accounts on the PC cannot read which
// addresses belong to the wallet.
class WalletStore
{
public:
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

    QString label = QStringLiteral("Wallet");
    QList<AccountKey> accounts;
    ScanSettings settings;
};

} // namespace wallet
