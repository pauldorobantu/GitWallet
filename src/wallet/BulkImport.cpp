#include "wallet/BulkImport.h"

#include "chain/Bip39.h"
#include "chain/Hash.h"
#include "wallet/WalletStore.h"

#include <QByteArray>
#include <QRegularExpression>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace wallet {

namespace {

// A phrase has at most 24 words: past one more, only the count matters. Reserved up
// front: a word's buffer sits inside the vector's, so a vector that grew would leave
// copies of the words behind in the memory it freed.
constexpr size_t kMaxKeptWords = 25;

constexpr const char* kNameIsSecret = "its name looks like seed words or a private key";

// A ':' or a tab is meant as the end of a name; a ',' or ';' may only part words.
bool isFirmEnd(char c)
{
    return c == ':' || c == '\t';
}

bool isNameEnd(char c)
{
    return isFirmEnd(c) || c == ';' || c == ',';
}

bool isGap(char c)
{
    return c == ' ' || c == '"' || c == '\r' || isNameEnd(c);
}

bool isValidWordCount(size_t count)
{
    return count == 12 || count == 15 || count == 18 || count == 21 || count == 24;
}

// The words of one reading of a line, scrubbed however that reading ends.
struct Words {
    std::vector<std::string> list;
    size_t count = 0; // how many the text holds, more than are kept on a long line

    Words() = default;
    Words(const Words&) = delete;
    Words& operator=(const Words&) = delete;
    ~Words() { scrub(); }

