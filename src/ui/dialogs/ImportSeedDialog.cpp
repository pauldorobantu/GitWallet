#include "ui/dialogs/ImportSeedDialog.h"

#include "app/WalletController.h"
#include "chain/Address.h"
#include "chain/Bip39.h"
#include "chain/Hash.h"
#include "ui/DialogChrome.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/XpubField.h"
#include "ui/design/Style.h"
#include "wallet/BtcScanner.h"
#include "wallet/WalletStore.h"

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTextDocument>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace app::ui {

namespace {

// The pages of the dialog, in the order of its "Add from" list.
enum Source { OneSeed, ManySeeds, OneKeys, ManyKeys };

// A list file is text someone keeps: far above any list, far below what could stall
// the window.
constexpr qint64 kMaxListFileBytes = 4 << 20;
// Problems the status line spells out; the rest are counted.
constexpr qsizetype kShownProblems = 4;

// Neither field may teach an input method or a touch keyboard what was typed.
constexpr Qt::InputMethodHints kSecretInputHints =
    Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase;

// The secret text one check or import turns into std::string: the phrase as
// typed, its words and the passphrase. The destructor scrubs them, so every
// way out of the caller (an early return, an exception from chain::) leaves
// no copy behind.
struct SeedCopy
{
    std::string text;
    std::vector<std::string> words;
    std::string passphrase;

    SeedCopy() = default;
    SeedCopy(const SeedCopy&) = delete;
    SeedCopy& operator=(const SeedCopy&) = delete;
    ~SeedCopy() { wipe(); }

    void wipe()
    {
        chain::secureClear(text);
        for (std::string& word : words)
            chain::secureClear(word);
        words.clear();
        chain::secureClear(passphrase);
    }
};

// `text` as UTF-8 into `out`. The QString and the intermediate QByteArray are
// zeroed before they are released; fill() is compiled into QtCore, so the
// optimiser cannot drop the writes as dead stores.
void takeUtf8(QString& text, std::string& out)
{
    QByteArray bytes = text.toUtf8();
    out.assign(bytes.constData(), static_cast<size_t>(bytes.size()));
    bytes.fill('\0');
    text.fill(QChar(u'\0'));
    text.clear();
}

// The phrase as typed, in the form chain::bip39::split() expects: it breaks
// words on ASCII whitespace and lowercases ASCII only. NFKD, the form BIP39
// defines the mnemonic in, turns full-width letters and the compatibility
// spaces (thin, em, narrow no-break, ideographic) into ASCII; any other
// Unicode space becomes an ASCII one. Invisible format characters (a BOM, a
// zero-width space, a soft hyphen, a direction mark) are dropped instead of
// spaced: they never show as a gap, so a word on screen stays one word. The
// live check and the import both read the phrase through here, so the words
// that passed the check are the words imported. `lines`: a list keeps its line
// breaks and tabs, which wallet::parseSeedLines() reads.
void takePhrase(const QPlainTextEdit* edit, std::string& out, bool lines = false)
{
    QString typed = edit->toPlainText();
    QString nfkd = typed.normalized(QString::NormalizationForm_KD);
    QString phrase;
    phrase.reserve(nfkd.size()); // never outgrown, so never copied on the way
    for (const QChar c : std::as_const(nfkd)) {
        if (c.category() == QChar::Other_Format)
            continue;
        const bool keep = lines && (c == QChar(u'\n') || c == QChar(u'\t'));
        phrase.append(c.isSpace() && !keep ? QChar(u' ') : c);
    }
    // normalized() hands a phrase that needs no change (any ASCII one) back
    // in the very buffer `typed` holds; filling both zeroes it whichever of
    // the two owns it by then.
    typed.fill(QChar(u'\0'));
    nfkd.fill(QChar(u'\0'));
    takeUtf8(phrase, out);
}

// The BIP39 English list, sorted, for the word-by-word checks while typing:
// chain::bip39 keeps its own copy private and validates whole phrases only.
// The list is public data, so a second copy holds nothing secret.
const std::vector<std::string>& bip39Words()
{
    static const std::vector<std::string> words = [] {
        std::vector<std::string> list;
        list.reserve(2048);
        QFile file(QStringLiteral(":/bip39/english.txt"));
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!file.atEnd()) {
                const QByteArray line = file.readLine().trimmed();
                if (!line.isEmpty())
                    list.push_back(line.toStdString());
            }
        }
        std::sort(list.begin(), list.end());
        return list;
    }();
    return words;
}

bool isBip39Word(const std::string& word)
{
    const std::vector<std::string>& list = bip39Words();
    return std::binary_search(list.begin(), list.end(), word);
}

// True if a list word starts with `prefix`, i.e. a word still being typed
// can become valid.
bool isBip39Prefix(const std::string& prefix)
{
    const std::vector<std::string>& list = bip39Words();
    const auto it = std::lower_bound(list.begin(), list.end(), prefix);
    return it != list.end() && it->starts_with(prefix);
}

bool isValidWordCount(size_t count)
{
    return count == 12 || count == 15 || count == 18 || count == 21 || count == 24;
}

// Replaces the text with as many spaces, then empties the field, with its
// signals blocked so no listener (the live check included) sees either step.
// Best effort only: Qt frees its own internal buffers without scrubbing them.
void wipeLineEdit(QLineEdit* edit)
{
    if (!edit)
        return;
    const QSignalBlocker blocker(edit);
    edit->setText(QString(edit->text().size(), QLatin1Char(' ')));
    edit->clear();
}

void wipeTextEdit(QPlainTextEdit* edit)
{
    if (!edit)
        return;
    const QSignalBlocker blocker(edit);
    // characterCount() includes the closing paragraph separator; it gives the
    // length without copying the text out once more.
    const int length = qMax(0, edit->document()->characterCount() - 1);
    edit->setPlainText(QString(length, QLatin1Char(' ')));
    edit->clear();
}

