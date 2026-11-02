#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <string>
#include <vector>

// Reads what a person pastes or loads to add many wallets at once: one wallet per
// line, with an optional name beside it ("Shop 1: word word ...", "word word ..., Shop 1").
// Marks are ':', a tab, ',' and ';', and a field in quotes holds whatever is inside it,
// so a two-column CSV reads as it is. A line numbered "12." or "12)" is the same line
// without the number; blank lines, blank columns and lines starting with '#' are
// skipped, unless what follows the '#' is a phrase.
//
// A line goes in only when it has exactly one reading. A checksum proves little (1 in 16
// for 12 words), so taking a name for part of a phrase, or the end of a phrase for a seed
// of its own, would show another seed's balances and write seed words to disk as a name.
// Every mark is tried; two readings, a mark inside the only phrase, or a name that could
// be part of the phrase are refused and say so.
namespace wallet {

// One line of seeds. The words are secret: they are scrubbed when the line goes, and
// a line with a problem holds none. A problem names a word by position, never by text.
struct SeedLine {
    int number = 0;                 // 1-based line of the text
    QString name;                   // may be empty
    std::vector<std::string> words; // lowercase, a valid BIP39 phrase unless `problem` says otherwise
    QString problem;                // empty when the words can be used

    SeedLine() = default;
    SeedLine(SeedLine&&) noexcept = default;
    SeedLine(const SeedLine&) = delete;
    SeedLine& operator=(const SeedLine&) = delete;
    SeedLine& operator=(SeedLine&&) = delete;
    ~SeedLine() { wipe(); }

    void wipe(); // scrubs and drops the words and the name
};

// `text` holds its lines separated by '\n'. It may hold tabs, not other control
// characters. Words in compatibility forms (full-width letters, a ligature) read as
// BIP39's NFKD makes them; a name stays as typed.
std::vector<SeedLine> parseSeedLines(const std::string& text);

// One line of public keys: one wallet. Its name stands before the first key or, if
// nothing does, after the last.
struct KeyLine {
    int number = 0;
    QString name;
    QStringList keys; // everything on the line that reads as a zpub, ypub or xpub
    QString problem;
};

QList<KeyLine> parseKeyLines(const QString& text);

// A phrase in a place that is not for seeds, read the way the seed fields read it: of 12
// words in a row, whatever parts them, at least 10 in the BIP39 list, so a word or two
// mistyped still counts. A real name or a sentence is hardly ever that.
bool looksLikeSeedWords(QStringView text);
// An extended private key, a WIF or a raw key in hex, in groups or not, also behind
// invisible characters or in full-width letters.
bool looksLikePrivateKey(QStringView text);
bool looksLikeSecret(QStringView text);
// The 0-based lines of a list of public keys that hold one, comments included.
QList<int> linesHoldingSecrets(const QString& text);

// A list file as Windows programs save one: by its mark, as UTF-16 with no mark, as
// UTF-8, or in this PC's code page, as Excel saves a CSV. False if it is none of them: not
// text, or two encodings at once, where which letters are right cannot be known.
bool decodeListFile(const QByteArray& bytes, QString* out);

} // namespace wallet