    void scrub()
    {
        for (std::string& word : list)
            chain::secureClear(word);
        list.clear();
    }
};

void toLowerAscii(std::string& word)
{
    for (char& c : word) {
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    }
}

// As BIP39 reads a word: lowercase, and in NFKD for what is not plain ASCII (full-width
// letters, a ligature copied from a PDF). Every copy is scrubbed.
void readWord(std::string& word)
{
    toLowerAscii(word);
    if (std::all_of(word.cbegin(), word.cend(), [](char c) { return static_cast<unsigned char>(c) < 0x80; }))
        return;
    QString typed = QString::fromUtf8(word.data(), qsizetype(word.size()));
    QString nfkd = typed.normalized(QString::NormalizationForm_KD);
    QByteArray bytes = nfkd.toUtf8();
    chain::secureClear(word);
    word.assign(bytes.constData(), size_t(bytes.size()));
    toLowerAscii(word);
    bytes.fill('\0');
    typed.fill(QChar(u'\0'));
    nfkd.fill(QChar(u'\0'));
}

// The words of [from, to).
void readWords(const std::string& text, size_t from, size_t to, Words* out)
{
    out->list.reserve(kMaxKeptWords);
    size_t i = from;
    while (i < to) {
        while (i < to && isGap(text[i]))
            ++i;
        size_t end = i;
        while (end < to && !isGap(text[end]))
            ++end;
        if (end > i) {
            if (out->list.size() < kMaxKeptWords) {
                // Built in place: a local string moved in would leave its letters on the stack.
                readWord(out->list.emplace_back(text, i, end - i));
            }
            ++out->count;
        }
        i = end;
    }
}

// Why these words are no phrase, by count or position, never by text. Empty if they are one.
QString problemWith(const Words& words, bool afterName)
{
    if (!isValidWordCount(words.count)) {
        return (afterName ? QStringLiteral("it needs 12, 15, 18, 21 or 24 words after the name (it has %1)")
                          : QStringLiteral("it needs 12, 15, 18, 21 or 24 words (it has %1)"))
            .arg(words.count);
    }
    for (size_t i = 0; i < words.list.size(); ++i) {
        if (!chain::bip39::isWord(words.list[i])) {
            return (afterName ? QStringLiteral("word %1 after the name is not in the BIP39 list")
                              : QStringLiteral("word %1 is not in the BIP39 list"))
                .arg(i + 1);
        }
    }
    // `why` can quote a word, so it goes unread.
    std::string why;
    const bool valid = chain::bip39::validate(words.list, &why);
    chain::secureClear(why);
    return valid ? QString() : QStringLiteral("the checksum does not match: a word is wrong or out of order");
}

bool allInList(const Words& words)
{
    return words.count > 0 && words.count == words.list.size()
           && std::all_of(words.list.cbegin(), words.list.cend(),
                          [](const std::string& word) { return chain::bip39::isWord(word); });
}

// "12. " and "12)\t" at the start of a line: a numbered list, not a name.
size_t afterListNumber(const std::string& text, size_t begin, size_t end)
{
    size_t p = begin;
    while (p < end && text[p] >= '0' && text[p] <= '9')
        ++p;
    if (p == begin || p + 1 >= end || (text[p] != '.' && text[p] != ')') || (text[p + 1] != ' ' && text[p + 1] != '\t'))
        return begin;
    p += 2;
    while (p < end && (text[p] == ' ' || text[p] == '\t'))
        ++p;
    return p;
}

// Where a line parts in two: the field before the mark and what follows it.
struct Parts {
    bool found = false;
    bool firm = false; // quotes, a ':' or a tab: meant as a field of its own
    size_t firstEnd = 0;
    size_t secondBegin = 0;
};

Parts partsOf(const std::string& text, size_t begin, size_t end)
{
    Parts parts;
    const auto partAt = [&parts](size_t mark, bool firm) {
        parts.found = true;
        parts.firm = firm;
        parts.firstEnd = mark;
        parts.secondBegin = mark + 1;
    };
    if (text[begin] == '"') {
        // A first field in quotes holds whatever is inside them; "" is a quote.
        size_t close = begin + 1;
        while (close < end) {
            if (text[close] == '"') {
                if (close + 1 >= end || text[close + 1] != '"')
                    break;
                ++close;
            }
            ++close;
        }
        size_t after = close + 1;
        while (after < end && text[after] == ' ')
            ++after;
        if (close < end && after < end && isNameEnd(text[after])) {
            partAt(after, true);
            return parts;
        }
    }
    const auto first = [&](auto pred) {
        const auto it = std::find_if(text.cbegin() + std::ptrdiff_t(begin), text.cbegin() + std::ptrdiff_t(end), pred);
        return size_t(it - text.cbegin());
    };
    size_t mark = first([](char c) { return c == ':'; });
    if (mark < end) {
        partAt(mark, true);
        return parts;
    }
    mark = first([](char c) { return c == '\t'; });
    if (mark < end) {
        partAt(mark, true);
        return parts;
    }
    mark = first([](char c) { return isNameEnd(c); });
    if (mark < end)
        partAt(mark, false);
    return parts;
}

// A mark between every two words is how some lists write one phrase, not a name.
bool marksPartEveryWord(const std::string& text, size_t begin, size_t end, const Words& whole)
{
    if (whole.count < 2 || whole.list.empty() || !chain::bip39::isWord(whole.list.front()))
        return false;
    const auto marks = std::count_if(text.cbegin() + std::ptrdiff_t(begin), text.cbegin() + std::ptrdiff_t(end), isNameEnd);
    return size_t(marks) + 1 >= whole.count;
}

// As typed, without one pair of outer quotes and with the CSV form of a quote undone.
// Worked on in place: if this turns out to be seed words, only one buffer holds them.
QString nameIn(const std::string& text, size_t from, size_t to)
{
    QString name = QString::fromUtf8(text.data() + from, qsizetype(to - from));
    name = std::move(name).trimmed();
    if (name.size() >= 2 && name.startsWith(QLatin1Char('"')) && name.endsWith(QLatin1Char('"'))) {
        name.chop(1);
        name.remove(0, 1);
        name.replace(QStringLiteral("\"\""), QStringLiteral("\""));
        name = std::move(name).trimmed();
    }
    return name.normalized(QString::NormalizationForm_C);
}

void takeName(SeedLine* line, QString name)
{
    if (looksLikeSecret(name, WalletStore::kMaxLabelChars)) {
        name.fill(QChar(u'\0'));
        line->problem = QString::fromLatin1(kNameIsSecret);
        return;
    }
    line->name = std::move(name);
}

// Reads [begin, end) as a phrase, with a name if there is one: every way the line can
// be read is judged in full. Any problem leaves the words for the caller to wipe.
void readLine(const std::string& text, size_t begin, size_t end, SeedLine* line)
{
    Words whole;
    readWords(text, begin, end, &whole);
    const QString wholeProblem = problemWith(whole, false);
    const Parts parts = partsOf(text, begin, end);
    if (!parts.found || marksPartEveryWord(text, begin, end, whole)) {
        line->problem = wholeProblem;
        if (wholeProblem.isEmpty())
            line->words.swap(whole.list);
        return;
    }

    Words first;
    Words second;
    readWords(text, begin, parts.firstEnd, &first);
    readWords(text, parts.secondBegin, end, &second);
    const QString firstProblem = problemWith(first, false);
    const QString secondProblem = problemWith(second, true);
    static const QString twoWays = QStringLiteral(
        "it can be read as one phrase, or as a name and a shorter phrase: join the words or change the name");
    static const QString wordsAsName = QStringLiteral(
        "what stands beside the phrase could be part of it, as all of it is in the BIP39 list: change the name");

    if (wholeProblem.isEmpty() && secondProblem.isEmpty()) {
        line->problem = twoWays;
    } else if (wholeProblem.isEmpty()) {
        // The whole line is a phrase. A ':' in the middle of one is not a slip, it is
        // a name with a word missing after it.
        if (parts.firm && second.count > 0)
            line->problem = secondProblem;
        else
            line->words.swap(whole.list);
    } else if (secondProblem.isEmpty()) {
        if (allInList(first) && isValidWordCount(first.count + second.count)) {
            line->problem = wordsAsName;
        } else {
            takeName(line, nameIn(text, begin, parts.firstEnd));
            line->words.swap(second.list);
        }
    } else if (firstProblem.isEmpty()) {
        if (allInList(second) && isValidWordCount(first.count + second.count)) {
            line->problem = wordsAsName;
        } else {
            takeName(line, nameIn(text, parts.secondBegin, end));
            line->words.swap(first.list);
        }
    } else {
        // No phrase either way: said of the line the person most likely meant.
        line->problem = parts.firm || !allInList(first) ? secondProblem : wholeProblem;
    }
}

} // namespace