// The address a key starts with, for the user to compare with the other wallet.
// Throws what the derivation throws.
QString firstAddress(const wallet::AccountKey& key)
{
    const chain::HdPubKey chainNode = key.node().deriveChild(0);
    return QString::fromStdString(chain::addressFromChainNode(chainNode, key.type, key.account, 0, 0).address);
}

// The status line of a list: how many lines can go in, and what is wrong with the rest.
QString listSummary(int ready, const QString& oneReady, const QString& manyReady, const QStringList& problems)
{
    QStringList lines;
    if (ready > 0)
        lines << (ready == 1 ? oneReady : manyReady.arg(ready));
    for (qsizetype i = 0; i < problems.size() && i < kShownProblems; ++i)
        lines << problems.at(i);
    if (problems.size() > kShownProblems)
        lines << trText(QStringLiteral("and %1 more lines to fix")).arg(problems.size() - kShownProblems);
    return lines.join(QLatin1Char('\n'));
}

// A list's accounts: an xpub is read as `xpubType`, which may be an Ethereum account.
bool accountsOfLine(const wallet::KeyLine& line, chain::ScriptType xpubType, QList<wallet::AccountKey>* out,
                    QString* why)
{
    const bool xpubIsEthereum = xpubType == chain::ScriptType::EthAccount;
    QStringList bitcoin;
    QStringList ethereum;
    for (const QString& key : line.keys) {
        if (xpubIsEthereum && key.startsWith(QLatin1String("xpub")))
            ethereum << key;
        else
            bitcoin << key;
    }
    return wallet::WalletStore::accountsFromPublicKeys(bitcoin, ethereum,
                                                       xpubIsEthereum ? chain::ScriptType::P2PKH : xpubType, out, why);
}

} // namespace

