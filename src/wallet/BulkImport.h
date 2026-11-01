#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <string>
#include <vector>

// Reads what a person pastes or loads to add many wallets at once: one wallet per
// line, with an optional name ("Shop 1: word word ...", "word word ..., Shop 1"). A
// name ends at a ':' or a tab, else at the first ',' or ';', and a first field in
// quotes is that field whatever it holds, so a two-column CSV reads as it is. A line
// numbered "12." or "12)" is the same line without the number; blank lines and lines
// starting with '#' are skipped, unless what follows the '#' is a phrase.
//
// A line that can be read two ways is refused, not guessed at: taking a name for part
// of a phrase, or the end of a phrase for a seed of its own, would show another seed's
// balances and write seed words to disk as a name.
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

// One line of public keys: one wallet.
struct KeyLine {
    int number = 0;
    QString name;
    QStringList keys; // everything on the line that reads as a zpub, ypub or xpub
    QString problem;
};

QList<KeyLine> parseKeyLines(const QString& text);

// A phrase in a place that is not for seeds: a run of 12 BIP39 words on a line, whatever
// parts them, read the way the seed fields read them. With `cutAt`, the length the text
// was cut to if it was: at that length a run of 5 is enough, which is what is left of a
// longer phrase. A real name or a sentence is hardly ever such a run.
bool looksLikeSeedWords(QStringView text, int cutAt = 0);
// An extended private key, a WIF or a raw key in hex anywhere on the line.
bool looksLikePrivateKey(QStringView text);
bool looksLikeSecret(QStringView text, int cutAt = 0);
// Any line of a list of public keys holds one, comments included.
bool keyListHoldsSecret(const QString& text);

} // namespace wallet
