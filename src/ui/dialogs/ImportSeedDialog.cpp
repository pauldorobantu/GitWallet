#include "ui/dialogs/ImportSeedDialog.h"

#include "app/WalletController.h"
#include "chain/Bip39.h"
#include "chain/Hash.h"
#include "ui/DialogChrome.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/design/Style.h"

#include <QByteArray>
#include <QCheckBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
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
// that passed the check are the words imported.
void takePhrase(const QPlainTextEdit* edit, std::string& out)
{
    QString typed = edit->toPlainText();
    QString nfkd = typed.normalized(QString::NormalizationForm_KD);
    QString phrase;
    phrase.reserve(nfkd.size()); // never outgrown, so never copied on the way
    for (const QChar c : std::as_const(nfkd)) {
        if (c.category() == QChar::Other_Format)
            continue;
        phrase.append(c.isSpace() ? QChar(u' ') : c);
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

} // namespace

ImportSeedDialog::ImportSeedDialog(WalletController* controller, QWidget* parent)
    : QDialog(parent)
    , controller_(controller)
{
    setWindowTitle(QStringLiteral("Import seed"));
    // The themed title bar and frame go on before anything is laid out or shown.
    applyDialogChrome(this);

    auto* root = new QVBoxLayout(this);
    page::applyDialogLayout(root);

    // The words, with the live check right under them.
    auto* wordsBlock = new QVBoxLayout;
    wordsBlock->setSpacing(tokens::layout::kFormVSpacing);
    auto* wordsLabel = new QLabel(QStringLiteral("Seed words"), this);
    wordsLabel->setObjectName(QStringLiteral("formSectionLabel"));
    words_ = new QPlainTextEdit(this);
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
    wordsBlock->addWidget(status_);
    root->addLayout(wordsBlock, 1); // the words field takes the spare height

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
    root->addLayout(passphraseBlock);

    auto* info = new QLabel(
        QStringLiteral("GitWallet derives the public keys of your accounts from the seed and "
                       "keeps only those, encrypted for your Windows user. The seed itself is "
                       "never saved."),
        this);
    info->setWordWrap(true);
    setTone(info, Tone::Muted);
    root->addWidget(info);

    // Cancel, then the primary action on the far right (the kit's order).
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(tokens::layout::kToolbarSpacing);
    buttons->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
    import_ = new QPushButton(QStringLiteral("Import"), this);
    import_->setObjectName(QStringLiteral("primaryButton"));
    import_->setDefault(true);
    import_->setEnabled(false);
    buttons->addWidget(cancel);
    buttons->addWidget(import_);
    root->addLayout(buttons);

    connect(words_, &QPlainTextEdit::textChanged, this, &ImportSeedDialog::validateWords);
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
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(import_, &QPushButton::clicked, this, &ImportSeedDialog::importSeed);

    words_->setFocus();
    page::fitDialog(this, page::DialogSize::Form);
}

ImportSeedDialog::~ImportSeedDialog()
{
    // Closing goes through done(); this covers a dialog destroyed while open.
    wipeFields();
}

void ImportSeedDialog::done(int result)
{
    wipeFields();
    // Shown again, the dialog starts blank: no stale status, no passphrase.
    hasPassphrase_->setChecked(false);
    setStatus(status_, QString(), Tone::Muted);
    updateImportEnabled();
    QDialog::done(result);
}

void ImportSeedDialog::wipeFields()
{
    wipeTextEdit(words_);
    wipeLineEdit(passphrase_);
    // The live check and the import hold their std::string copies in a
    // SeedCopy that scrubs itself before the slot returns, so the editors are
    // the only place left where the words live.
    wordsValid_ = false;
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
    import_->setEnabled(controller_ != nullptr && wordsValid_ && passphraseReady);
}

void ImportSeedDialog::importSeed()
{
    if (!controller_ || !wordsValid_)
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
        imported = controller_->importMnemonic(seed.words, seed.passphrase, &error);
    } catch (const std::exception&) {
        imported = false;
        error.clear();
    }
    // The SeedCopy is scrubbed by now, before accept() lets anything else run.

    if (!imported) {
        setStatus(status_,
                  error.isEmpty() ? trText(QStringLiteral("Could not import the seed")) : error,
                  Tone::Error);
        return;
    }
    accept();
}

} // namespace app::ui
