#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

// Reads what a person pastes or loads to add many wallets at once: one wallet per
// line, with an optional name in front ("Shop 1: word word ..."). A separator of
// ':', ';', ',' or a tab follows the name, so a two-column CSV reads as it is; a line
// numbered "12." or "12)" is the same line without the number; blank lines and lines
// starting with '#' are skipped.
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

    void wipe(); // scrubs and drops the words
};

// `text` holds its lines separated by '\n'. It may hold tabs, not other control characters.
std::vector<SeedLine> parseSeedLines(const std::string& text);

// One line of public keys: one wallet.
struct KeyLine {
    int number = 0;
    QString name;
    QStringList keys; // everything on the line that reads as a zpub, ypub or xpub
    QString problem;
};

QList<KeyLine> parseKeyLines(const QString& text);

// A phrase in a place that is not for seeds: six or more BIP39 words on one line. A
// name cut at the longest a wallet's may be still holds that many, and a real name
// hardly ever does.
bool looksLikeSeedWords(const QString& line);

} // namespace wallet