ImportSeedDialog::ImportSeedDialog(WalletController* controller, QWidget* parent, bool many)
    : QDialog(parent)
    , controller_(controller)
{
    setWindowTitle(QStringLiteral("Add wallet"));
    // The themed title bar and frame go on before anything is laid out or shown.
    applyDialogChrome(this);

    auto* root = new QVBoxLayout(this);
    page::applyDialogLayout(root);

    // The name it will go by, and what it is made from.
    auto* identity = new QHBoxLayout;
    identity->setSpacing(tokens::layout::kFormHSpacing);
    auto* nameBlock = new QVBoxLayout;
    nameBlock->setSpacing(tokens::layout::kFormVSpacing);
    nameLabel_ = new QLabel(QStringLiteral("Wallet name"), this);
    nameLabel_->setObjectName(QStringLiteral("formSectionLabel"));
    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("walletName"));
    name_->setMaxLength(wallet::WalletStore::kMaxLabelChars);
    name_->setPlaceholderText(QStringLiteral("For example: Online shop"));
    nameLabel_->setBuddy(name_);
    nameBlock->addWidget(nameLabel_);
    nameBlock->addWidget(name_);
    identity->addLayout(nameBlock, 1);
    auto* sourceBlock = new QVBoxLayout;
    sourceBlock->setSpacing(tokens::layout::kFormVSpacing);
    auto* sourceLabel = new QLabel(QStringLiteral("Add from"), this);
    sourceLabel->setObjectName(QStringLiteral("formSectionLabel"));
    source_ = new QComboBox(this);
    source_->setObjectName(QStringLiteral("walletSource"));
    source_->addItems({QStringLiteral("One seed"), QStringLiteral("Many seeds"),
                       QStringLiteral("Public keys (watch only)"), QStringLiteral("Many public keys (watch only)")});
    sourceLabel->setBuddy(source_);
    sourceBlock->addWidget(sourceLabel);
    sourceBlock->addWidget(source_);
    identity->addLayout(sourceBlock, 1);
    root->addLayout(identity);

    pages_ = new QStackedWidget(this);
    auto* wordsPage = new QWidget(pages_);
    auto* wordsColumn = new QVBoxLayout(wordsPage);
    wordsColumn->setContentsMargins(0, 0, 0, 0);
    wordsColumn->setSpacing(tokens::layout::kFormVSpacing);

    // The words; the live check is right under the pages.
    auto* wordsBlock = new QVBoxLayout;
    wordsBlock->setSpacing(tokens::layout::kFormVSpacing);
    auto* wordsLabel = new QLabel(QStringLiteral("Seed words"), this);
    wordsLabel->setObjectName(QStringLiteral("formSectionLabel"));
    words_ = new QPlainTextEdit(this);
    words_->setObjectName(QStringLiteral("seedWords"));
    words_->setTabChangesFocus(true);
    words_->setUndoRedoEnabled(false); // no undo stack keeping earlier versions of the words
    // Added to the hints already there (the editor's Qt::ImhMultiLine), not
    // replacing them.
    words_->setInputMethodHints(words_->inputMethodHints() | kSecretInputHints);
    words_->setPlaceholderText(
        QStringLiteral("Type or paste the 12–24 words, separated by spaces"));
    words_->setAccessibleName(QStringLiteral("Seed words"));
    wordsLabel->setBuddy(words_);
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText); // it can quote a wallet's name
    status_->setWordWrap(true);
    // The line keeps its room while it is empty, so the words field does not
    // jump when the first status appears.
    QSizePolicy keepRoom = status_->sizePolicy();
    keepRoom.setRetainSizeWhenHidden(true);
    status_->setSizePolicy(keepRoom);
    status_->setMinimumHeight(status_->fontMetrics().lineSpacing());
    setStatus(status_, QString(), Tone::Muted);
    wordsBlock->addWidget(wordsLabel);
    wordsBlock->addWidget(words_, 1);
    wordsColumn->addLayout(wordsBlock, 1); // the words field takes the spare height

    // The optional passphrase, shown only while the box is ticked.
    auto* passphraseBlock = new QVBoxLayout;
    passphraseBlock->setSpacing(tokens::layout::kFormVSpacing);
    hasPassphrase_ = new QCheckBox(QStringLiteral("This seed has a passphrase (25th word)"), this);
    passphrase_ = new QLineEdit(this);
    passphrase_->setEchoMode(QLineEdit::Password);
    // Added, not assigned: the password echo mode has just set Qt::ImhHiddenText.
    passphrase_->setInputMethodHints(passphrase_->inputMethodHints() | kSecretInputHints);
    passphrase_->setPlaceholderText(QStringLiteral("Passphrase"));
    passphrase_->setAccessibleName(QStringLiteral("Passphrase"));
    passphrase_->setMaximumWidth(tokens::layout::kFieldMaxW);
    passphrase_->setVisible(false);
    passphraseBlock->addWidget(hasPassphrase_);
    passphraseBlock->addWidget(passphrase_);
    pages_->addWidget(wordsPage);

    // A list of seeds, secret like the words.
    auto* seedListPage = new QWidget(pages_);
    auto* seedListColumn = new QVBoxLayout(seedListPage);
    seedListColumn->setContentsMargins(0, 0, 0, 0);
    seedListColumn->setSpacing(tokens::layout::kFormVSpacing);
    auto* seedListLabel = new QLabel(QStringLiteral("Seeds, one per line"), seedListPage);
    seedListLabel->setObjectName(QStringLiteral("formSectionLabel"));
    seedList_ = new QPlainTextEdit(seedListPage);
    seedList_->setObjectName(QStringLiteral("seedList"));
    seedList_->setTabChangesFocus(true);
    seedList_->setUndoRedoEnabled(false);
    seedList_->setLineWrapMode(QPlainTextEdit::NoWrap);
    seedList_->setInputMethodHints(seedList_->inputMethodHints() | kSecretInputHints);
    seedList_->setPlaceholderText(QStringLiteral("Shop 1: word word word ...\nShop 2: word word word ...\n"
                                                 "word word word ...   (a name is optional)"));
    seedList_->setAccessibleName(QStringLiteral("Seeds, one per line"));
    seedListLabel->setBuddy(seedList_);
    auto* loadSeeds = new QPushButton(QStringLiteral("Load from file"), seedListPage);
    loadSeeds->setToolTip(QStringLiteral("A text or CSV file with one seed per line. GitWallet only reads it: "
                                         "it stays where it is, so delete it yourself if you no longer need it."));
    seedListColumn->addWidget(seedListLabel);
    seedListColumn->addWidget(seedList_, 1);
    seedListColumn->addWidget(loadSeeds, 0, Qt::AlignLeft);
    pages_->addWidget(seedListPage);

    // Or public keys alone. Not secret themselves, but seed words pasted here by
    // mistake are, so these fields keep no undo history either.
    auto* keysPage = new QWidget(pages_);
    auto* keysColumn = new QVBoxLayout(keysPage);
    keysColumn->setContentsMargins(0, 0, 0, 0);
    keysColumn->setSpacing(tokens::layout::kFormVSpacing);
    auto* keysLabel = new QLabel(QStringLiteral("Bitcoin public keys"), keysPage);
    keysLabel->setObjectName(QStringLiteral("formSectionLabel"));
    keys_ = new QPlainTextEdit(keysPage);
    keys_->setObjectName(QStringLiteral("publicKeys"));
    keys_->setTabChangesFocus(true);
    keys_->setUndoRedoEnabled(false);
    keys_->setInputMethodHints(keys_->inputMethodHints() | kSecretInputHints);
    keys_->setPlaceholderText(QStringLiteral("One account key per line: zpub (native SegWit), ypub (nested "
                                             "SegWit) or xpub"));
    keysLabel->setBuddy(keys_);
    keysColumn->addWidget(keysLabel);
    keysColumn->addWidget(keys_, 1);
    xpubRow_ = new QWidget(keysPage);
    auto* xpubLayout = new QHBoxLayout(xpubRow_);
    xpubLayout->setContentsMargins(0, 0, 0, 0);
    xpubLayout->setSpacing(tokens::layout::kFormHSpacing);
    xpubLayout->addWidget(new QLabel(QStringLiteral("An xpub here is"), xpubRow_));
    xpubKind_ = new QComboBox(xpubRow_);
    xpubKind_->setObjectName(QStringLiteral("xpubKind"));
    xpubKind_->addItem(QStringLiteral("Native SegWit (m/84'/0'/…)"), int(chain::ScriptType::P2WPKH));
    xpubKind_->addItem(QStringLiteral("Nested SegWit (m/49'/0'/…)"), int(chain::ScriptType::P2SH_P2WPKH));
    xpubKind_->addItem(QStringLiteral("Legacy (m/44'/0'/…)"), int(chain::ScriptType::P2PKH));
    xpubKind_->setPlaceholderText(QStringLiteral("Choose"));
    xpubKind_->setCurrentIndex(-1);
    xpubKind_->setToolTip(QStringLiteral("Wallets write these accounts with the xpub prefix too, so it does not "
                                         "say which. The wrong choice shows other addresses: compare the first "
                                         "address below with the other wallet."));
    xpubLayout->addWidget(xpubKind_);
    xpubLayout->addStretch(1);
    xpubRow_->setVisible(false);
    keysColumn->addWidget(xpubRow_);
    auto* ethLabel = new QLabel(QStringLiteral("Ethereum public key (optional)"), keysPage);
    ethLabel->setObjectName(QStringLiteral("formSectionLabel"));
    ethKey_ = new QLineEdit(keysPage);
    ethKey_->setObjectName(QStringLiteral("ethereumKey"));
    ethKey_->setInputMethodHints(ethKey_->inputMethodHints() | kSecretInputHints);
    ethKey_->setPlaceholderText(QStringLiteral("The xpub of the account at m/44'/60'/0'"));
    ethLabel->setBuddy(ethKey_);
    keysColumn->addWidget(ethLabel);
    keysColumn->addWidget(ethKey_);
    pages_->addWidget(keysPage);

    // A list of public keys, one wallet per line.
    auto* keyListPage = new QWidget(pages_);
    auto* keyListColumn = new QVBoxLayout(keyListPage);
    keyListColumn->setContentsMargins(0, 0, 0, 0);
    keyListColumn->setSpacing(tokens::layout::kFormVSpacing);
    auto* keyListLabel = new QLabel(QStringLiteral("Public keys, one wallet per line"), keyListPage);
    keyListLabel->setObjectName(QStringLiteral("formSectionLabel"));
    keyList_ = new QPlainTextEdit(keyListPage);
    keyList_->setObjectName(QStringLiteral("keyList"));
    keyList_->setTabChangesFocus(true);
    keyList_->setUndoRedoEnabled(false);
    keyList_->setLineWrapMode(QPlainTextEdit::NoWrap);
    keyList_->setInputMethodHints(keyList_->inputMethodHints() | kSecretInputHints);
    keyList_->setPlaceholderText(QStringLiteral("Shop 1: zpub...\nShop 2: zpub... xpub...\n"
                                                "zpub...   (a name is optional)"));
    keyListLabel->setBuddy(keyList_);
    listKindRow_ = new QWidget(keyListPage);
    auto* listKindLayout = new QHBoxLayout(listKindRow_);
    listKindLayout->setContentsMargins(0, 0, 0, 0);
    listKindLayout->setSpacing(tokens::layout::kFormHSpacing);
    listKindLayout->addWidget(new QLabel(QStringLiteral("An xpub here is"), listKindRow_));
    listKind_ = new QComboBox(listKindRow_);
    listKind_->setObjectName(QStringLiteral("listKind"));
    listKind_->addItem(QStringLiteral("Native SegWit (m/84'/0'/…)"), int(chain::ScriptType::P2WPKH));
    listKind_->addItem(QStringLiteral("Nested SegWit (m/49'/0'/…)"), int(chain::ScriptType::P2SH_P2WPKH));
    listKind_->addItem(QStringLiteral("Legacy (m/44'/0'/…)"), int(chain::ScriptType::P2PKH));
    listKind_->addItem(QStringLiteral("Ethereum (m/44'/60'/…)"), int(chain::ScriptType::EthAccount));
    listKind_->setPlaceholderText(QStringLiteral("Choose"));
    listKind_->setCurrentIndex(-1);
    listKindLayout->addWidget(listKind_);
    listKindLayout->addStretch(1);
    listKindRow_->setVisible(false);
    auto* loadKeys = new QPushButton(QStringLiteral("Load from file"), keyListPage);
    keyListColumn->addWidget(keyListLabel);
    keyListColumn->addWidget(keyList_, 1);
    keyListColumn->addWidget(listKindRow_);
    keyListColumn->addWidget(loadKeys, 0, Qt::AlignLeft);
    pages_->addWidget(keyListPage);

    root->addWidget(pages_, 1);
    root->addLayout(passphraseBlock); // for the one seed or the whole list
    root->addWidget(status_);

    info_ = new QLabel(this);
    info_->setWordWrap(true);
    setTone(info_, Tone::Muted);
    root->addWidget(info_);

    // Cancel, then the primary action on the far right (the kit's order).
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(tokens::layout::kToolbarSpacing);
    buttons->addStretch(1);
    cancel_ = new QPushButton(QStringLiteral("Cancel"), this);
    import_ = new QPushButton(QStringLiteral("Add wallet"), this);
    import_->setObjectName(QStringLiteral("primaryButton"));
    import_->setDefault(true);
    import_->setEnabled(false);
    buttons->addWidget(cancel_);
    buttons->addWidget(import_);
    root->addLayout(buttons);

    connect(source_, &QComboBox::currentIndexChanged, this, &ImportSeedDialog::sourceChanged);
    connect(words_, &QPlainTextEdit::textChanged, this, &ImportSeedDialog::validateWords);
    connect(keys_, &QPlainTextEdit::textChanged, this, &ImportSeedDialog::validateKeys);
    connect(ethKey_, &QLineEdit::textChanged, this, &ImportSeedDialog::validateKeys);
    connect(xpubKind_, &QComboBox::currentIndexChanged, this, &ImportSeedDialog::validateKeys);
    connect(seedList_, &QPlainTextEdit::textChanged, this, &ImportSeedDialog::validateSeedList);
    connect(keyList_, &QPlainTextEdit::textChanged, this, &ImportSeedDialog::validateKeyList);
    connect(listKind_, &QComboBox::currentIndexChanged, this, &ImportSeedDialog::validateKeyList);
    connect(loadSeeds, &QPushButton::clicked, this, &ImportSeedDialog::chooseListFile);
    connect(loadKeys, &QPushButton::clicked, this, &ImportSeedDialog::chooseListFile);
    connect(name_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (!wallet::looksLikeSeedWords(text))
            return;
        setStatus(status_,
                  trText(QStringLiteral("That looks like seed words, so it was not kept as a name. "
                                        "Paste them in the field for seeds.")),
                  Tone::Error);
        // Queued: not from inside the field's own signal.
        QMetaObject::invokeMethod(this, [this] { wipeLineEdit(name_); }, Qt::QueuedConnection);
    });
    connect(hasPassphrase_, &QCheckBox::toggled, this, [this](bool on) {
        // Unticked means "no passphrase": what was typed must not be imported
        // unseen, so it goes at once.
        if (!on)
            wipeLineEdit(passphrase_);
        passphrase_->setVisible(on);
        if (on)
            passphrase_->setFocus();
        updateImportEnabled();
    });
    connect(passphrase_, &QLineEdit::textChanged, this, &ImportSeedDialog::updateImportEnabled);
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    connect(import_, &QPushButton::clicked, this, &ImportSeedDialog::importSeed);

    source_->setCurrentIndex(many ? ManySeeds : OneSeed);
    sourceChanged();
    // A phrase pasted straight away lands where it belongs.
    (many ? seedList_ : words_)->setFocus();
    page::fitDialog(this, page::DialogSize::Form);
}

