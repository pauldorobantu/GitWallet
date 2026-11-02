#include "ui/Figures.h"

#include "core/SettingsRepo.h"

#include <QLocale>
#include <QStringList>
#include <QtNumeric>

namespace app::ui::figures {

QString dash()
{
    return QStringLiteral("—");
}

QString formatAmount(const chain::Amount& amount, const wallet::AssetInfo& info)
{
    const int shown = info.shownDecimals;
    const QString plain = QString::fromStdString(amount.format(info.decimals, shown));
    // Dust that truncates to zero reads "<0.000001", as on the Addresses page:
    // a bare "0" next to "3 funded addresses" would look like a bug.
    if (plain == QLatin1String("0") && !amount.isZero() && shown > 0)
        return QStringLiteral("<0.") + QStringLiteral("1").rightJustified(shown, QLatin1Char('0'));
    const qsizetype dot = plain.indexOf(QLatin1Char('.'));
    QString whole = dot < 0 ? plain : plain.left(dot);
    QString fraction = dot < 0 ? QString() : plain.mid(dot + 1);
    if (info.dollar)
        fraction = fraction.leftJustified(2, QLatin1Char('0'));
    for (qsizetype i = whole.size() - 3; i > 0; i -= 3)
        whole.insert(i, QLatin1Char(','));
    return fraction.isEmpty() ? whole : whole + QLatin1Char('.') + fraction;
}

QString formatUsd(double value)
{
    if (!qIsFinite(value))
        return dash();
    if (value > 0.0 && value < 0.01)
        return QStringLiteral("<$0.01");
    return QStringLiteral("$") + QLocale(QLocale::English).toString(qMax(0.0, value), 'f', 2);
}

QString formatCount(int n)
{
    return QLocale(QLocale::English).toString(n);
}

QString dustLimits()
{
    QStringList parts;
    for (const wallet::AssetInfo& info : wallet::assets()) {
        if (!info.dust.isZero())
            parts << formatAmount(info.dust, info) + QLatin1Char(' ') + info.symbol;
    }
    return parts.join(QStringLiteral(", "));
}

bool hideDust()
{
    return SettingsRepo().get(QStringLiteral("ui.hideDust")) != QLatin1String("0");
}

void setHideDust(bool hide)
{
    SettingsRepo().set(QStringLiteral("ui.hideDust"), hide ? QStringLiteral("1") : QStringLiteral("0"));
}

QString countSoFar(int count, bool complete)
{
    if (complete)
        return formatCount(count);
    return count > 0 ? atLeast(formatCount(count)) : dash();
}

QString atLeast(const QString& figure)
{
    if (figure == dash())
        return figure;
    if (figure.startsWith(QLatin1String("<$")))
        return QStringLiteral("> $0");
    if (figure.startsWith(QLatin1Char('<')))
        return QStringLiteral("> 0");
    return QStringLiteral("≥ ") + figure;
}

} // namespace app::ui::figures
