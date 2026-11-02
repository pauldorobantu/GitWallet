#pragma once

#include <QChar>
#include <QString>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace app::ui {

// How a spreadsheet on this PC reads a CSV: Excel splits columns on the Windows
// list separator and reads numbers with its decimal symbol, so an export in any
// other form lands in one column ("," is ";" on Romanian, German or French formats).
struct CsvFormat {
    QChar separator = QLatin1Char(',');
    QChar decimal = QLatin1Char('.');
};

inline CsvFormat csvFormat()
{
    CsvFormat format;
#ifdef Q_OS_WIN
    wchar_t text[8] = {};
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SLIST, text, 8) >= 2)
        format.separator = QChar(text[0]);
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, text, 8) >= 2)
        format.decimal = QChar(text[0]);
#endif
    return format;
}

// A CSV field: quoted when it has to be. A name opening with what a spreadsheet
// reads as a formula is defused, so opening the export never runs anything.
inline QString csvField(const QString& text, const CsvFormat& format)
{
    QString field = text;
    if (!field.isEmpty() && QStringLiteral("=+-@\t\r").contains(field.at(0)))
        field.prepend(QLatin1Char('\''));
    if (field.contains(format.separator) || field.contains(QLatin1Char('"')) || field.contains(QLatin1Char('\n'))
        || field.contains(QLatin1Char('\r'))) {
        field.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        field = QLatin1Char('"') + field + QLatin1Char('"');
    }
    return field;
}

// A number as this PC's spreadsheet reads it; `plain` has a '.' for a decimal point. GitWallet
// made it, so a minus sign in front is one and stays one.
inline QString csvNumber(QString plain, const CsvFormat& format)
{
    plain.replace(QLatin1Char('.'), format.decimal);
    return plain.contains(format.separator) ? QLatin1Char('"') + plain + QLatin1Char('"') : plain;
}

} // namespace app::ui