ImportSeedDialog::~ImportSeedDialog()
{
    // Closing goes through done(); this covers a dialog destroyed while open.
    stopList();
    wipeFields();
}

void ImportSeedDialog::done(int result)
{
    stopList();
    wipeFields();
    // Shown again, the dialog starts blank: no stale status, no passphrase.
    hasPassphrase_->setChecked(false);
    xpubKind_->setCurrentIndex(-1);
    listKind_->setCurrentIndex(-1);
    source_->setCurrentIndex(OneSeed);
    setStatus(status_, QString(), Tone::Muted);
    updateImportEnabled();
    QDialog::done(result);
}

int ImportSeedDialog::source() const
{
    return pages_->currentIndex();
}

void ImportSeedDialog::sourceChanged()
{
    const int chosen = source_->currentIndex();
    pages_->setCurrentIndex(chosen);
    const bool seeds = chosen == OneSeed || chosen == ManySeeds;
    const bool many = chosen == ManySeeds || chosen == ManyKeys;
    hasPassphrase_->setVisible(seeds);
    passphrase_->setVisible(seeds && hasPassphrase_->isChecked());
    nameLabel_->setText(many ? trText(QStringLiteral("Name for lines without one")) : trText(QStringLiteral("Wallet name")));
    name_->setPlaceholderText(many ? trText(QStringLiteral("For example: Shop, which gives Shop 1, Shop 2"))
                                   : trText(QStringLiteral("For example: Online shop")));
    import_->setText(many ? trText(QStringLiteral("Add wallets")) : trText(QStringLiteral("Add wallet")));
    info_->setText(seeds ? trText(QStringLiteral("GitWallet derives the public keys of your accounts from each "
                                                 "seed and keeps only those, encrypted for your Windows user. "
                                                 "A seed itself is never saved."))
                         : trText(QStringLiteral("Public keys show addresses and balances but can never spend. "
                                                 "A wallet added this way needs no seed on this PC.")));
    switch (chosen) {
    case ManySeeds:
        validateSeedList();
        seedList_->setFocus();
        break;
    case OneKeys:
        validateKeys();
        keys_->setFocus();
        break;
    case ManyKeys:
        validateKeyList();
        keyList_->setFocus();
        break;
    default:
        validateWords();
        words_->setFocus();
        break;
    }
}

