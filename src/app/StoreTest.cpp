#include "app/StoreTest.h"

#include "app/WalletController.h"
#include "chain/Bip32.h"
#include "chain/Bip39.h"
#include "chain/Hash.h"
#include "wallet/BulkImport.h"
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

        // Lists: one wallet per line, whatever the form it comes in.
        const std::string list =
            std::string("# a comment, and a blank line after it\n\n")
            + "Store A: " + kSecondMnemonic + "\n"                    // 3: a name
            + kThirdMnemonic + "\n"                                   // 4: no name
            + "12. " + kThirdMnemonic + "\n"                          // 5: a numbered list
            + "\"Store B\",\"" + kSecondMnemonic + "\"\n"             // 6: CSV
            + "Store C\t" + kSecondMnemonic + "\n"                    // 7: a tab
            + "legal,winner,thank,year,wave,sausage,worth,useful,legal,winner,thank,yellow\n" // 8: commas between words
            + "Store D: legal winner thank year wave sausage worth useful legal winner thank\n" // 9: a word short
            + "Store E: abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon\n" // 10: checksum
            + "Store F: legal winner thank notaword wave sausage worth useful legal winner thank yellow\n" // 11: a typo
            + "Shop 1 Shop 2 Shop 3\n"                                // 12: no phrase at all
            + "Store G: " + kSecondMnemonic;                          // 13: no line break at the end
        const std::vector<wallet::SeedLine> lines = wallet::parseSeedLines(list);
        const auto lineAt = [&lines](int number) -> const wallet::SeedLine* {
            for (const wallet::SeedLine& line : lines) {
                if (line.number == number) return &line;
            }
            return nullptr;
        };
        const auto good = [&lineAt](int number, const QString& name) {
            const wallet::SeedLine* line = lineAt(number);
            return line && line->problem.isEmpty() && line->name == name && line->words.size() == 12;
        };
        const auto bad = [&lineAt](int number, const QString& says) {
            const wallet::SeedLine* line = lineAt(number);
            return line && line->problem.contains(says) && line->words.empty();
        };
        check(lines.size() == 11, QStringLiteral("a list skips blank lines and comments"));
        check(good(3, QStringLiteral("Store A")), QStringLiteral("a line reads its name before a colon"));
        check(good(4, QString()), QStringLiteral("a name is optional"));
        check(good(5, QString()), QStringLiteral("a number in front of a line is not a name"));
        check(good(6, QStringLiteral("Store B")), QStringLiteral("a CSV line reads as name and words"));
        check(good(7, QStringLiteral("Store C")), QStringLiteral("a tab can part the name from the words"));
        check(good(8, QString()), QStringLiteral("commas between the words are not a name"));
        check(good(13, QStringLiteral("Store G")), QStringLiteral("the last line needs no line break"));
        check(bad(9, QStringLiteral("11")), QStringLiteral("a short phrase says how many words it has, and keeps none"));
        check(bad(10, QStringLiteral("checksum")), QStringLiteral("a wrong checksum is named, and keeps no words"));
        check(bad(11, QStringLiteral("word 4")), QStringLiteral("a word not in the list is named by position"));
        check(lineAt(11) && !lineAt(11)->problem.contains(QStringLiteral("notaword")),
              QStringLiteral("and never quoted"));
        check(bad(12, QStringLiteral("words")), QStringLiteral("a line without a phrase says so"));
        check(lineAt(3) && lineAt(3)->words.front() == "legal" && lineAt(3)->words.back() == "yellow",
              QStringLiteral("the words of a line are the phrase, in order"));

        const QList<wallet::KeyLine> keyLines = wallet::parseKeyLines(
            QStringLiteral("# comment\nPartner A: %1\n%2 %3\n3) Partner B, %1\nno key here\n")
                .arg(first.at(0).xpub, first.at(1).xpub, first.at(2).xpub));
        check(keyLines.size() == 4 && keyLines.at(0).name == QStringLiteral("Partner A") && keyLines.at(0).keys.size() == 1
                  && keyLines.at(0).number == 2,
              QStringLiteral("a line of keys reads its name and key"));
        check(keyLines.size() == 4 && keyLines.at(1).name.isEmpty() && keyLines.at(1).keys.size() == 2,
              QStringLiteral("several keys on a line are one wallet"));
        check(keyLines.size() == 4 && keyLines.at(2).name == QStringLiteral("Partner B"),
              QStringLiteral("a numbered line of keys keeps its name"));
        check(keyLines.size() == 4 && !keyLines.at(3).problem.isEmpty(), QStringLiteral("a line without a key says so"));

        const int nameLimit = wallet::WalletStore::kMaxLabelChars;
        check(wallet::looksLikeSeedWords(QString::fromLatin1(kSecondMnemonic).left(nameLimit), nameLimit),
              QStringLiteral("a phrase cut at the longest name is still seen as one"));
        check(!wallet::looksLikeSeedWords(QStringLiteral("Online shop main account family business")),
              QStringLiteral("a real name made of list words is not"));
        check(wallet::looksLikeSeedWords(QStringLiteral("1. abandon 2. abandon 3. abandon 4. abandon 5. abandon 6. abando"), nameLimit),
              QStringLiteral("a numbered phrase cut there is"));
        check(wallet::looksLikeSeedWords(QString::fromLatin1(kSecondMnemonic).replace(QLatin1Char(' '), QChar(0x3000))),
              QStringLiteral("and so is one with other kinds of space"));
        check(wallet::looksLikeSeedWords(QString::fromLatin1(kSecondMnemonic).replace(QLatin1Char(' '), QStringLiteral(", "))),
              QStringLiteral("or commas"));
        const QList<wallet::KeyLine> spaced = wallet::parseKeyLines(QChar(0x3000) + first.at(2).xpub + QChar(0x2003));
        check(spaced.size() == 1 && spaced.first().keys == QStringList{first.at(2).xpub},
              QStringLiteral("a key copied with other kinds of space around it is still that key"));
        const std::vector<wallet::SeedLine> accented = wallet::parseSeedLines(
            QStringLiteral("Magazin Iași: ").normalized(QString::NormalizationForm_KD).toStdString() + kSecondMnemonic);
        check(accented.size() == 1 && accented.front().problem.isEmpty()
                  && accented.front().name == QStringLiteral("Magazin Iași"),
              QStringLiteral("a name keeps its letters in their usual form"));

        // A line that can be read two ways is refused, not guessed at. Every phrase here
        // is valid as written; the words after the mark pass their own checksum or not.
        const auto oneLine = [](const QString& text) {
            std::vector<wallet::SeedLine> parsed = wallet::parseSeedLines(text.toStdString());
            wallet::SeedLine line;
            if (parsed.size() == 1) {
                line.number = parsed.front().number;
                line.name = parsed.front().name;
                line.words.swap(parsed.front().words);
                line.problem = parsed.front().problem;
            }
            return line;
        };
        const QString eleven = QStringLiteral("abandon ").repeated(11).trimmed();
        {
            const wallet::SeedLine both = oneLine(QStringLiteral("legal winner thank, %1 alpha").arg(eleven));
            check(both.problem.contains(QStringLiteral("two ways")) || both.problem.contains(QStringLiteral("read as one phrase")),
                  QStringLiteral("15 valid words whose last 12 are valid too can be read two ways: refused"));
            check(both.words.empty() && both.name.isEmpty(), QStringLiteral("and neither its words nor a name are kept"));
            const wallet::SeedLine stray = oneLine(QStringLiteral("legal winner thank, %1 adult").arg(eleven));
            check(stray.problem.isEmpty() && stray.words.size() == 15 && stray.name.isEmpty(),
                  QStringLiteral("a phrase with a stray comma and no other reading is that phrase"));
            const wallet::SeedLine thirteen = oneLine(QStringLiteral("zoo,%1,about").arg(QString(eleven).replace(QLatin1Char(' '), QLatin1Char(','))));
            check(thirteen.problem.contains(QStringLiteral("it has 13")),
                  QStringLiteral("13 words parted by commas are 13 words, not a name and a phrase"));
            const wallet::SeedLine colon = oneLine(QStringLiteral("Office: abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon achieve"));
            check(colon.problem.contains(QStringLiteral("after the name")) && colon.problem.contains(QStringLiteral("it has 11")),
                  QStringLiteral("a name and 11 words is not a phrase that starts with the name"));
            const wallet::SeedLine listWords = oneLine(QStringLiteral("Main office, north: %1 express").arg(eleven));
            check(!listWords.problem.isEmpty() && listWords.words.empty(),
                  QStringLiteral("a name of list words that makes a whole phrase is refused"));
            const wallet::SeedLine comma = oneLine(QStringLiteral("Main office 1, north: %1").arg(QString::fromLatin1(kSecondMnemonic)));
            check(comma.problem.isEmpty() && comma.name == QStringLiteral("Main office 1, north") && comma.words.size() == 12,
                  QStringLiteral("a name can hold a comma"));
            const wallet::SeedLine after = oneLine(QStringLiteral("%1, Shop 1").arg(QString::fromLatin1(kSecondMnemonic)));
            check(after.problem.isEmpty() && after.name == QStringLiteral("Shop 1") && after.words.size() == 12,
                  QStringLiteral("a name can follow the phrase"));
            const wallet::SeedLine shortList = oneLine(QStringLiteral("legal,winner,thank,year,wave,sausage,worth,useful,legal,winner,thank"));
            check(shortList.problem.contains(QStringLiteral("it has 11")), QStringLiteral("a word count is of the line as written"));
            const wallet::SeedLine named13 = oneLine(QStringLiteral("Shop 1: %1 legal").arg(QString::fromLatin1(kSecondMnemonic)));
            check(named13.problem.contains(QStringLiteral("it has 13")), QStringLiteral("and after a name, of what follows it"));
            const wallet::SeedLine tab = oneLine(QStringLiteral("12.\t%1").arg(QString::fromLatin1(kSecondMnemonic)));
            check(tab.problem.isEmpty() && tab.name.isEmpty(), QStringLiteral("a list number can end in a tab"));
            const wallet::SeedLine tabName = oneLine(QStringLiteral("1.\tShop A: %1").arg(QString::fromLatin1(kSecondMnemonic)));
            check(tabName.problem.isEmpty() && tabName.name == QStringLiteral("Shop A"), QStringLiteral("with a name after it"));
            const wallet::SeedLine quotes = oneLine(QStringLiteral("Magazin \"Central\": %1").arg(QString::fromLatin1(kSecondMnemonic)));
            check(quotes.name == QStringLiteral("Magazin \"Central\""), QStringLiteral("quotes inside a name stay"));
            const wallet::SeedLine csvQuotes = oneLine(QStringLiteral("\"Magazin \"\"Central\"\", north\",%1").arg(QString::fromLatin1(kSecondMnemonic)));
            check(csvQuotes.name == QStringLiteral("Magazin \"Central\", north"), QStringLiteral("and CSV quotes read as CSV"));
            const wallet::SeedLine symbol = oneLine(QStringLiteral("Shop №1: %1").arg(QString::fromLatin1(kSecondMnemonic)));
            check(symbol.name == QStringLiteral("Shop №1"), QStringLiteral("a name stays as typed"));
            QString wide = QString::fromLatin1(kSecondMnemonic);
            wide.replace(QStringLiteral("legal"), QStringLiteral("ｌｅｇａｌ"));
            const wallet::SeedLine fullWidth = oneLine(wide);
            check(fullWidth.problem.isEmpty() && fullWidth.words.size() == 12 && fullWidth.words.front() == "legal",
                  QStringLiteral("full-width letters read as BIP39 reads them"));
            const wallet::SeedLine commented = oneLine(QStringLiteral("#1 Shop A: %1").arg(QString::fromLatin1(kSecondMnemonic)));
            check(commented.problem.contains(QStringLiteral("starts with #")) && commented.words.empty() && commented.number == 1,
                  QStringLiteral("a phrase behind a # is not left out unseen"));
            check(wallet::parseSeedLines("# Public key list, main office, north side").empty(),
                  QStringLiteral("a comment is skipped"));
            const wallet::SeedLine privateName = oneLine(QStringLiteral("xprv9s21ZrQH143K3QTDL4LXw2F7HEK3wJUD2nW2nRk4stbPy6cq3jPPqjiChkVvvNKmPGJxWUtg6LnF5kejMRNNU3TGtRBeJgk33yuGBxrMPHi: %1")
                                                             .arg(QString::fromLatin1(kSecondMnemonic)));
            check(privateName.problem.contains(QStringLiteral("private key")) && privateName.name.isEmpty()
                      && privateName.words.empty(),
                  QStringLiteral("a private key is no name"));
        }

        // What must never become a name.
        check(wallet::looksLikePrivateKey(QStringLiteral("KwDiBf89QgGbjEhKnhXJuH7LrciVrZi3qYjgd9M7rFU73sVHnoWn")),
              QStringLiteral("a WIF key reads as a private key"));
        check(wallet::looksLikePrivateKey(QStringLiteral("Shop 0x") + QStringLiteral("ab").repeated(32)),
              QStringLiteral("so does a raw one in hex"));
        check(wallet::looksLikePrivateKey(QStringLiteral("zprvAdG4iTXWBoARxkkzNpNh8r6Qag3irQB8PzEMkAFeTRXxHpbF9z4QgEvBRmfvqWvGp42t42nvgGpNgYSJA9iefm1yYNZKEm7z6qUWCroSQnE")),
              QStringLiteral("and a zprv"));
        check(!wallet::looksLikePrivateKey(QStringLiteral("Shop 1 bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu")),
              QStringLiteral("an address does not"));
        check(!wallet::looksLikePrivateKey(first.at(0).xpub), QStringLiteral("nor a public key"));
        QString hidden = QString::fromLatin1(kSecondMnemonic);
        hidden.replace(QLatin1Char(' '), QString(QChar(0x200B)) + QLatin1Char(' '));
        check(wallet::looksLikeSeedWords(hidden), QStringLiteral("a phrase with invisible characters reads as one"));
        QString numberedWords;
        const QStringList plainWords = QString::fromLatin1(kSecondMnemonic).split(QLatin1Char(' '));
        for (qsizetype i = 0; i < plainWords.size(); ++i)
            numberedWords += QStringLiteral("%1.%2 ").arg(i + 1).arg(plainWords.at(i));
        check(wallet::looksLikeSeedWords(numberedWords), QStringLiteral("and so does one with a number on each word"));
        check(wallet::looksLikeSeedWords(QString::fromLatin1(kSecondMnemonic).replace(QLatin1Char(' '), QLatin1Char('-'))),
              QStringLiteral("or joined by dashes"));
        check(!wallet::keyListHoldsSecret(QStringLiteral("Online shop main account family: %1").arg(first.at(0).xpub)),
              QStringLiteral("a name of list words beside a key is no secret"));
        check(!wallet::keyListHoldsSecret(QStringLiteral("# Public key list for the main office, one key per line, shop accounts\n%1").arg(first.at(0).xpub)),
              QStringLiteral("nor is a long comment"));
        check(wallet::keyListHoldsSecret(QStringLiteral("%1\n%2").arg(first.at(0).xpub, QString::fromLatin1(kSecondMnemonic))),
              QStringLiteral("a phrase on one line of a key list is"));
        check(wallet::keyListHoldsSecret(QStringLiteral("xprv9s21ZrQH143K3QTDL4LXw2F7HEK3wJUD2nW2nRk4stbPy6cq3jPPqjiChkVvvNKmPGJxWUtg6LnF5kejMRNNU3TGtRBeJgk33yuGBxrMPHi %1").arg(first.at(0).xpub)),
              QStringLiteral("and a private key beside a public one"));

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

            // Ethereum's `used` flag outlives what set it, and a fresh scan starts without it.
            const auto ethRow = [own](quint32 index, bool used, quint64 nonce) {
                wallet::AddressRow row = rowOf(own, index, 0);
                row.type = chain::ScriptType::EthAccount;
                row.used = used;
                row.nonce = nonce;
                return row;
            };
            controller.model()->upsert(ethRow(50, true, 0));
            controller.model()->upsert(ethRow(120, true, 1));
            settings = controller.store().settings;
            check(controller.gapImpact(settings).used == 0,
                  QStringLiteral("with no gap falling the rows stay, so nothing is left out"));
            settings.btcChangeGap -= 10;
            check(controller.gapImpact(settings).used == 1,
                  QStringLiteral("any gap falling clears them, and an Ethereum address only marked used keeps nothing in reach"));
            controller.model()->clear();

            // Remove all takes the files kept aside too.
            done(controller.removeAllWallets(&error), QStringLiteral("every wallet can be removed"));
            check(!QFile::exists(filePath) && asideFiles().isEmpty(),
                  QStringLiteral("which leaves neither wallet.dat nor the file kept aside"));
        }
        {
            // A batch: one save, the good ones in, the others say why.
            WalletController controller;
            wallet::WalletRequest named;
            named.accounts = first;
            named.label = QStringLiteral("Batch A");
            wallet::WalletRequest unnamed;
            unnamed.accounts = second;
            wallet::WalletRequest again;
            again.accounts = first;
            again.label = QStringLiteral("Batch A again");
            QList<wallet::WalletRequest> batch{named, unnamed, again};
            check(controller.addWallets(&batch, &error) == 2 && error.isEmpty(), QStringLiteral("a batch adds what it can"));
            check(batch.at(0).refused.isEmpty() && batch.at(1).refused.isEmpty()
                      && batch.at(2).refused.contains(QStringLiteral("Batch A")),
                  QStringLiteral("and the same seed twice in it is refused, naming the first"));
            check(controller.store().wallets.size() == 2 && controller.store().wallets.at(1).label.startsWith(QStringLiteral("Wallet ")),
                  QStringLiteral("a wallet without a name gets a number"));
            wallet::WalletStore saved;
            check(saved.load(&error) && saved.wallets.size() == 2, QStringLiteral("all of it is saved"));
            QList<wallet::WalletRequest> twice{named};
            check(controller.addWallets(&twice, &error) == 0 && error.isEmpty() && !twice.at(0).refused.isEmpty(),
                  QStringLiteral("a batch of what is here already adds nothing, and says why per line"));
            done(controller.removeAllWallets(&error), QStringLiteral("the batch can be removed"));

            // One seed, 12,000 used addresses in a row: no hole for a smaller gap to
            // leave out, but the row limit falls with the gaps (15,000 at 200/50, 10,000 at 100/50).
            done(controller.addKeysWallet(first, QStringLiteral("Long run"), &error), QStringLiteral("a long run's wallet"));
            const quint32 runId = controller.store().wallets.first().id;
            for (quint32 index = 0; index < 12000; ++index) {
                wallet::AddressRow row = rowOf(runId, index, index == 11999 ? 700 : 0);
                row.used = true;
                controller.model()->upsert(row);
            }
            wallet::ScanSettings gapped = controller.store().settings;
            check(gapped.btcReceiveGap == 200 && gapped.btcChangeGap == 50 && controller.gapImpact(gapped).used == 0,
                  QStringLiteral("the same gaps leave nothing out"));
            gapped.btcReceiveGap = 100;
            const WalletController::GapImpact run = controller.gapImpact(gapped);
            check(run.atMost && run.used == 2450 && run.funded == 1,
                  QStringLiteral("smaller ones say how many the row limit can cut"));
            controller.model()->clear();
            done(controller.removeAllWallets(&error), QStringLiteral("and it can be removed"));

            // Numbers go to what really goes in, and past every name in use.
            const auto numberedRequest = [](const QList<wallet::AccountKey>& accounts, const QString& base) {
                wallet::WalletRequest request;
                request.accounts = accounts;
                request.label = base;
                request.numbered = true;
                return request;
            };
            wallet::WalletRequest explicitName;
            explicitName.accounts = accountsOf(kFirstMnemonic);
            explicitName.label = QStringLiteral("Shop 2");
            QList<wallet::WalletRequest> names{numberedRequest(accountsOf(kSecondMnemonic), QStringLiteral("Shop")), explicitName,
                                               numberedRequest(accountsOf(kFirstMnemonic), QStringLiteral("Shop")),
                                               numberedRequest(accountsOf(kThirdMnemonic), QStringLiteral("Shop"))};
            check(controller.addWallets(&names, &error) == 3, QStringLiteral("a list with a seed twice adds the others"));
            const auto labelOf = [&controller](qsizetype i) { return controller.store().wallets.value(i).label; };
            check(labelOf(0) == QStringLiteral("Shop 1") && labelOf(1) == QStringLiteral("Shop 2") && labelOf(2) == QStringLiteral("Shop 3"),
                  QStringLiteral("numbers skip a name the list asks for, and go only to what goes in"));
            QList<wallet::WalletRequest> sameName(1);
            sameName.first().accounts = accountsOf("ozone drill grab fiber curtain grace pudding thank cruise elder eight picnic");
            sameName.first().label = QStringLiteral("shop 1");
            check(controller.addWallets(&sameName, &error) == 0 && sameName.first().refused.contains(QStringLiteral("already named")),
                  QStringLiteral("a name in use, in any case, is refused"));
            QList<wallet::WalletRequest> longBase{numberedRequest(sameName.first().accounts, QStringLiteral("Z").repeated(64))};
            check(controller.addWallets(&longBase, &error) == 1 && labelOf(3).size() == wallet::WalletStore::kMaxLabelChars
                      && labelOf(3).endsWith(QStringLiteral(" 1")),
                  QStringLiteral("a number always fits a name's length"));
            QList<wallet::WalletRequest> marker{numberedRequest(accountsOf("scheme spot photo card baby mountain device kick cradle pact join borrow"),
                                                                QStringLiteral("Sale %1"))};
            check(controller.addWallets(&marker, &error) == 1 && labelOf(4) == QStringLiteral("Sale %1 1"),
                  QStringLiteral("what a name holds is only text"));
            QList<wallet::WalletRequest> secretName(1);
            secretName.first().accounts = accountsOf("cat swing flag economy stadium alone churn speed unique patch report train");
            secretName.first().label = QString::fromLatin1(kSecondMnemonic);
            check(controller.addWallets(&secretName, &error) == 0 && secretName.first().refused.contains(QStringLiteral("seed words"))
                      && !secretName.first().refused.contains(QStringLiteral("legal")),
                  QStringLiteral("seed words never become a name, whatever the screen let through"));
            const quint32 renamedId = controller.store().wallets.first().id;
            check(!controller.renameWallet(renamedId, QString::fromLatin1(kSecondMnemonic), &error)
                      && labelOf(0) == QStringLiteral("Shop 1"),
                  QStringLiteral("nor by renaming"));
            check(!controller.renameWallet(renamedId, QStringLiteral("SHOP 2"), &error) && error.contains(QStringLiteral("already named")),
                  QStringLiteral("and a rename cannot take a name in use"));
            done(controller.renameWallet(renamedId, QStringLiteral("shop 1"), &error), QStringLiteral("though it can change its own"));
            done(controller.removeAllWallets(&error), QStringLiteral("and they can be removed"));

            // Raised gaps can go past the limit too, with nothing used to lose yet.
            QList<wallet::WalletRequest> twoSeeds{numberedRequest(accountsOf(kFirstMnemonic), QStringLiteral("Big")),
                                                  numberedRequest(accountsOf(kSecondMnemonic), QStringLiteral("Big"))};
            check(controller.addWallets(&twoSeeds, &error) == 2, QStringLiteral("two seeds"));
            wallet::ScanSettings raised = controller.store().settings;
            raised.btcReceiveGap = 5000;
            raised.btcChangeGap = 5000;
            const WalletController::GapImpact raisedImpact = controller.gapImpact(raised);
            check(raisedImpact.overLimit == 20000 && raisedImpact.used == 0,
                  QStringLiteral("raised to 5,000 they want 20,000 addresses past what one connection follows"));
            done(controller.removeAllWallets(&error), QStringLiteral("and they can be removed"));

            // The same text can be two sets of addresses, in one wallet or two.
            QString why;
            QList<wallet::AccountKey> twoKinds;
            check(wallet::WalletStore::accountsFromPublicKeys({first.at(2).xpub}, {first.at(2).xpub}, chain::ScriptType::P2PKH,
                                                              &twoKinds, &why)
                      && twoKinds.size() == 2,
                  QStringLiteral("one xpub reads as a legacy and an Ethereum account"));
            done(controller.addKeysWallet(twoKinds, QStringLiteral("Two kinds"), &error),
                 QStringLiteral("and such a wallet saves"));
            done(controller.removeAllWallets(&error), QStringLiteral("and it can be removed"));
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
