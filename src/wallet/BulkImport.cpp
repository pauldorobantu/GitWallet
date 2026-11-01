#include "wallet/BulkImport.h"

#include "chain/Bip39.h"
#include "chain/Hash.h"
#include "wallet/WalletStore.h"

#include <QRegularExpression>

#include <cstddef>

namespace wallet {

namespace {

// A phrase has at most 24 words: past one more, only the count matters, and it says
// "too many". Reserved up front: a word's buffer sits inside the vector's, so a vector
// that grew would leave copies of the words behind in the memory it freed.
constexpr size_t kMaxKeptWords = 25;

bool isNameEnd(char c)
{
    return c == ':' || c == ';' || c == ',' || c == '\t';
}

bool isGap(char c)
{
    return c == ' ' || c == '"' || c == '\r' || isNameEnd(c);
}

bool isValidWordCount(size_t count)
{
    return count == 12 || count == 15 || count == 18 || count == 21 || count == 24;
}

void scrub(std::vector<std::string>& words)
{
    for (std::string& word : words)
        chain::secureClear(word);
    words.clear();
}

// The words of [from, to), lowercase (the ASCII of the BIP39 list).
std::vector<std::string> wordsIn(const std::string& text, size_t from, size_t to)
{
    std::vector<std::string> out;
    out.reserve(kMaxKeptWords);
    size_t i = from;
    while (i < to && out.size() < kMaxKeptWords) {
        while (i < to && isGap(text[i]))
            ++i;
        size_t end = i;
        while (end < to && !isGap(text[end]))
            ++end;
        if (end > i) {
            std::string word;
            word.reserve(end - i);
            for (size_t k = i; k < end; ++k) {
                const char c = text[k];
                word.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
            }
            out.push_back(std::move(word));
        }
        i = end;
    }
    return out;
}

// "12. " and "12) " at the start of a line: a numbered list, not a name.
size_t afterListNumber(const std::string& text, size_t begin, size_t end)
{
    size_t p = begin;
    while (p < end && text[p] >= '0' && text[p] <= '9')
        ++p;
    if (p == begin || p + 1 >= end || (text[p] != '.' && text[p] != ')') || text[p + 1] != ' ')
        return begin;
    p += 2;
    while (p < end && text[p] == ' ')
        ++p;
    return p;
}

QString problemWith(const std::vector<std::string>& words, size_t count)
{
    if (!isValidWordCount(count)) {
        return count >= kMaxKeptWords
                   ? QStringLiteral("it needs 12, 15, 18, 21 or 24 words (it has more)")
                   : QStringLiteral("it needs 12, 15, 18, 21 or 24 words (it has %1)").arg(count);
    }
    for (size_t i = 0; i < words.size(); ++i) {
        if (!chain::bip39::isWord(words[i]))
            return QStringLiteral("word %1 is not in the BIP39 list").arg(i + 1);
    }
    // `why` can quote a word, so it goes unread.
    std::string why;
    const bool valid = chain::bip39::validate(words, &why);
    chain::secureClear(why);
    return valid ? QString() : QStringLiteral("the checksum does not match: a word is wrong or out of order");
}

QString trimmedName(const std::string& text, size_t from, size_t to)
{
    QString name = QString::fromUtf8(text.data() + from, qsizetype(to - from)).trimmed();
    while (name.startsWith(QLatin1Char('"')))
        name.remove(0, 1);
    while (name.endsWith(QLatin1Char('"')))
        name.chop(1);
    // The list was read in NFKD for its words; a name goes back to the usual form.
    return name.trimmed().normalized(QString::NormalizationForm_C);
}

} // namespace

void SeedLine::wipe()
{
    scrub(words);
}

std::vector<SeedLine> parseSeedLines(const std::string& text)
{
    std::vector<SeedLine> out;
    int number = 0;
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = text.size();
        ++number;

        size_t begin = lineStart;
        size_t end = lineEnd;
        const size_t next = lineEnd + 1;
        while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'))
            ++begin;
        while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
            --end;
        lineStart = next;
        if (begin == end || text[begin] == '#')
            continue;
        begin = afterListNumber(text, begin, end);

        SeedLine line;
        line.number = number;
        // A name is what stands before the first separator, when a whole phrase
        // follows it. Otherwise the separators only part the words.
        size_t nameEnd = begin;
        while (nameEnd < end && !isNameEnd(text[nameEnd]))
            ++nameEnd;
        size_t reportedCount = 0;
        if (nameEnd < end) {
            line.words = wordsIn(text, nameEnd + 1, end);
            reportedCount = line.words.size();
            if (isValidWordCount(reportedCount)) {
                line.name = trimmedName(text, begin, nameEnd);
            } else {
                scrub(line.words);
                line.words = wordsIn(text, begin, end);
                if (isValidWordCount(line.words.size()))
                    reportedCount = line.words.size();
            }
        } else {
            line.words = wordsIn(text, begin, end);
            reportedCount = line.words.size();
        }

        line.problem = problemWith(line.words, reportedCount);
        if (line.problem.isEmpty() && looksLikeSeedWords(line.name)) {
            line.problem = QStringLiteral("its name looks like seed words");
            line.name.clear();
        }
        if (!line.problem.isEmpty())
            line.wipe();
        out.push_back(std::move(line));
    }
    return out;
}

QList<KeyLine> parseKeyLines(const QString& text)
{
    // Any kind of space: a key copied from a document or a page can come with one.
    static const QRegularExpression gap(QStringLiteral("[\\s,;:\"]+"), QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression listNumber(QStringLiteral("^\\d+[.)]\\s+"),
                                               QRegularExpression::UseUnicodePropertiesOption);
    QList<KeyLine> out;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        line.remove(listNumber);

        KeyLine entry;
        entry.number = int(i + 1);
        qsizetype firstKey = -1;
        qsizetype searched = 0;
        const QStringList tokens = line.split(gap, Qt::SkipEmptyParts);
        for (const QString& token : tokens) {
            const qsizetype at = line.indexOf(token, searched);
            searched = at + token.size();
            if (token.startsWith(QLatin1String("zpub")) || token.startsWith(QLatin1String("ypub"))
                || token.startsWith(QLatin1String("xpub"))) {
                if (firstKey < 0)
                    firstKey = at;
                entry.keys << token;
            }
        }
        if (entry.keys.isEmpty()) {
            entry.problem = QStringLiteral("it has no zpub, ypub or xpub");
        } else {
            QString name = line.left(firstKey);
            static const QRegularExpression edges(QStringLiteral("^[\\s,;:\"]+|[\\s,;:\"]+$"),
                                                  QRegularExpression::UseUnicodePropertiesOption);
            name.remove(edges);
            entry.name = name;
        }
        out.append(entry);
    }
    return out;
}

bool looksLikeSeedWords(const QString& line)
{
    static const QRegularExpression gap(QStringLiteral("[\\s,;:\"]+"), QRegularExpression::UseUnicodePropertiesOption);
    const int needed = line.size() >= WalletStore::kMaxLabelChars ? 5 : 12;
    int words = 0;
    const QStringList tokens = line.split(gap, Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        if (chain::bip39::isWord(token.toLower().toStdString()) && ++words >= needed)
            return true;
    }
    return false;
}

} // namespace wallet
