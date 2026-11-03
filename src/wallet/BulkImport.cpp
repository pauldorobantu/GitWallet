#include "wallet/BulkImport.h"

#include "chain/Bip39.h"
#include "chain/Hash.h"

#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace wallet {

namespace {

// A phrase has at most 24 words: past one more, only the count matters. Reserved up
// front: a word's buffer sits inside the vector's, so a vector that grew would leave
// copies of the words behind in the memory it freed.
constexpr size_t kMaxKeptWords = 25;
// A line with more marks than this holds no name: tried at each one, it would take long.
constexpr size_t kMaxMarksTried = 64;

constexpr const char* kNameIsSecret = "its name looks like seed words or a private key";
constexpr const char* kCommentSkipped = "it starts with #, so it was skipped: remove the # to add it";

// A ':' or a tab is meant as the end of a field; a ',' or ';' may be a slip inside a phrase.
bool isFirmMark(char c)
{
    return c == ':' || c == '\t';
}

bool isMark(char c)
{
    return isFirmMark(c) || c == ';' || c == ',';
}

bool isSpace(char c)
{
    return c == ' ' || c == '\r';
}

bool isGap(char c)
{
    return isSpace(c) || c == '"' || isMark(c);
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

// Adds the words of [from, to).
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

// Why these words are no phrase, by count or position, never by text. Empty if they are
// one. `beside`: they stand beside what reads as a name.
QString problemWith(const Words& words, bool beside)
{
    if (!isValidWordCount(words.count)) {
        return (beside ? QStringLiteral("it needs 12, 15, 18, 21 or 24 words beside the name (it has %1)")
                       : QStringLiteral("it needs 12, 15, 18, 21 or 24 words (it has %1)"))
            .arg(words.count);
    }
    for (size_t i = 0; i < words.list.size(); ++i) {
        if (!chain::bip39::isWord(words.list[i])) {
            return (beside ? QStringLiteral("word %1 of the phrase is not a seed word")
                           : QStringLiteral("word %1 is not a seed word"))
                .arg(i + 1);
        }
    }
    // `why` can quote a word, so it goes unread.
    std::string why;
    const bool valid = chain::bip39::validate(words.list, &why);
    chain::secureClear(why);
    return valid ? QString() : QStringLiteral("these words do not make a valid seed: a word is wrong or out of order");
}

size_t listWordsIn(const Words& words)
{
    return size_t(std::count_if(words.list.cbegin(), words.list.cend(),
                                [](const std::string& word) { return chain::bip39::isWord(word); }));
}

bool allInList(const Words& words)
{
    return words.count > 0 && words.count == words.list.size() && listWordsIn(words) == words.count;
}

// One letters-only word off the list and the rest on it: a phrase with a word mistyped.
bool oneSlipFromList(const Words& words)
{
    if (words.count != words.list.size() || listWordsIn(words) + 1 != words.count)
        return false;
    return std::all_of(words.list.cbegin(), words.list.cend(), [](const std::string& word) {
        return std::all_of(word.cbegin(), word.cend(), [](char c) { return c >= 'a' && c <= 'z'; });
    });
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

// [from, to) is one field in quotes, with "" for a quote inside.
bool isOneQuotedField(const std::string& text, size_t from, size_t to)
{
    if (to < from + 2 || text[from] != '"' || text[to - 1] != '"')
        return false;
    for (size_t i = from + 1; i + 1 < to; ++i) {
        if (text[i] != '"')
            continue;
        if (i + 2 >= to || text[i + 1] != '"')
            return false;
        ++i;
    }
    return true;
}

// Spaces, blank columns at either end and quotes around the whole line say nothing.
void trimFields(const std::string& text, size_t* begin, size_t* end)
{
    for (;;) {
        size_t b = *begin;
        size_t e = *end;
        while (b < e && (isSpace(text[b]) || isMark(text[b])))
            ++b;
        while (e > b && (isSpace(text[e - 1]) || isMark(text[e - 1])))
            --e;
        if (e - b >= 2 && text[b] == '"') { // a blank quoted column first
            size_t close = b + 1;
            while (close < e && text[close] == ' ')
                ++close;
            if (close < e && text[close] == '"' && (close + 1 == e || isSpace(text[close + 1]) || isMark(text[close + 1])))
                b = close + 1;
        }
        if (e - b >= 2 && text[e - 1] == '"') { // a blank quoted column last
            size_t open = e - 1;
            while (open > b && text[open - 1] == ' ')
                --open;
            if (open > b && text[open - 1] == '"'
                && (open - 1 == b || isSpace(text[open - 2]) || isMark(text[open - 2])))
                e = open - 1;
        }
        if (e > b && isOneQuotedField(text, b, e)) {
            ++b;
            --e;
        }
        if (b == *begin && e == *end)
            return;
        *begin = b;
        *end = e;
    }
}

// The marks of a line, outside quotes. A quote with no partner is only a character.
std::vector<size_t> marksOf(const std::string& text, size_t begin, size_t end)
{
    const auto first = text.cbegin() + std::ptrdiff_t(begin);
    const auto last = text.cbegin() + std::ptrdiff_t(end);
    const bool quotesPair = std::count(first, last, '"') % 2 == 0;
    std::vector<size_t> marks;
    bool inQuotes = false;
    for (size_t i = begin; i < end; ++i) {
        if (text[i] == '"' && quotesPair)
            inQuotes = !inQuotes;
        else if (!inQuotes && isMark(text[i]))
            marks.push_back(i);
    }
    return marks;
}

// A mark between every two words is how some lists write one phrase, not a name.
bool marksPartEveryWord(const std::vector<size_t>& marks, const Words& whole)
{
    return whole.count >= 2 && !whole.list.empty() && chain::bip39::isWord(whole.list.front())
           && marks.size() + 1 >= whole.count;
}

// A name as typed: without what parts it from the rest, including the closing quote of a
// key's own field, and with one field in quotes read as CSV. Worked on in place: if this
// turns out to be seed words, only one buffer holds them.
QString fieldName(QString field)
{
    static const QRegularExpression leading(QStringLiteral("^[\\s,;:]*(\"\\s*[,;:]+)?[\\s,;:]*"),
                                            QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression trailing(QStringLiteral("[\\s,;:]*([,;:]+\\s*\")?[\\s,;:]*$"),
                                             QRegularExpression::UseUnicodePropertiesOption);
    field.remove(leading);
    field.remove(trailing);
    if (field == QLatin1String("\""))
        return QString(); // only the opening quote of a key's own field
    bool quoted = field.size() >= 2 && field.startsWith(QLatin1Char('"')) && field.endsWith(QLatin1Char('"'));
    for (qsizetype i = 1; quoted && i + 1 < field.size(); ++i) {
        if (field.at(i) != QLatin1Char('"'))
            continue;
        if (i + 2 >= field.size() || field.at(i + 1) != QLatin1Char('"'))
            quoted = false;
        ++i;
    }
    if (quoted) {
        field.chop(1);
        field.remove(0, 1);
        field.replace(QStringLiteral("\"\""), QStringLiteral("\""));
    }
    field = std::move(field).trimmed();
    return field.normalized(QString::NormalizationForm_C);
}

// One way a line can be meant.
struct Reading {
    size_t phraseFrom = 0;
    size_t phraseTo = 0;
    size_t nameFrom = 0;
    size_t nameTo = 0;
    bool firm = false; // the name is meant as a field: a firm mark or quotes
};

void addReading(const std::string& text, Reading reading, std::vector<Reading>* readings)
{
    const auto bounds = [&text](size_t* from, size_t* to) {
        while (*from < *to && isGap(text[*from]))
            ++*from;
        while (*to > *from && isGap(text[*to - 1]))
            --*to;
    };
    bounds(&reading.phraseFrom, &reading.phraseTo);
    const bool sameWords = std::any_of(readings->cbegin(), readings->cend(), [&reading](const Reading& other) {
        return other.phraseFrom == reading.phraseFrom && other.phraseTo == reading.phraseTo;
    });
    if (!sameWords)
        readings->push_back(reading);
}

// Reads [begin, end) as a phrase, with a name if there is one. Any problem leaves what
// was read for the caller to wipe. `plain`: the line has no mark at all.
void readLine(const std::string& text, size_t begin, size_t end, SeedLine* line, bool* plain)
{
    Words whole;
    readWords(text, begin, end, &whole);
    const QString wholeProblem = problemWith(whole, false);
    const std::vector<size_t> marks = marksOf(text, begin, end);
    *plain = marks.empty();
    if (marks.empty() || marks.size() > kMaxMarksTried || marksPartEveryWord(marks, whole)) {
        line->problem = wholeProblem;
        if (wholeProblem.isEmpty())
            line->words.swap(whole.list);
        return;
    }

    std::vector<Reading> readings;
    for (const size_t mark : marks) {
        Words before;
        Words after;
        readWords(text, begin, mark, &before);
        readWords(text, mark + 1, end, &after);
        const bool firmMark = isFirmMark(text[mark]);
        if (problemWith(after, false).isEmpty()) {
            Reading reading{mark + 1, end, begin, mark, firmMark};
            size_t b = begin;
            size_t e = mark;
            while (b < e && isSpace(text[b]))
                ++b;
            while (e > b && isSpace(text[e - 1]))
                --e;
            reading.firm = reading.firm || isOneQuotedField(text, b, e);
            addReading(text, reading, &readings);
        }
        if (problemWith(before, false).isEmpty()) {
            Reading reading{begin, mark, mark + 1, end, firmMark};
            size_t b = mark + 1;
            size_t e = end;
            while (b < e && isSpace(text[b]))
                ++b;
            while (e > b && isSpace(text[e - 1]))
                --e;
            reading.firm = reading.firm || isOneQuotedField(text, b, e);
            addReading(text, reading, &readings);
        }
    }

    const bool wholeIsPhrase = wholeProblem.isEmpty();
    if (readings.size() > 1 || (wholeIsPhrase && !readings.empty())) {
        line->problem = QStringLiteral("it can be read two ways, as one phrase or as a name and a phrase: remove the "
                                       "mark that does not belong, or change the name");
        return;
    }
    if (readings.empty()) {
        // No phrase beside any mark: said of the line the person most likely meant.
        const auto firm = std::find_if(marks.cbegin(), marks.cend(), [&text](size_t mark) { return isFirmMark(text[mark]); });
        const size_t mark = firm != marks.cend() ? *firm : marks.front();
        Words before;
        Words after;
        readWords(text, begin, mark, &before);
        readWords(text, mark + 1, end, &after);
        const bool afterIsPhrase = listWordsIn(after) >= listWordsIn(before);
        const Words& phrase = afterIsPhrase ? after : before;
        const Words& other = afterIsPhrase ? before : after;
        if (wholeIsPhrase && !isFirmMark(text[mark]))
            line->problem = QStringLiteral("a ',' or ';' stands inside the phrase: remove it if this is one phrase");
        else if (isFirmMark(text[mark]) || !allInList(other))
            line->problem = problemWith(phrase, true);
        else
            line->problem = wholeProblem;
        return;
    }

    const Reading& reading = readings.front();
    Words name;
    Words phrase;
    readWords(text, reading.nameFrom, reading.nameTo, &name);
    readWords(text, reading.phraseFrom, reading.phraseTo, &phrase);
    // A comma can be a slip inside a longer phrase, and a checksum proves little.
    const bool totalIsPhraseSized = isValidWordCount(name.count + phrase.count);
    if ((allInList(name) && (totalIsPhraseSized || !reading.firm))
        || (!reading.firm && totalIsPhraseSized && oneSlipFromList(name))) {
        line->problem = QStringLiteral("its name could be part of the phrase: change it, for example by adding a number, "
                                       "or write it before a ':'");
        return;
    }
    if (name.count > 0 || reading.nameTo > reading.nameFrom) {
        QString typed = fieldName(QString::fromUtf8(text.data() + reading.nameFrom, qsizetype(reading.nameTo - reading.nameFrom)));
        if (looksLikeSecret(typed)) {
            typed.fill(QChar(u'\0'));
            line->problem = QString::fromLatin1(kNameIsSecret);
            return;
        }
        line->name = std::move(typed);
    }
    line->words.swap(phrase.list);
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
    struct Span {
        size_t begin = 0;
        size_t end = 0;
        bool plain = false;
    };
    std::vector<SeedLine> out;
    std::vector<Span> spans;
    int number = 0;
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = text.size();
        ++number;

        size_t begin = lineStart;
        size_t end = lineEnd;
        lineStart = lineEnd + 1;
        while (begin < end && (isSpace(text[begin]) || text[begin] == '\t'))
            ++begin;
        while (end > begin && (isSpace(text[end - 1]) || text[end - 1] == '\t'))
            --end;
        if (begin == end)
            continue;

        SeedLine line;
        line.number = number;
        Span span;
        if (text[begin] == '#') {
            // A comment, unless a phrase follows: then a seed would be left out unseen.
            size_t after = begin + 1;
            while (after < end && (text[after] == ' ' || text[after] == '\t'))
                ++after;
            after = afterListNumber(text, after, end);
            trimFields(text, &after, &end);
            if (after >= end)
                continue;
            SeedLine inside;
            bool plain = false;
            readLine(text, after, end, &inside, &plain);
            if (!inside.problem.isEmpty())
                continue;
            line.problem = QString::fromLatin1(kCommentSkipped);
        } else {
            begin = afterListNumber(text, begin, end);
            trimFields(text, &begin, &end);
            if (begin >= end)
                continue; // a row of blank columns
            readLine(text, begin, end, &line, &span.plain);
            span.begin = begin;
            span.end = end;
            if (!line.problem.isEmpty())
                line.wipe();
        }
        out.push_back(std::move(line));
        spans.push_back(span);
    }

    // One long phrase wrapped over two lines: half of it alone can pass its checksum.
    for (size_t i = 0; i + 1 < out.size(); ++i) {
        if (out[i].number + 1 != out[i + 1].number || !spans[i].plain || !spans[i + 1].plain)
            continue;
        if (!out[i].problem.isEmpty() && !out[i + 1].problem.isEmpty())
            continue; // neither goes in anyway
        Words joined;
        readWords(text, spans[i].begin, spans[i].end, &joined);
        readWords(text, spans[i + 1].begin, spans[i + 1].end, &joined);
        if (!problemWith(joined, false).isEmpty())
            continue;
        const QString why = QStringLiteral("this line and the one beside it also read as one phrase: join them if they "
                                           "are one, or add them one at a time if they are two");
        for (const size_t k : {i, i + 1}) {
            out[k].wipe();
            out[k].problem = why;
        }
    }
    return out;
}

QList<KeyLine> parseKeyLines(const QString& text)
{
    // Any kind of space: a key copied from a document or a page can come with one.
    static const QRegularExpression gap(QStringLiteral("[\\s,;:\"=\\[\\]]+"), QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression listNumber(QStringLiteral("^\\d+[.)]\\s+"), QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression origin(QStringLiteral("\\[[0-9a-fA-F]{8}[^\\]]*\\]")); // a key's origin in a descriptor
    QList<KeyLine> out;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); ++i) {
        QString line;
        line.reserve(lines.at(i).size());
        for (const QChar c : lines.at(i)) {
            if (c.category() != QChar::Other_Format)
                line.append(c);
        }
        line = std::move(line).trimmed();
        if (line.isEmpty())
            continue;
        const bool comment = line.startsWith(QLatin1Char('#'));
        if (comment)
            line = std::move(line).mid(1).trimmed();
        line.remove(origin);
        line.remove(listNumber);

        KeyLine entry;
        entry.number = int(i + 1);
        qsizetype firstKey = -1;
        qsizetype lastKeyEnd = 0;
        qsizetype searched = 0;
        const QStringList tokens = line.split(gap, Qt::SkipEmptyParts);
        for (const QString& token : tokens) {
            const qsizetype at = line.indexOf(token, searched);
            searched = at + token.size();
            if (token.startsWith(QLatin1String("zpub")) || token.startsWith(QLatin1String("ypub"))
                || token.startsWith(QLatin1String("xpub"))) {
                if (firstKey < 0)
                    firstKey = at;
                lastKeyEnd = searched;
                entry.keys << token;
            }
        }
        if (comment) {
            if (entry.keys.isEmpty())
                continue;
            entry.keys.clear();
            entry.problem = QString::fromLatin1(kCommentSkipped);
        } else if (entry.keys.isEmpty()) {
            entry.problem = QStringLiteral("no zpub, ypub or xpub could be read on it");
        } else {
            QString name = fieldName(line.left(firstKey));
            if (name.isEmpty())
                name = fieldName(line.mid(lastKeyEnd));
            if (looksLikeSecret(name))
                entry.problem = QString::fromLatin1(kNameIsSecret);
            else
                entry.name = name;
        }
        out.append(entry);
    }
    return out;
}

bool looksLikeSeedWords(QStringView text)
{
    // Read the way the seed fields read it. normalized() can hand back the very buffer
    // of `copy`; filling both zeroes it whichever owns it by then.
    QString copy = text.toString();
    QString nfkd = copy.normalized(QString::NormalizationForm_KD);
    constexpr int kWindow = 12;
    constexpr int kInList = 10;
    std::array<bool, kWindow> window{};
    int position = 0;
    int seen = 0;
    int inList = 0;
    std::string word; // a list word fits the string itself, so no allocation holds it
    bool plain = true; // ASCII letters only, and short enough to be a list word
    bool found = false;
    const auto endWord = [&] {
        if (word.empty() && plain)
            return;
        const bool hit = plain && chain::bip39::isWord(word);
        if (seen == kWindow)
            inList -= window[size_t(position)] ? 1 : 0;
        else
            ++seen;
        window[size_t(position)] = hit;
        inList += hit ? 1 : 0;
        position = (position + 1) % kWindow;
        found = found || (seen == kWindow && inList >= kInList);
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
    // As the seed fields read text, and as cleanLabel would store it. Same care as above.
    QString copy = text.toString();
    QString nfkd = copy.normalized(QString::NormalizationForm_KD);
    QString clean;
    clean.reserve(nfkd.size());
    for (const QChar c : std::as_const(nfkd)) {
        if (c.category() != QChar::Other_Format)
            clean.append(c);
    }

    // A raw key is 64 hex digits, cut or not, and written in groups or not. 66 or 130 are
    // public keys, which are no secret.
    constexpr qsizetype kFewestHexDigits = 56;
    constexpr qsizetype kMostHexDigits = 64;
    constexpr qsizetype kShortestGroup = 4;
    qsizetype hexRun = 0;
    bool found = false;
    const auto endHexRun = [&] {
        found = found || (hexRun >= kFewestHexDigits && hexRun <= kMostHexDigits);
        hexRun = 0;
    };
    const QStringView view(clean);
    qsizetype i = 0;
    while (i < view.size() && !found) {
        while (i < view.size() && !isAlnum(view[i]))
            ++i;
        qsizetype end = i;
        while (end < view.size() && isAlnum(view[end]))
            ++end;
        QStringView token = view.mid(i, end - i);
        i = end;
        if (token.isEmpty())
            break;

        // The prefixes of extended private keys, at any length a field may have cut them to.
        static const char16_t* const kPrefixes[] = {u"xprv", u"yprv", u"zprv", u"Yprv", u"Zprv",
                                                    u"tprv", u"uprv", u"vprv", u"Uprv", u"Vprv"};
        for (const char16_t* prefix : kPrefixes)
            found = found || (token.size() >= 44 && token.startsWith(QStringView(prefix)));
        // A key in Wallet Import Format.
        const bool wifSize = (token.size() == 51 && token.front() == u'5')
                             || (token.size() == 52 && (token.front() == u'K' || token.front() == u'L'));
        found = found || (wifSize && std::all_of(token.cbegin(), token.cend(), isBase58));

        if (token.startsWith(u"0x") || token.startsWith(u"0X"))
            token = token.mid(2);
        if (token.size() >= kShortestGroup && std::all_of(token.cbegin(), token.cend(), isHex)) {
            found = found || (token.size() >= kFewestHexDigits && token.size() <= kMostHexDigits);
            hexRun += token.size();
        } else {
            endHexRun();
        }
    }
    endHexRun();
    copy.fill(QChar(u'\0'));
    nfkd.fill(QChar(u'\0'));
    clean.fill(QChar(u'\0'));
    return found;
}

bool looksLikeSecret(QStringView text)
{
    return looksLikeSeedWords(text) || looksLikePrivateKey(text);
}

QList<int> linesHoldingSecrets(const QString& text)
{
    QList<int> found;
    const QList<QStringView> lines = QStringView(text).split(u'\n');
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (looksLikeSecret(lines.at(i)))
            found.append(int(i));
    }
    return found;
}

bool decodeListFile(const QByteArray& bytes, QString* out)
{
    const auto decodeAs = [&bytes, out](QStringConverter::Encoding encoding) {
        QStringDecoder decoder(encoding, QStringDecoder::Flag::Stateless);
        QString text = decoder.decode(bytes);
        if (decoder.hasError() || text.contains(QChar(u'\0'))) {
            text.fill(QChar(u'\0'));
            return false;
        }
        *out = std::move(text);
        return true;
    };
    if (const std::optional<QStringConverter::Encoding> marked = QStringConverter::encodingForData(bytes))
        return decodeAs(*marked);

    // UTF-16 with no mark: every other byte is zero.
    qsizetype zerosEven = 0;
    qsizetype zerosOdd = 0;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        if (bytes.at(i) == '\0')
            ++(i % 2 == 0 ? zerosEven : zerosOdd);
    }
    const qsizetype mostOfHalf = bytes.size() / 2 * 8 / 10;
    if (mostOfHalf > 0 && zerosEven == 0 && zerosOdd >= mostOfHalf)
        return decodeAs(QStringConverter::Utf16LE);
    if (mostOfHalf > 0 && zerosOdd == 0 && zerosEven >= mostOfHalf)
        return decodeAs(QStringConverter::Utf16BE);

    qsizetype validSequences = 0;
    qsizetype strayBytes = 0;
    for (qsizetype i = 0; i < bytes.size();) {
        const auto c = static_cast<unsigned char>(bytes.at(i));
        if (c < 0x80) {
            ++i;
            continue;
        }
        const qsizetype length = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        bool whole = length > 0 && i + length <= bytes.size();
        for (qsizetype k = 1; whole && k < length; ++k)
            whole = (static_cast<unsigned char>(bytes.at(i + k)) & 0xC0) == 0x80;
        if (whole) {
            ++validSequences;
            i += length;
        } else {
            ++strayBytes;
            ++i;
        }
    }
    if (strayBytes == 0)
        return decodeAs(QStringConverter::Utf8);
    return validSequences == 0 && decodeAs(QStringConverter::System);
}

} // namespace wallet
