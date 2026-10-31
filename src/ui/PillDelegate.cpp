#include "ui/PillDelegate.h"
#include "ui/PageKit.h"
#include "ui/ThemeManager.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>

namespace app::ui {

namespace {

struct PillStyle
{
    QColor dot;
    QColor text;
    QColor bg;
};

PillStyle styleFor(const QString& v)
{
    auto* theme = theme::ThemeManager::instance();
    const auto make = [&theme](const QString& role) {
        const QColor c = theme->color(role);
        return PillStyle{ c, c, QColor(c.red(), c.green(), c.blue(), 40) };
    };

    // Stări „pornit/activ" legate de identitatea aplicației → ACCENT (urmează
    // culoarea aleasă); înainte foloseau „success" (teal fix = accentul default),
    // deci rămâneau verzi când userul schimba accentul.
    if (v == QLatin1String("running") || v == QLatin1String("active")
        || v == QLatin1String("enabled"))
        return make(QStringLiteral("accent"));
    // Rezultate reale de succes (pass/ok/citit) → verdele semantic.
    if (v == QLatin1String("ok") || v == QLatin1String("read")
        || v == QLatin1String("success") || v == QLatin1String("allow")
        || v == QLatin1String("whitelist"))
        return make(QStringLiteral("success"));
    if (v == QLatin1String("error") || v == QLatin1String("failed")
        || v == QLatin1String("crashed") || v == QLatin1String("blocked")
        || v == QLatin1String("moved") || v == QLatin1String("deactivated"))
        return make(QStringLiteral("error"));
    if (v == QLatin1String("warning") || v == QLatin1String("untested")
        || v == QLatin1String("flagged") || v == QLatin1String("pending")
        || v == QLatin1String("initializing"))
        return make(QStringLiteral("warning"));
    if (v == QLatin1String("disabled") || v == QLatin1String("stopped")
        || v == QLatin1String("inactive") || v == QLatin1String("unread")
        || v == QLatin1String("off")) {
        // Muted, but legible: gray dot, light text, slightly denser background.
        const QColor m = theme->color(QStringLiteral("muted"));
        return PillStyle{ m, theme->color(QStringLiteral("textSoft")),
                          QColor(m.red(), m.green(), m.blue(), 46) };
    }
    return make(QStringLiteral("info"));
}

} // namespace

QString PillDelegate::normalize(const QString& raw)
{
    const QString v = raw.trimmed().toLower();
    if (v == QLatin1String("block"))
        return QStringLiteral("blocked");
    if (v == QLatin1String("fail"))
        return QStringLiteral("failed");
    return v;
}

void PillDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                         const QModelIndex& index) const
{
    const QString value = normalize(index.data().toString());
    if (value.isEmpty()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    // Fără fundal de selecție per celulă: banda rândului (HoverRow / RowBands)
    // e pictată dedesubt, uniform pe tot rândul.

    const PillStyle st = styleFor(value);
    QFont f = option.font;
    // The theme font may be defined in pixels → pointSizeF() == -1; subtract 1px instead.
    if (f.pointSizeF() > 0)
        f.setPointSizeF(f.pointSizeF() * 0.92);
    else if (f.pixelSize() > 1)
        f.setPixelSize(f.pixelSize() - 1);
    f.setWeight(QFont::DemiBold);
    const QFontMetrics fm(f);

    const int dotR = 3;
    const int padX = 9;
    const int gap = 6;
    const int textW = fm.horizontalAdvance(value);
    const int pillH = 20;
    const int pillW = textW + padX * 2 + dotR * 2 + gap;
    QRect pill(option.rect.left() + 8,
               option.rect.center().y() - pillH / 2,
               pillW, pillH);

    QPainterPath round;
    // Pastilă pe temele rotunjite, dreptunghi pe „Square" (urmează tema).
    const qreal pr = ui::page::corner(pillH / 2.0);
    round.addRoundedRect(pill, pr, pr);
    painter->fillPath(round, st.bg);

    QPoint dotCenter(pill.left() + padX + dotR, pill.center().y());
    painter->setPen(Qt::NoPen);
    painter->setBrush(st.dot);
    painter->drawEllipse(dotCenter, dotR, dotR);

    painter->setFont(f);
    painter->setPen(st.text);
    painter->drawText(QRect(pill.left() + padX + dotR * 2 + gap, pill.top(),
                            textW + 2, pillH),
                      Qt::AlignVCenter | Qt::AlignLeft, value);
    painter->restore();
}

QSize PillDelegate::sizeHint(const QStyleOptionViewItem& option,
                             const QModelIndex& index) const
{
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    s.setHeight(page::kRowHeight); // standard row (single token), like Services
    return s;
}

} // namespace app::ui