void SeedLine::wipe()
{
    for (std::string& word : words)
        chain::secureClear(word);
    words.clear();
    name.fill(QChar(u'\0'));
    name.clear();
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
        if (begin == end)
            continue;

        SeedLine line;
        line.number = number;
        if (text[begin] == '#') {
            // A comment, unless a phrase follows: then a seed would be left out unseen.
            size_t after = begin + 1;
            while (after < end && (text[after] == ' ' || text[after] == '\t'))
                ++after;
            if (after == end)
                continue;
            SeedLine inside;
            readLine(text, afterListNumber(text, after, end), end, &inside);
            if (!inside.problem.isEmpty())
                continue;
            line.problem = QStringLiteral("it starts with #, so it was skipped: remove the # to add it");
        } else {
            readLine(text, afterListNumber(text, begin, end), end, &line);
            if (!line.problem.isEmpty())
                line.wipe();
        }
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
            if (looksLikeSecret(name, WalletStore::kMaxLabelChars))
                entry.problem = QString::fromLatin1(kNameIsSecret);
            else
                entry.name = name;
        }
        out.append(entry);
    }
    return out;
}

bool looksLikeSeedWords(QStringView text, int cutAt)
{
    const int needed = cutAt > 0 && text.size() >= cutAt ? 5 : 12;
    // Read the way the seed fields read it. normalized() can hand back the very buffer
    // of `copy`; filling both zeroes it whichever owns it by then.
    QString copy = text.toString();
    QString nfkd = copy.normalized(QString::NormalizationForm_KD);
    std::string word; // a list word fits the string itself, so no allocation holds it
    bool plain = true; // ASCII letters only, and short enough to be a list word
    int run = 0;
    bool found = false;
    const auto endWord = [&] {
        if (!word.empty() || !plain) {
            if (plain && chain::bip39::isWord(word)) {
                if (++run >= needed)
                    found = true;
            } else {
                run = 0;
            }
        }
        word.clear();
        plain = true;
    };
    for (const QChar c : std::as_const(nfkd)) {
        if (c.category() == QChar::Other_Format)
            continue; // never shows as a gap, so a word on screen stays one word
        if (!c.isLetter()) {
            endWord();
            continue;
        }
        const char16_t u = c.unicode();
        if (u < 0x80 && word.size() < 15)
            word.push_back(char(u >= 'A' && u <= 'Z' ? u - 'A' + 'a' : u));
        else
            plain = false;
    }
    endWord();
    chain::secureClear(word);
    copy.fill(QChar(u'\0'));
    nfkd.fill(QChar(u'\0'));
    return found;
}

bool looksLikePrivateKey(QStringView text)
{
    const auto isAlnum = [](QChar c) {
        return (c >= u'0' && c <= u'9') || (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z');
    };
    const auto isBase58 = [](QChar c) {
        return ((c >= u'1' && c <= u'9') || (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z')) && c != u'I'
               && c != u'O' && c != u'l';
    };
    const auto isHex = [](QChar c) {
        return (c >= u'0' && c <= u'9') || (c >= u'A' && c <= u'F') || (c >= u'a' && c <= u'f');
    };
    qsizetype i = 0;
    while (i < text.size()) {
        while (i < text.size() && !isAlnum(text[i]))
            ++i;
        qsizetype end = i;
        while (end < text.size() && isAlnum(text[end]))
            ++end;
        QStringView token = text.mid(i, end - i);
        i = end;

        // The prefixes of extended private keys, at any length a field may have cut them to.
        static const char16_t* const kPrefixes[] = {u"xprv", u"yprv", u"zprv", u"Yprv", u"Zprv",
                                                    u"tprv", u"uprv", u"vprv", u"Uprv", u"Vprv"};
        for (const char16_t* prefix : kPrefixes) {
            if (token.size() >= 44 && token.startsWith(QStringView(prefix)))
                return true;
        }
        // A key in Wallet Import Format.
        const bool wifSize = (token.size() == 51 && token.front() == u'5')
                             || (token.size() == 52 && (token.front() == u'K' || token.front() == u'L'));
        if (wifSize && std::all_of(token.cbegin(), token.cend(), isBase58))
            return true;
        // A raw key in hex, cut or not.
        if (token.startsWith(u"0x") || token.startsWith(u"0X"))
            token = token.mid(2);
        if (token.size() >= 60 && std::all_of(token.cbegin(), token.cend(), isHex))
            return true;
    }
    return false;
}

bool looksLikeSecret(QStringView text, int cutAt)
{
    return looksLikeSeedWords(text, cutAt) || looksLikePrivateKey(text);
}

bool keyListHoldsSecret(const QString& text)
{
    const QList<QStringView> lines = QStringView(text).split(u'\n');
    return std::any_of(lines.cbegin(), lines.cend(), [](QStringView line) { return looksLikeSecret(line); });
}

} // namespace wallet