void ImportSeedDialog::validateSeedList()
{
    if (running_ || source() != ManySeeds)
        return;
    listReady_ = 0;
    QString message;
    Tone tone = Tone::Muted;
    try {
        std::string text;
        takePhrase(seedList_, text, true);
        const std::vector<wallet::SeedLine> lines = wallet::parseSeedLines(text);
        chain::secureClear(text);
        QStringList problems;
        for (const wallet::SeedLine& line : lines) {
            if (line.problem.isEmpty())
                ++listReady_;
            else
                problems << trText(QStringLiteral("Line %1: %2")).arg(line.number).arg(line.problem);
        }
        message = listSummary(listReady_, trText(QStringLiteral("1 seed ready")), trText(QStringLiteral("%1 seeds ready")),
                              problems);
        tone = !problems.isEmpty() ? (listReady_ > 0 ? Tone::Warning : Tone::Error)
                                   : (listReady_ > 0 ? Tone::Success : Tone::Muted);
        // What a seed asks of the one connection, at the gaps set now: each of its
        // three Bitcoin account types watches both of its chains.
        if (controller_ && listReady_ > 0) {
            const wallet::ScanSettings& s = controller_->store().settings;
            const qint64 perSeed = 3 * (qint64(s.btcReceiveGap) + s.btcChangeGap);
            const qint64 needed = (controller_->store().wallets.size() + listReady_) * perSeed;
            if (needed > wallet::BtcScanner::kMaxFollowedRows) {
                message += QLatin1Char('\n')
                           + trText(QStringLiteral("At these gaps the wallets need about %1 Bitcoin addresses, more than "
                                                   "the %2 one connection follows. Lower the gaps in Settings, or some "
                                                   "wallets will read as not fully scanned."))
                                 .arg(needed)
                                 .arg(wallet::BtcScanner::kMaxFollowedRows);
                tone = Tone::Warning;
            }
        }
    } catch (const std::exception&) {
        listReady_ = 0;
        message = trText(QStringLiteral("Could not check the list"));
        tone = Tone::Error;
    }
    setStatus(status_, message, tone);
    updateImportEnabled();
}

