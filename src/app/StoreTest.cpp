#include "app/StoreTest.h"

#include "chain/Bip32.h"
#include "chain/Bip39.h"
#include "chain/Hash.h"
#include "wallet/Types.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <exception>

namespace app {

namespace {

// Published BIP39 test vectors; none of them is anyone's wallet.
const char* kFirstMnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
const char* kSecondMnemonic = "legal winner thank year wave sausage worth useful legal winner thank yellow";

QList<wallet::AccountKey> accountsOf(const char* mnemonic)
{
    return wallet::WalletStore::deriveAccounts(chain::bip39::split(mnemonic), {});
}

wallet::WalletEntry entryOf(quint32 id, const QString& label, QList<wallet::AccountKey> accounts)
{
    wallet::WalletEntry entry;
    entry.id = id;
    entry.label = label;
    for (wallet::AccountKey& key : accounts)
        key.wallet = id;
    entry.accounts = accounts;
    return entry;
}

// The wallet.dat of the first format, as the release before many wallets wrote it.
QByteArray legacyBody(const QList<wallet::AccountKey>& accounts)
{
    QJsonArray list;
    for (const wallet::AccountKey& key : accounts) {
        const char* type = key.type == chain::ScriptType::P2WPKH        ? "p2wpkh"
                           : key.type == chain::ScriptType::P2SH_P2WPKH ? "p2sh-p2wpkh"
                           : key.type == chain::ScriptType::P2PKH       ? "p2pkh"
                                                                        : "ethereum";
        list.append(QJsonObject{{QStringLiteral("type"), QLatin1String(type)},
                                {QStringLiteral("account"), qint64(key.account)},
                                {QStringLiteral("xpub"), key.xpub}});
    }
    const QJsonObject root{{QStringLiteral("format"), QStringLiteral("gitwallet")},
                           {QStringLiteral("version"), 1},
                           {QStringLiteral("label"), QStringLiteral("Old wallet")},
                           {QStringLiteral("accounts"), list},
                           {QStringLiteral("settings"), QJsonObject{{QStringLiteral("btcReceiveGap"), 321}}}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

wallet::AddressRow rowOf(quint32 id, quint32 index, qint64 sats)
{
    wallet::AddressRow row;
    row.wallet = id;
    row.index = index;
    row.scripthash = QStringLiteral("%1-%2").arg(id).arg(index);
    row.address = QStringLiteral("addr-%1-%2").arg(id).arg(index);
    row.btcConfirmed = sats;
    row.used = sats != 0;
    row.checked = true;
    return row;
}

} // namespace

StoreTestResult runStoreTest()
{
    StoreTestResult result;
    const auto check = [&result](bool ok, const QString& what) {
        ++result.total;
        if (ok) ++result.passed;
        result.report += (ok ? QStringLiteral("ok    ") : QStringLiteral("FAIL  ")) + what + QLatin1Char('\n');
    };

    try {
        const QList<wallet::AccountKey> first = accountsOf(kFirstMnemonic);
        const QList<wallet::AccountKey> second = accountsOf(kSecondMnemonic);
        check(first.size() == 4 && second.size() == 4, QStringLiteral("a seed derives 4 account keys"));

        // A file from before many wallets: one wallet, its name and settings kept.
        QString error;
        check(wallet::WalletStore::writeRawForTest(legacyBody(first), &error),
              QStringLiteral("a first-format wallet.dat was written ") + error);
        wallet::WalletStore store;
        check(store.load(&error), QStringLiteral("a first-format wallet.dat loads ") + error);
        check(store.wallets.size() == 1, QStringLiteral("it is one wallet"));
        const bool legacyOk = !store.wallets.isEmpty() && store.wallets.first().label == QStringLiteral("Old wallet")
                              && store.wallets.first().accounts.size() == 4 && !store.wallets.first().fromKeys;
        check(legacyOk, QStringLiteral("it keeps its name and its 4 keys"));
        check(store.settings.btcReceiveGap == 321, QStringLiteral("it keeps its settings"));
        const QList<wallet::AccountKey> all = store.allAccounts();
        const quint32 firstId = store.wallets.value(0).id;
        check(std::all_of(all.cbegin(), all.cend(), [firstId](const wallet::AccountKey& k) { return k.wallet == firstId; }),
              QStringLiteral("every key names its wallet"));

        // A second wallet, written in the current format and read back.
        const quint32 secondId = store.takeWalletId();
        check(secondId != firstId && secondId != 0, QStringLiteral("a new wallet gets its own id"));
        store.wallets.append(entryOf(secondId, QStringLiteral("Warehouse"), second));
        check(store.save(&error), QStringLiteral("two wallets save ") + error);
        wallet::WalletStore reread;
        check(reread.load(&error), QStringLiteral("two wallets load ") + error);
        check(reread.wallets.size() == 2 && reread.find(secondId)
                  && reread.find(secondId)->label == QStringLiteral("Warehouse"),
              QStringLiteral("both come back, by id and name"));
        check(reread.allAccounts().size() == 8, QStringLiteral("with all 8 keys"));
        check(reread.find(secondId) && reread.find(secondId)->accounts.first().wallet == secondId,
              QStringLiteral("keys read back name their wallet"));
        QFile raw(wallet::WalletStore::filePath());
        const QByteArray bytes = raw.open(QIODevice::ReadOnly) ? raw.readAll() : QByteArray();
        check(!bytes.contains("zpub") && !bytes.contains("Warehouse"), QStringLiteral("the file stays encrypted"));

        // An id is not reused, not even after its wallet went.
        reread.wallets.removeLast();
        check(reread.save(&error), QStringLiteral("one wallet removed saves ") + error);
        wallet::WalletStore afterRemove;
        check(afterRemove.load(&error) && afterRemove.takeWalletId() > secondId,
              QStringLiteral("the removed wallet's id is not handed out again"));

        // One set of addresses has one owner, also in the file.
        wallet::WalletStore repeated;
        repeated.wallets.append(entryOf(repeated.takeWalletId(), QStringLiteral("A"), first));
        repeated.wallets.append(entryOf(repeated.takeWalletId(), QStringLiteral("B"), first));
        check(!repeated.save(&error), QStringLiteral("the same key in two wallets is refused: ") + error);

        // Public keys.
        QList<wallet::AccountKey> keys;
        check(wallet::WalletStore::accountsFromPublicKeys({first.at(0).xpub, first.at(1).xpub}, false, &keys, &error)
                  && keys.size() == 2 && keys.at(0).type == chain::ScriptType::P2WPKH
                  && keys.at(1).type == chain::ScriptType::P2SH_P2WPKH,
              QStringLiteral("zpub and ypub are read as native and nested SegWit"));
        check(wallet::WalletStore::accountsFromPublicKeys({first.at(2).xpub}, false, &keys, &error) && keys.size() == 1
                  && keys.first().type == chain::ScriptType::P2PKH,
              QStringLiteral("an xpub is Bitcoin legacy by default"));
        check(wallet::WalletStore::accountsFromPublicKeys({first.at(3).xpub}, true, &keys, &error) && keys.size() == 1
                  && keys.first().type == chain::ScriptType::EthAccount,
              QStringLiteral("an xpub is Ethereum when asked"));
        check(!wallet::WalletStore::accountsFromPublicKeys({QStringLiteral("not a key")}, false, &keys, &error)
                  && error.contains(QLatin1Char('1')) && !error.contains(QStringLiteral("not a key")),
              QStringLiteral("a bad key is named by its number, never quoted"));
        const chain::HdKey master = chain::HdKey::fromSeed(chain::Bytes(32, 7));
        const QString masterKey = QString::fromStdString(master.neuter().serialize(chain::xpubVersion::kXpub));
        check(!wallet::WalletStore::accountsFromPublicKeys({masterKey}, false, &keys, &error),
              QStringLiteral("a key from another depth is refused"));
        check(!wallet::WalletStore::accountsFromPublicKeys({first.at(0).xpub, first.at(0).xpub}, false, &keys, &error),
              QStringLiteral("the same account twice is refused"));

        // The address model, per wallet.
        wallet::WalletModel model;
        model.upsert(rowOf(1, 0, 1000));
        model.upsert(rowOf(1, 1, 0));
        model.upsert(rowOf(2, 0, 250));
        model.upsert(rowOf(2, 0, 500)); // the same position again: replaced
        const auto btc = [&model](quint32 id) {
            return model.total(wallet::Asset::BTC, id) == chain::Amount::fromU64(id == 1 ? 1000 : 500);
        };
        check(model.size() == 3, QStringLiteral("a row is identified within its wallet"));
        check(btc(1) && btc(2), QStringLiteral("each wallet has its own total"));
        check(model.total(wallet::Asset::BTC) == chain::Amount::fromU64(1500), QStringLiteral("the overall total adds them"));
        check(model.counts(1).rows == 2 && model.counts(1).funded == 1 && model.counts(2).rows == 1,
              QStringLiteral("each wallet has its own counts"));
        model.upsert(rowOf(1, 0, 400));
        check(model.total(wallet::Asset::BTC, 1) == chain::Amount::fromU64(400),
              QStringLiteral("a wallet's total follows a new balance"));
        model.removeWallet(1);
        check(model.size() == 1 && model.find(2, chain::ScriptType::P2WPKH, 0, 0, 0) == 0
                  && model.findByScripthash(QStringLiteral("2-0")) == 0 && model.findByScripthash(QStringLiteral("1-0")) < 0,
              QStringLiteral("removing a wallet keeps the other's row findable"));
        check(model.total(wallet::Asset::BTC) == chain::Amount::fromU64(500) && model.counts(1).rows == 0
                  && model.fundedCount() == 1,
              QStringLiteral("and leaves only its totals and counts"));
    } catch (const std::exception& e) {
        check(false, QStringLiteral("threw: ") + QString::fromUtf8(e.what()));
    }

    QFile::remove(wallet::WalletStore::filePath());
    return result;
}

} // namespace app
