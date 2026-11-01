#include "app/StoreTest.h"

#include "app/WalletController.h"
#include "chain/Bip32.h"
#include "chain/Bip39.h"
#include "chain/Hash.h"
#include "wallet/Types.h"
#include "wallet/WalletModel.h"
#include "wallet/WalletStore.h"

#include <QCryptographicHash>
#include <QDir>
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
const char* kThirdMnemonic = "letter advice cage absurd amount doctor acoustic avoid letter advice cage above";

QList<wallet::AccountKey> accountsOf(const char* mnemonic)
{
    return wallet::WalletStore::deriveAccounts(chain::bip39::split(mnemonic), {});
}

// The text of the first `count` keys.
QStringList accountsText(const QList<wallet::AccountKey>& accounts, qsizetype count)
{
    QStringList out;
    for (qsizetype i = 0; i < count; ++i)
        out << accounts.at(i).xpub;
    return out;
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
    // For a call that reports through `error`: read here, once the call has run.
    QString error;
    const auto done = [&check, &error](bool ok, const QString& what) {
        check(ok, ok ? what : what + QStringLiteral(": ") + error);
    };

    try {
        const QList<wallet::AccountKey> first = accountsOf(kFirstMnemonic);
        const QList<wallet::AccountKey> second = accountsOf(kSecondMnemonic);
        check(first.size() == 4 && second.size() == 4, QStringLiteral("a seed derives 4 account keys"));

        // A file from before many wallets: one wallet, its name and settings kept.
        done(wallet::WalletStore::writeRawForTest(legacyBody(first), &error),
             QStringLiteral("a first-format wallet.dat was written"));
        wallet::WalletStore store;
        done(store.load(&error), QStringLiteral("a first-format wallet.dat loads"));
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
        done(store.save(&error), QStringLiteral("two wallets save"));
        wallet::WalletStore reread;
        done(reread.load(&error), QStringLiteral("two wallets load"));
        check(reread.wallets.size() == 2 && reread.find(secondId)
                  && reread.find(secondId)->label == QStringLiteral("Warehouse"),
              QStringLiteral("both come back, by id and name"));
        check(reread.allAccounts().size() == 8, QStringLiteral("with all 8 keys"));
        check(reread.find(secondId) && reread.find(secondId)->accounts.first().wallet == secondId,
              QStringLiteral("keys read back name their wallet"));
        {
            QFile raw(wallet::WalletStore::filePath()); // closed before the next save: Windows refuses to replace an open file
            const QByteArray bytes = raw.open(QIODevice::ReadOnly) ? raw.readAll() : QByteArray();
            check(!bytes.isEmpty() && !bytes.contains("zpub") && !bytes.contains("Warehouse"),
                  QStringLiteral("the file stays encrypted"));
        }

        // An id is not reused, not even after its wallet went.
        reread.wallets.removeLast();
        done(reread.save(&error), QStringLiteral("one wallet removed saves"));
        wallet::WalletStore afterRemove;
        done(afterRemove.load(&error), QStringLiteral("one wallet removed loads"));
        check(afterRemove.wallets.size() == 1 && afterRemove.find(firstId) && !afterRemove.find(secondId),
              QStringLiteral("only that wallet is gone"));
        check(afterRemove.takeWalletId() > secondId, QStringLiteral("the removed wallet's id is not handed out again"));

        // One set of addresses has one owner, also in the file.
        wallet::WalletStore repeated;
        repeated.wallets.append(entryOf(repeated.takeWalletId(), QStringLiteral("A"), first));
        repeated.wallets.append(entryOf(repeated.takeWalletId(), QStringLiteral("B"), first));
        check(!repeated.save(&error) && error.contains(QStringLiteral("two wallets")),
              QStringLiteral("the same key in two wallets is refused"));

        // Public keys.
        const auto keysOf = [&error](const QStringList& bitcoin, const QStringList& ethereum,
                                     chain::ScriptType xpubType, QList<wallet::AccountKey>* out) {
            return wallet::WalletStore::accountsFromPublicKeys(bitcoin, ethereum, xpubType, out, &error);
        };
        const chain::ScriptType legacy = chain::ScriptType::P2PKH;
        QList<wallet::AccountKey> keys;
        check(keysOf({first.at(0).xpub, first.at(1).xpub}, {}, legacy, &keys) && keys.size() == 2
                  && keys.at(0).account == 0 && keys.at(0).type == chain::ScriptType::P2WPKH
                  && keys.at(1).type == chain::ScriptType::P2SH_P2WPKH,
              QStringLiteral("zpub and ypub are read as native and nested SegWit"));
        check(keysOf({first.at(2).xpub}, {}, legacy, &keys) && keys.size() == 1
                  && keys.first().type == chain::ScriptType::P2PKH,
              QStringLiteral("an xpub is read as the Bitcoin type chosen"));
        check(keysOf({}, {first.at(3).xpub}, legacy, &keys) && keys.size() == 1
                  && keys.first().type == chain::ScriptType::EthAccount,
              QStringLiteral("an Ethereum key is an Ethereum account"));
        check(!keysOf({}, {first.at(0).xpub}, legacy, &keys) && error.contains(QStringLiteral("Ethereum key 1")),
              QStringLiteral("a zpub is refused as an Ethereum key"));
        check(keysOf(accountsText(first, 3), {first.at(3).xpub}, legacy, &keys) && keys.size() == 4,
              QStringLiteral("all four keys the Public keys dialog shows go in as one wallet"));

        // A SegWit account written with the xpub prefix: the same node, kept as its
        // type is stored, so it shows the same addresses as the zpub of that account.
        const chain::HdPubKey nativeNode = first.at(0).node();
        const QString asXpub = QString::fromStdString(nativeNode.serialize(chain::xpubVersion::kXpub));
        check(keysOf({asXpub}, {}, chain::ScriptType::P2WPKH, &keys) && keys.size() == 1
                  && keys.first().type == chain::ScriptType::P2WPKH && keys.first().xpub == first.at(0).xpub,
              QStringLiteral("an xpub chosen as native SegWit becomes that account's zpub"));
        check(wallet::WalletStore::addressSetId(keys.first()) == wallet::WalletStore::addressSetId(first.at(0)),
              QStringLiteral("and the two count as the same addresses"));
        check(keysOf({asXpub}, {}, legacy, &keys) && keys.size() == 1
                  && wallet::WalletStore::addressSetId(keys.first()) != wallet::WalletStore::addressSetId(first.at(0)),
              QStringLiteral("the same node read as legacy is another set of addresses"));

        // The same node under other metadata: other text, the same addresses.
        const chain::HdPubKey parsed = first.at(0).node();
        const chain::HdPubKey relabelled = chain::HdPubKey::fromParts(parsed.publicKey(), parsed.chainCode(), parsed.depth(),
                                                                      0, parsed.childNumber());
        const QString otherText = QString::fromStdString(relabelled.serialize(chain::xpubVersion::kZpub));
        check(otherText != first.at(0).xpub, QStringLiteral("a key with another parent fingerprint reads differently"));
        check(keysOf({otherText}, {}, legacy, &keys) && keys.size() == 1
                  && wallet::WalletStore::addressSetId(keys.first()) == wallet::WalletStore::addressSetId(first.at(0)),
              QStringLiteral("yet counts as the same addresses"));
        check(!keysOf({first.at(0).xpub, otherText}, {}, legacy, &keys) && error.contains(QStringLiteral("repeats")),
              QStringLiteral("so the two are refused together"));

        check(!keysOf({QStringLiteral("not a key")}, {}, legacy, &keys) && error.contains(QStringLiteral("Bitcoin key 1"))
                  && !error.contains(QStringLiteral("not a key")),
              QStringLiteral("a bad key is named by its number, never quoted"));
        const chain::HdKey master = chain::HdKey::fromSeed(chain::Bytes(32, 7));
        const QString masterKey = QString::fromStdString(master.neuter().serialize(chain::xpubVersion::kXpub));
        check(!keysOf({masterKey}, {}, legacy, &keys), QStringLiteral("a key from another depth is refused"));
        check(!keysOf({first.at(0).xpub, first.at(0).xpub}, {}, legacy, &keys),
              QStringLiteral("the same account twice is refused"));

        // What save() reports is what it wrote.
        QByteArray digest;
        done(afterRemove.save(&error, &digest), QStringLiteral("a save reports a digest"));
        QFile written(wallet::WalletStore::filePath());
        const QByteArray writtenBytes = written.open(QIODevice::ReadOnly) ? written.readAll() : QByteArray();
        written.close();
        check(!digest.isEmpty() && digest == QCryptographicHash::hash(writtenBytes, QCryptographicHash::Sha256),
              QStringLiteral("and it is the digest of the file"));
        QFile::remove(wallet::WalletStore::filePath());

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

        // The controller: what it does to a wallet.dat it did not write. Nothing
        // here reaches the network: no event loop runs while it is alive.
        const QList<wallet::AccountKey> third = accountsOf(kThirdMnemonic);
        const QString filePath = wallet::WalletStore::filePath();
        const auto asideFiles = [] {
            return QDir(wallet::WalletStore::dataDir())
                .entryList({QStringLiteral("wallet.dat.unread-*")}, QDir::Files);
        };
        const auto zpubOf = [](const QList<wallet::AccountKey>& accounts) {
            return QList<wallet::AccountKey>{accounts.first()};
        };
        {
            // A readable file that appears after the start is not moved away.
            WalletController controller;
            check(!controller.loadExisting(&error) && error.isEmpty(), QStringLiteral("no file: no wallet, no error"));
            wallet::WalletStore other;
            other.wallets.append(entryOf(other.takeWalletId(), QStringLiteral("Restored"), first));
            done(other.save(&error), QStringLiteral("a file appears after the start"));
            check(!controller.addKeysWallet(zpubOf(third), QStringLiteral("New"), &error)
                      && error.contains(QStringLiteral("restart")),
                  QStringLiteral("adding says to restart instead of moving it"));
            check(asideFiles().isEmpty(), QStringLiteral("nothing was moved"));
            wallet::WalletStore reread2;
            check(reread2.load(&error) && reread2.wallets.size() == 1
                      && reread2.wallets.first().label == QStringLiteral("Restored"),
                  QStringLiteral("and the file still holds its wallet"));
        }
        QFile::remove(filePath);
        {
            // A file that cannot be read is kept, and the user is told where.
            QFile damaged(filePath);
            done(damaged.open(QIODevice::WriteOnly) && damaged.write("not a wallet file") > 0,
                 QStringLiteral("a damaged wallet.dat is in place"));
            damaged.close();
            WalletController controller;
            QStringList notices;
            QObject::connect(&controller, &WalletController::errorOccurred, &controller,
                             [&notices](wallet::Problem kind, const QString& message, const QString&) {
                                 if (kind == wallet::Problem::FileSetAside)
                                     notices << message;
                             });
            check(!controller.loadExisting(&error) && !error.isEmpty(), QStringLiteral("it does not load, and says so"));
            done(controller.addKeysWallet(zpubOf(third), QStringLiteral("New"), &error),
                 QStringLiteral("a wallet can still be added"));
            const QStringList aside = asideFiles();
            check(aside.size() == 1, QStringLiteral("the damaged file is kept aside"));
            QFile kept(QDir(wallet::WalletStore::dataDir()).filePath(aside.value(0)));
            const QByteArray keptBytes = kept.open(QIODevice::ReadOnly) ? kept.readAll() : QByteArray();
            kept.close();
            check(keptBytes == "not a wallet file", QStringLiteral("unchanged"));
            check(notices.size() == 1 && !aside.isEmpty() && notices.first().contains(aside.first()),
                  QStringLiteral("the user is told its new name"));
            wallet::WalletStore reread3;
            check(reread3.load(&error) && reread3.wallets.size() == 1, QStringLiteral("the new file holds the new wallet"));

            // Other text, same addresses: refused as well.
            const chain::HdPubKey node = third.first().node();
            wallet::AccountKey relabelled = third.first();
            relabelled.xpub = QString::fromStdString(
                chain::HdPubKey::fromParts(node.publicKey(), node.chainCode(), node.depth(), 0, node.childNumber())
                    .serialize(chain::xpubVersion::kZpub));
            check(relabelled.xpub != third.first().xpub
                      && !controller.addKeysWallet({relabelled}, QStringLiteral("Again"), &error)
                      && error.contains(QStringLiteral("already has these keys")),
                  QStringLiteral("the same addresses under other key text are refused"));

            // Smaller gaps: what they would leave out.
            wallet::ScanSettings settings = controller.store().settings;
            const quint32 own = controller.store().wallets.first().id;
            wallet::AddressRow lowRow = rowOf(own, 0, 0);
            lowRow.used = true;
            controller.model()->upsert(lowRow);
            controller.model()->upsert(rowOf(own, 150, 700));
            settings.btcReceiveGap = 200;
            check(controller.gapImpact(settings).used == 0, QStringLiteral("a gap of 200 reaches an address at index 150"));
            settings.btcReceiveGap = 100;
            const WalletController::GapImpact impact = controller.gapImpact(settings);
            check(impact.used == 1 && impact.funded == 1,
                  QStringLiteral("a gap of 100 leaves it out, with its balance"));
            controller.model()->clear();

            // Remove all takes the files kept aside too.
            done(controller.removeAllWallets(&error), QStringLiteral("every wallet can be removed"));
            check(!QFile::exists(filePath) && asideFiles().isEmpty(),
                  QStringLiteral("which leaves neither wallet.dat nor the file kept aside"));
        }
        {
            // A file changed outside this window is not this window's to delete.
            WalletController controller;
            done(controller.addKeysWallet(zpubOf(third), QStringLiteral("One"), &error),
                 QStringLiteral("a wallet is added"));
            wallet::WalletStore other;
            other.wallets.append(entryOf(other.takeWalletId(), QStringLiteral("Another window's"), first));
            done(other.save(&error), QStringLiteral("another window writes the file"));
            check(!controller.removeAllWallets(&error) && error.contains(QStringLiteral("changed outside")),
                  QStringLiteral("remove all is refused"));
            check(QFile::exists(filePath), QStringLiteral("and the file stays"));
        }
    } catch (const std::exception& e) {
        check(false, QStringLiteral("threw: ") + QString::fromUtf8(e.what()));
    }

    QFile::remove(wallet::WalletStore::filePath());
    return result;
}

} // namespace app