void ImportSeedDialog::validateKeyList()
{
    if (running_ || source() != ManyKeys)
        return;
    listReady_ = 0;
    const QString text = keyList_->toPlainText();

    const QStringList raw = text.split(QLatin1Char('\n'));
    if (std::any_of(raw.cbegin(), raw.cend(), [](const QString& line) { return wallet::looksLikeSeedWords(line); })) {
        setStatus(status_,
                  trText(QStringLiteral("Those look like seed words, not public keys, so they were removed. "
                                        "Choose Many seeds to add seeds.")),
                  Tone::Error);
        updateImportEnabled();
        // Queued: not from inside the field's own signal.
        QMetaObject::invokeMethod(this, [this] { wipeTextEdit(keyList_); }, Qt::QueuedConnection);
        return;
    }

    const QList<wallet::KeyLine> lines = wallet::parseKeyLines(text);
    const bool anyXpub = std::any_of(lines.cbegin(), lines.cend(), [](const wallet::KeyLine& line) {
        return std::any_of(line.keys.cbegin(), line.keys.cend(),
                           [](const QString& key) { return key.startsWith(QLatin1String("xpub")); });
    });
    listKindRow_->setVisible(anyXpub);
    if (anyXpub && listKind_->currentIndex() < 0) {
        setStatus(status_, trText(QStringLiteral("Choose what an xpub here is: its prefix does not say.")), Tone::Muted);
        updateImportEnabled();
        return;
    }
    const auto xpubType = anyXpub ? static_cast<chain::ScriptType>(listKind_->currentData().toInt())
                                  : chain::ScriptType::P2PKH;
    QStringList problems;
    for (const wallet::KeyLine& line : lines) {
        QString why = line.problem;
        QList<wallet::AccountKey> accounts;
        if (why.isEmpty())
            accountsOfLine(line, xpubType, &accounts, &why);
        if (why.isEmpty())
            ++listReady_;
        else
            problems << trText(QStringLiteral("Line %1: %2")).arg(line.number).arg(why);
    }
    const QString message = listSummary(listReady_, trText(QStringLiteral("1 wallet ready")),
                                        trText(QStringLiteral("%1 wallets ready")), problems);
    setStatus(status_, message,
              !problems.isEmpty() ? (listReady_ > 0 ? Tone::Warning : Tone::Error)
                                  : (listReady_ > 0 ? Tone::Success : Tone::Muted));
    updateImportEnabled();
}

void ImportSeedDialog::validateKeys()
{
    wordsValid_ = false; // this source does not use them
    parsedKeys_.clear();

    if (wallet::looksLikeSeedWords(keys_->toPlainText()) || wallet::looksLikeSeedWords(ethKey_->text())) {
        setStatus(status_,
                  trText(QStringLiteral("Those look like seed words, not public keys, so they were removed. "
                                        "Choose One seed to add a seed.")),
                  Tone::Error);
        updateImportEnabled();
        // Queued: not from inside the fields' own signals.
        QMetaObject::invokeMethod(this, [this] {
            wipeTextEdit(keys_);
            wipeLineEdit(ethKey_);
        }, Qt::QueuedConnection);
        return;
    }

    QString message;
    Tone tone = Tone::Muted;
    static const QRegularExpression space(QStringLiteral("\\s+"));
    const QStringList bitcoin = keys_->toPlainText().split(space, Qt::SkipEmptyParts);
    const QStringList ethereum = ethKey_->text().split(space, Qt::SkipEmptyParts);
    const bool anyXpub = std::any_of(bitcoin.cbegin(), bitcoin.cend(), [](const QString& key) {
        return key.startsWith(QLatin1String("xpub"));
    });
    xpubRow_->setVisible(anyXpub);
    if (anyXpub && xpubKind_->currentIndex() < 0) {
        message = trText(QStringLiteral("Choose what an xpub here is: its prefix does not say."));
    } else if (!bitcoin.isEmpty() || !ethereum.isEmpty()) {
        const auto xpubType = anyXpub ? static_cast<chain::ScriptType>(xpubKind_->currentData().toInt())
                                      : chain::ScriptType::P2PKH;
        QString why;
        QList<wallet::AccountKey> parsed;
        if (wallet::WalletStore::accountsFromPublicKeys(bitcoin, ethereum, xpubType, &parsed, &why)) {
            try {
                QStringList lines;
                lines << (parsed.size() == 1 ? trText(QStringLiteral("1 account key · OK. Compare its first "
                                                                     "address with the other wallet:"))
                                             : trText(QStringLiteral("%1 account keys · OK. Compare their first "
                                                                     "addresses with the other wallet:"))
                                                   .arg(parsed.size()));
                constexpr qsizetype kShownKeys = 4; // room in the dialog; the rest pass the same checks
                for (qsizetype i = 0; i < parsed.size() && i < kShownKeys; ++i)
                    lines << QStringLiteral("%1: %2").arg(accountLabel(parsed.at(i)), firstAddress(parsed.at(i)));
                message = lines.join(QLatin1Char('\n'));
                parsedKeys_ = parsed;
                tone = Tone::Success;
            } catch (const std::exception&) {
                message = trText(QStringLiteral("A public key could not be used to derive an address."));
                tone = Tone::Error;
            }
        } else {
            message = why;
            tone = Tone::Error;
        }
    }
    setStatus(status_, message, tone);
    updateImportEnabled();
}

void ImportSeedDialog::wipeFields()
{
    wipeTextEdit(words_);
    wipeLineEdit(passphrase_);
    // Not meant for secrets, but a phrase can land there by mistake.
    wipeLineEdit(name_);
    wipeTextEdit(keys_);
    wipeLineEdit(ethKey_);
    wipeTextEdit(seedList_);
    wipeTextEdit(keyList_);
    // The live check and the import hold their std::string copies in a
    // SeedCopy that scrubs itself before the slot returns, so the editors are
    // the only place left where the words live.
    wordsValid_ = false;
    parsedKeys_.clear();
    listReady_ = 0;
}

void ImportSeedDialog::validateWords()
{
    wordsValid_ = false;
    QString message;
    Tone tone = Tone::Muted;
    try {
        SeedCopy seed;
        takePhrase(words_, seed.text);
        seed.words = chain::bip39::split(seed.text);
        const size_t count = seed.words.size();
        // A last word with no whitespace after it may still be being typed:
        // while it can grow into a list word it is not an error yet.
        const bool typing = count > 0
                            && !std::isspace(static_cast<unsigned char>(seed.text.back()))
                            && !isBip39Word(seed.words.back())
                            && isBip39Prefix(seed.words.back());
        size_t unknown = count;
        for (size_t i = 0; i < count && unknown == count; ++i) {
            if (!(typing && i + 1 == count) && !isBip39Word(seed.words[i]))
                unknown = i;
        }

        if (count == 0) {
            // Nothing typed yet: no status.
        } else if (unknown < count) {
            message = trText(QStringLiteral("Word %1 is not in the BIP39 list"))
                          .arg(static_cast<int>(unknown + 1));
            tone = Tone::Error;
        } else if (!isValidWordCount(count)) {
            message = trText(QStringLiteral("Needs 12, 15, 18, 21 or 24 words (now %1)"))
                          .arg(static_cast<int>(count));
        } else if (typing) {
            // The right count, but the last word is unfinished: a muted note
            // until a space or the missing letters follow.
            message = trText(QStringLiteral("Word %1 is not in the BIP39 list"))
                          .arg(static_cast<int>(count));
        } else if (!chain::bip39::validate(seed.words)) {
            message =
                trText(QStringLiteral("Checksum does not match — check the words and their order"));
            tone = Tone::Error;
        } else {
            message = trText(QStringLiteral("%1 words · checksum OK")).arg(static_cast<int>(count));
            tone = Tone::Success;
            wordsValid_ = true;
        }
    } catch (const std::exception&) {
        message = trText(QStringLiteral("Could not check the words"));
        tone = Tone::Error;
        wordsValid_ = false;
    }
    setStatus(status_, message, tone);
    updateImportEnabled();
}

void ImportSeedDialog::updateImportEnabled()
{
    // A ticked box with an empty field is unfinished input, not "no passphrase".
    const bool passphraseReady = !hasPassphrase_->isChecked() || !passphrase_->text().isEmpty();
    bool ready = false;
    switch (source()) {
    case ManySeeds: ready = listReady_ > 0 && passphraseReady; break;
    case OneKeys: ready = !parsedKeys_.isEmpty(); break;
    case ManyKeys: ready = listReady_ > 0; break;
    default: ready = wordsValid_ && passphraseReady; break;
    }
    import_->setEnabled(controller_ != nullptr && !running_ && ready);
}

void ImportSeedDialog::importSeed()
{
    if (!controller_ || running_)
        return;
    switch (source()) {
    case ManySeeds: startSeedList(); break;
    case OneKeys: addOneKeys(); break;
    case ManyKeys: addKeyList(); break;
    default: addOneSeed(); break;
    }
}

void ImportSeedDialog::addOneKeys()
{
    QString error;
    if (controller_->addKeysWallet(parsedKeys_, name_->text(), &error)) {
        result_ = trText(QStringLiteral("Wallet added"));
        accept();
    } else {
        setStatus(status_, error.isEmpty() ? trText(QStringLiteral("Could not add the keys")) : error, Tone::Error);
    }
}

void ImportSeedDialog::addOneSeed()
{
    if (!wordsValid_)
        return;

    QString error;
    bool imported = false;
    try {
        SeedCopy seed;
        takePhrase(words_, seed.text); // read exactly as the live check read it
        seed.words = chain::bip39::split(seed.text);
        chain::secureClear(seed.text); // the words are all that is needed from here on
        if (hasPassphrase_->isChecked()) {
            // BIP39 salts with the NFKD form of the passphrase.
            QString passphrase = passphrase_->text().normalized(QString::NormalizationForm_KD);
            takeUtf8(passphrase, seed.passphrase);
        }
        imported = controller_->addMnemonicWallet(seed.words, seed.passphrase, name_->text(), &error);
    } catch (const std::exception&) {
        imported = false;
        error.clear();
    }
    // The SeedCopy is scrubbed by now, before accept() lets anything else run.

    if (!imported) {
        setStatus(status_,
                  error.isEmpty() ? trText(QStringLiteral("Could not add the seed")) : error,
                  Tone::Error);
        return;
    }
    result_ = trText(QStringLiteral("Wallet added"));
    accept();
}

// ------------------------------------------------------------------ lists ----

QString ImportSeedDialog::labelFor(const QString& name)
{
    if (!name.isEmpty())
        return name;
    const QString base = name_->text().trimmed();
    return base.isEmpty() ? QString() : QStringLiteral("%1 %2").arg(base).arg(++unnamed_);
}

void ImportSeedDialog::setBusy(bool busy)
{
    running_ = busy;
    pages_->setEnabled(!busy);
    source_->setEnabled(!busy);
    name_->setEnabled(!busy);
    hasPassphrase_->setEnabled(!busy);
    passphrase_->setEnabled(!busy);
    updateImportEnabled();
}

void ImportSeedDialog::stopList()
{
    running_ = false;
    pending_.clear(); // each line scrubs its words
    chain::secureClear(listPassphrase_);
}

void ImportSeedDialog::startSeedList()
{
    listRequests_.clear();
    listToFix_.clear();
    pendingNext_ = 0;
    pendingDone_ = 0;
    pendingTotal_ = 0;
    try {
        std::string text;
        takePhrase(seedList_, text, true); // read exactly as the live check read it
        pending_ = wallet::parseSeedLines(text);
        chain::secureClear(text);
        if (hasPassphrase_->isChecked()) {
            // BIP39 salts with the NFKD form of the passphrase.
            QString passphrase = passphrase_->text().normalized(QString::NormalizationForm_KD);
            takeUtf8(passphrase, listPassphrase_);
        }
    } catch (const std::exception&) {
        stopList();
        setStatus(status_, trText(QStringLiteral("Could not read the list")), Tone::Error);
        return;
    }
    for (const wallet::SeedLine& line : pending_) {
        if (line.problem.isEmpty())
            ++pendingTotal_;
        else
            listToFix_.insert(line.number, line.problem);
    }
    setBusy(true);
    QMetaObject::invokeMethod(this, &ImportSeedDialog::seedListStep, Qt::QueuedConnection);
}

void ImportSeedDialog::seedListStep()
{
    if (!running_)
        return; // closed meanwhile
    while (pendingNext_ < pending_.size() && !pending_[pendingNext_].problem.isEmpty())
        ++pendingNext_;
    if (pendingNext_ >= pending_.size()) {
        const QMap<int, QString> toFix = listToFix_;
        stopList();
        finishList(seedList_, toFix);
        return;
    }
    wallet::SeedLine& line = pending_[pendingNext_++];
    try {
        wallet::WalletRequest request;
        request.accounts = wallet::WalletStore::deriveAccounts(line.words, listPassphrase_);
        request.label = labelFor(line.name);
        request.line = line.number;
        listRequests_.append(request);
    } catch (const std::exception&) {
        listToFix_.insert(line.number, trText(QStringLiteral("its keys could not be derived")));
    }
    line.wipe();
    ++pendingDone_;
    setStatus(status_, trText(QStringLiteral("Working out the public keys: %1 of %2")).arg(pendingDone_).arg(pendingTotal_),
              Tone::Muted);
    QMetaObject::invokeMethod(this, &ImportSeedDialog::seedListStep, Qt::QueuedConnection);
}

void ImportSeedDialog::addKeyList()
{
    listRequests_.clear();
    QMap<int, QString> toFix;
    const auto xpubType = listKindRow_->isVisibleTo(this) && listKind_->currentIndex() >= 0
                              ? static_cast<chain::ScriptType>(listKind_->currentData().toInt())
                              : chain::ScriptType::P2PKH;
    const QList<wallet::KeyLine> lines = wallet::parseKeyLines(keyList_->toPlainText());
    for (const wallet::KeyLine& line : lines) {
        QString why = line.problem;
        wallet::WalletRequest request;
        if (why.isEmpty())
            accountsOfLine(line, xpubType, &request.accounts, &why);
        if (!why.isEmpty()) {
            toFix.insert(line.number, why);
            continue;
        }
        request.label = labelFor(line.name);
        request.fromKeys = true;
        request.line = line.number;
        listRequests_.append(request);
    }
    finishList(keyList_, toFix);
}

void ImportSeedDialog::finishList(QPlainTextEdit* field, const QMap<int, QString>& toFix)
{
    QString error;
    const int added = controller_->addWallets(&listRequests_, &error);
    setBusy(false);
    if (!error.isEmpty()) {
        // Nothing went in: the list stays as it is for another try.
        setStatus(status_, error, Tone::Error);
        listRequests_.clear();
        return;
    }

    // What went in leaves the list; what did not stays, each line with why.
    QMap<int, QString> stays = toFix;
    for (const wallet::WalletRequest& request : std::as_const(listRequests_)) {
        if (!request.refused.isEmpty())
            stays.insert(request.line, request.refused);
    }
    listRequests_.clear();
    addedTotal_ += added;
    if (addedTotal_ > 0) {
        result_ = addedTotal_ == 1 ? trText(QStringLiteral("1 wallet added"))
                                   : trText(QStringLiteral("%1 wallets added")).arg(addedTotal_);
        cancel_->setText(trText(QStringLiteral("Close"))); // leaving now takes nothing back
    }
    if (stays.isEmpty()) {
        accept();
        return;
    }

    // Renumbered as the field now stands.
    QString text = field->toPlainText();
    QStringList lines = text.split(QLatin1Char('\n'));
    QStringList kept;
    QStringList notes;
    for (auto it = stays.cbegin(); it != stays.cend(); ++it) {
        if (it.key() < 1 || it.key() > lines.size())
            continue;
        kept << lines.at(it.key() - 1);
        QString why = it.value();
        if (why.endsWith(QLatin1Char('.')))
            why.chop(1);
        notes << trText(QStringLiteral("Line %1: %2")).arg(kept.size()).arg(why);
    }
    QString left = kept.join(QLatin1Char('\n'));
    wipeTextEdit(field);
    {
        const QSignalBlocker quiet(field);
        field->setPlainText(left);
    }
    // All of them can hold seed words. A line shared by both lists detaches from the
    // first fill, so the second is what zeroes its buffer.
    text.fill(QChar(u'\0'));
    left.fill(QChar(u'\0'));
    for (QString& line : lines)
        line.fill(QChar(u'\0'));
    for (QString& line : kept)
        line.fill(QChar(u'\0'));
    listReady_ = 0;
    updateImportEnabled();
    const QString fix = listSummary(0, QString(), QString(), notes);
    setStatus(status_, result_.isEmpty() ? fix : result_ + QLatin1Char('\n') + fix,
              addedTotal_ > 0 ? Tone::Warning : Tone::Error);
}

void ImportSeedDialog::chooseListFile()
{
    const QString path = QFileDialog::getOpenFileName(this, trText(QStringLiteral("Load a list")), QString(),
                                                      trText(QStringLiteral("Text or CSV (*.txt *.csv);;All files (*)")));
    if (!path.isEmpty())
        loadListFile(path);
}

bool ImportSeedDialog::loadListFile(const QString& path)
{
    QPlainTextEdit* field = source() == ManyKeys ? keyList_ : seedList_;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(status_, trText(QStringLiteral("Could not open the file: %1")).arg(file.errorString()), Tone::Error);
        return false;
    }
    if (file.size() > kMaxListFileBytes) {
        setStatus(status_, trText(QStringLiteral("That file is too large to be a list.")), Tone::Error);
        return false;
    }
    QByteArray bytes = file.readAll();
    QString text = QString::fromUtf8(bytes);
    bytes.fill('\0');
    text.remove(QChar(u'\r'));
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    field->setPlainText(text); // the field's own check says what it found
    text.fill(QChar(u'\0'));
    return true;
}

} // namespace app::ui
