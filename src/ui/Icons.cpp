#include "ui/Icons.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QGradientStops>
#include <QHash>
#include <QIcon>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QSvgRenderer>

namespace app::ui {

namespace {

// The Lucide icon set (lucide-static v0.544.0, ISC) — modern, stroke 2px,
// viewBox 24 — embedded in res/icons/lucide/. Fallback: our own vector
// drawing below, if the resource is missing.
QString svgPathFor(icon::Kind kind)
{
    switch (kind) {
    case icon::Kind::Dashboard:    return QStringLiteral(":/icons/lucide/Dashboard.svg");
    case icon::Kind::Profiles:     return QStringLiteral(":/icons/lucide/Profiles.svg");
    case icon::Kind::Sessions:     return QStringLiteral(":/icons/lucide/Sessions.svg");
    case icon::Kind::Flows:        return QStringLiteral(":/icons/lucide/Flows.svg");
    case icon::Kind::Pages:        return QStringLiteral(":/icons/lucide/Pages.svg");
    case icon::Kind::Plugins:      return QStringLiteral(":/icons/lucide/Plugins.svg");
    case icon::Kind::Traffic:      return QStringLiteral(":/icons/lucide/Traffic.svg");
    case icon::Kind::Cloud:        return QStringLiteral(":/icons/lucide/Cloud.svg");
    case icon::Kind::Shield:       return QStringLiteral(":/icons/lucide/Shield.svg");
    case icon::Kind::Mail:         return QStringLiteral(":/icons/lucide/Mail.svg");
    case icon::Kind::Logs:         return QStringLiteral(":/icons/lucide/Logs.svg");
    case icon::Kind::Settings:     return QStringLiteral(":/icons/lucide/Settings.svg");
    case icon::Kind::Search:       return QStringLiteral(":/icons/lucide/Search.svg");
    case icon::Kind::Plus:         return QStringLiteral(":/icons/lucide/Plus.svg");
    case icon::Kind::Cross:        return QStringLiteral(":/icons/lucide/Cross.svg");
    case icon::Kind::Play:         return QStringLiteral(":/icons/lucide/Play.svg");
    case icon::Kind::Stop:         return QStringLiteral(":/icons/lucide/Stop.svg");
    case icon::Kind::Refresh:      return QStringLiteral(":/icons/lucide/Refresh.svg");
    case icon::Kind::ChevronDown:  return QStringLiteral(":/icons/lucide/ChevronDown.svg");
    case icon::Kind::Minimize:     return QStringLiteral(":/icons/lucide/Minimize.svg");
    case icon::Kind::Maximize:     return QStringLiteral(":/icons/lucide/Maximize.svg");
    case icon::Kind::Restore:      return QStringLiteral(":/icons/lucide/Restore.svg");
    case icon::Kind::Close:        return QStringLiteral(":/icons/lucide/Cross.svg");
    case icon::Kind::Menu:         return QStringLiteral(":/icons/lucide/Menu.svg");
    case icon::Kind::Brush:        return QStringLiteral(":/icons/lucide/Brush.svg");
    case icon::Kind::Copy:         return QStringLiteral(":/icons/lucide/Copy.svg");
    case icon::Kind::Filter:       return QStringLiteral(":/icons/lucide/Filter.svg");
    case icon::Kind::ListRows:     return QStringLiteral(":/icons/lucide/ListRows.svg");
    case icon::Kind::MinusCircle:  return QStringLiteral(":/icons/lucide/MinusCircle.svg");
    case icon::Kind::Globe:        return QStringLiteral(":/icons/lucide/Globe.svg");
    case icon::Kind::ImportDown:   return QStringLiteral(":/icons/lucide/ImportDown.svg");
    case icon::Kind::Key:          return QStringLiteral(":/icons/lucide/Key.svg");
    case icon::Kind::ExternalLink: return QStringLiteral(":/icons/lucide/ExternalLink.svg");
    case icon::Kind::LayoutGrid:   return QStringLiteral(":/icons/lucide/LayoutGrid.svg");
    case icon::Kind::Pencil:       return QStringLiteral(":/icons/lucide/Pencil.svg");
    case icon::Kind::Check:        return QStringLiteral(":/icons/lucide/Check.svg");
    case icon::Kind::Power:        return QStringLiteral(":/icons/lucide/Power.svg");
    case icon::Kind::Sun:          return QStringLiteral(":/icons/lucide/Sun.svg");
    case icon::Kind::Moon:         return QStringLiteral(":/icons/lucide/Moon.svg");
    case icon::Kind::User:         return QStringLiteral(":/icons/lucide/User.svg");
    case icon::Kind::LogOut:       return QStringLiteral(":/icons/lucide/LogOut.svg");
    }
    return QString();
}

// Tinted Lucide SVG (stroke="currentColor" → the requested color).
QByteArray tintedLucideSvg(icon::Kind kind, const QColor& color)
{
    QFile f(svgPathFor(kind));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QString svg = QString::fromUtf8(f.readAll());
    svg.replace(QStringLiteral("currentColor"), color.name());
    return svg.toUtf8();
}

// Prepares a painter with rounded stroke at scale (icons are drawn in 24x24).
struct Canvas
{
    QPixmap pm;
    QPainter p;

    Canvas(int size, const QColor& color, qreal stroke)
    {
        // PHYSICAL allocation of size*dpr with DPR set: on screen the icon takes up exactly
        // `size` logical px and stays sharp at any Windows scaling (125/150/200%).
        // (The old variant allocated size physical px → at dpr>1 the icon SHRANK to
        // size/dpr logical px — exactly the „icons too small" bug.)
        const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
        pm = QPixmap(qRound(size * dpr), qRound(size * dpr));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        p.begin(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal s = size / 24.0;
        p.scale(s, s);
        // Minimum effective thickness ~1.75px: at 100% DPI a stroke of 1.6 * (14/24)
        // ≈ 0.9px antialiases to gray — „foggy icons". Capped at 2.2px so
        // large icons don't become massive.
        const qreal eff = qBound(1.75, stroke * s, 2.2);
        QPen pen(color, eff / s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
    }

    ~Canvas() { p.end(); }
};

QPainterPath line(qreal x1, qreal y1, qreal x2, qreal y2)
{
    QPainterPath path;
    path.moveTo(x1, y1);
    path.lineTo(x2, y2);
    return path;
}

QPainterPath roundedRectPath(qreal x, qreal y, qreal w, qreal h, qreal r)
{
    QPainterPath path;
    path.addRoundedRect(QRectF(x, y, w, h), r, r);
    return path;
}

QPainterPath circlePath(qreal cx, qreal cy, qreal r)
{
    QPainterPath path;
    path.addEllipse(QPointF(cx, cy), r, r);
    return path;
}

// Small filled dot/plate (duotone detail accent).
QPainterPath dotPath(qreal cx, qreal cy, qreal r)
{
    return circlePath(cx, cy, r);
}

} // namespace

QIcon makeIcon(icon::Kind kind, const QColor& color, int size, qreal stroke)
{
    // Global cache: tables with hundreds of rows request the same icons on every
    // reload — regenerating the pixmap (QPainter) was the main cause of
    // slowness (2000+ generations/reload for 448 profiles).
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(static_cast<int>(kind))
                            .arg(color.rgba())
                            .arg(size)
                            .arg(QString::number(stroke, 'f', 2));
    static QHash<QString, QIcon> cache;
    const auto hit = cache.constFind(key);
    if (hit != cache.constEnd())
        return hit.value();

    // 1) The tinted Lucide icon (the modern set), at the requested size.
    if (const QByteArray svg = tintedLucideSvg(kind, color); !svg.isEmpty()) {
        QSvgRenderer renderer(svg);
        if (renderer.isValid()) {
            const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
            QPixmap pm(qRound(size * dpr), qRound(size * dpr));
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            renderer.render(&p, QRectF(0, 0, size, size));
            p.end();
            const QIcon result(pm);
            cache.insert(key, result);
            return result;
        }
    }

    // 2) Fallback: our own vector drawing.
    Canvas c(size, color, stroke);
    QPainter& p = c.p;

    switch (kind) {
    case icon::Kind::Dashboard:
        p.drawPath(roundedRectPath(3.5, 3.5, 7, 7, 2));
        p.drawPath(roundedRectPath(13.5, 3.5, 7, 7, 2));
        p.drawPath(roundedRectPath(3.5, 13.5, 7, 7, 2));
        p.drawPath(roundedRectPath(13.5, 13.5, 7, 7, 2));
        break;
    case icon::Kind::Profiles:
        p.drawPath(roundedRectPath(3.5, 5, 17, 15, 3));
        p.drawPath(line(7.5, 9.5, 13, 9.5));
        p.drawPath(line(7.5, 13, 16.5, 13));
        p.drawPath(line(7.5, 16.5, 11, 16.5));
        break;
    case icon::Kind::Sessions:
        p.drawPath(roundedRectPath(3, 4.5, 18, 12.5, 2.5));
        p.drawPath(line(12, 9.5, 12, 12.5));
        p.drawPath(line(12, 9.5, 10, 11));
        p.drawPath(line(12, 9.5, 14, 11));
        p.drawPath(line(9, 20.5, 15, 20.5));
        break;    case icon::Kind::Flows: {
        QPainterPath nodes;
        nodes.addEllipse(QRectF(4.5, 4.5, 4, 4));
        nodes.addEllipse(QRectF(15.5, 8.5, 4, 4));
        nodes.addEllipse(QRectF(8.5, 15.5, 4, 4));
        p.drawPath(nodes);
        p.drawPath(line(8.0, 7.5, 15.0, 10.0));
        p.drawPath(line(11.0, 12.5, 13.5, 15.5));
        break;
    }
    case icon::Kind::Pages:
        p.drawPath(roundedRectPath(5, 3, 14, 18, 2.5));
        p.drawPath(line(8.5, 8, 15.5, 8));
        p.drawPath(line(8.5, 11.5, 15.5, 11.5));
        p.drawPath(line(8.5, 15, 12.5, 15));
        break;
    case icon::Kind::Plugins: {
        QPainterPath puzzle;
        puzzle.moveTo(9, 4.5);
        puzzle.lineTo(15, 4.5);
        puzzle.lineTo(15, 6.5);
        puzzle.arcTo(QRectF(15, 5.5, 4, 4), -90, 180);
        puzzle.lineTo(15, 10.5);
        puzzle.lineTo(19.5, 10.5);
        puzzle.lineTo(19.5, 15);
        puzzle.lineTo(17.5, 15);
        puzzle.arcTo(QRectF(16.5, 13, 4, 4), 0, 180);
        puzzle.lineTo(15, 15);
        puzzle.lineTo(15, 19.5);
        puzzle.lineTo(9, 19.5);
        puzzle.lineTo(9, 16);
        puzzle.lineTo(7.5, 16);
        puzzle.arcTo(QRectF(4.5, 13, 4, 4), 180, -180);
        puzzle.lineTo(9, 14);
        puzzle.lineTo(4.5, 14);
        puzzle.lineTo(4.5, 9);
        puzzle.lineTo(9, 9);
        puzzle.closeSubpath();
        p.drawPath(puzzle);
        break;
    }
    case icon::Kind::Traffic: {
        QPainterPath pulse;
        pulse.moveTo(3, 12);
        pulse.lineTo(7.5, 12);
        pulse.lineTo(10, 6.5);
        pulse.lineTo(14, 17.5);
        pulse.lineTo(16.5, 12);
        pulse.lineTo(21, 12);
        p.drawPath(pulse);
        break;
    }
    case icon::Kind::Cloud: {
        QPainterPath cloud;
        cloud.moveTo(7, 17.5);
        cloud.arcTo(QRectF(3.5, 12.5, 8, 5), 180, -180);
        cloud.lineTo(17, 17.5);
        cloud.arcTo(QRectF(13, 8.5, 8.5, 9), 180, -180);
        cloud.lineTo(8.5, 17.5);
        p.drawPath(cloud);
        break;
    }
    case icon::Kind::Shield: {
        QPainterPath shield;
        shield.moveTo(12, 3.5); shield.lineTo(19, 6.5); shield.lineTo(19, 12);
        shield.lineTo(12, 20.5); shield.lineTo(5, 12); shield.lineTo(5, 6.5);
        p.drawPath(shield);
        break;
    }
        p.drawPath(line(9, 12, 11, 14));
        p.drawPath(line(11, 14, 15, 9.5));
        break;
    case icon::Kind::Mail:
        p.drawPath(roundedRectPath(3.5, 5.5, 17, 13, 2.5));
        p.drawPath(line(4.5, 7.5, 12, 13));
        p.drawPath(line(19.5, 7.5, 12, 13));
        break;
    case icon::Kind::Logs:
        p.drawPath(roundedRectPath(3.5, 4, 17, 16, 2.5));
        p.drawPath(line(6.5, 9, 9.5, 12));
        p.drawPath(line(6.5, 15, 9.5, 12));
        p.drawPath(line(12, 15, 16.5, 15));
        break;
    case icon::Kind::Settings: {
        p.drawPath(line(4.5, 7.5, 15.5, 7.5));
        p.drawPath(line(19.5, 7.5, 19.5, 7.5));
        p.drawEllipse(QRectF(15.5, 5.5, 4, 4));
        p.drawPath(line(4.5, 16.5, 8.5, 16.5));
        p.drawEllipse(QRectF(8.5, 14.5, 4, 4));
        p.drawPath(line(12.5, 16.5, 19.5, 16.5));
        break;
    }
    case icon::Kind::Search:
        p.drawEllipse(QRectF(4.5, 4.5, 10.5, 10.5));
        p.drawPath(line(12.5, 12.5, 19.5, 19.5));
        break;
    case icon::Kind::Plus:
        p.drawPath(line(12, 5, 12, 19));
        p.drawPath(line(5, 12, 19, 12));
        break;
    case icon::Kind::Cross:
        p.drawPath(line(6, 6, 18, 18));
        p.drawPath(line(18, 6, 6, 18));
        break;
    case icon::Kind::Play: {
        QPainterPath play;
        play.moveTo(8.5, 5.5); play.lineTo(18.5, 12); play.lineTo(8.5, 18.5);
        p.drawPath(play);
        break;
    }
        break;
    case icon::Kind::Stop:
        p.drawPath(roundedRectPath(6.5, 6.5, 11, 11, 1.5));
        break;
    case icon::Kind::Refresh: {
        p.drawArc(QRectF(4.5, 4.5, 15, 15), 30 * 16, 280 * 16);
        QPainterPath arrow;
        arrow.moveTo(18, 4.5); arrow.lineTo(19.5, 9); arrow.lineTo(15, 9.5);
        p.drawPath(arrow);
        break;
    }
    case icon::Kind::ChevronDown:
        p.drawPath(line(7, 10, 12, 15));
        p.drawPath(line(12, 15, 17, 10));
        break;
    case icon::Kind::Minimize:
        p.drawPath(line(6, 12, 18, 12));
        break;
    case icon::Kind::Maximize:
        p.drawPath(roundedRectPath(6, 6, 12, 12, 1.5));
        break;
    case icon::Kind::Restore:
        p.drawPath(roundedRectPath(5.5, 8.5, 10, 10, 1.5));
        p.drawPath(line(8.5, 8.5, 8.5, 5.5));
        p.drawPath(line(8.5, 5.5, 18.5, 5.5));
        p.drawPath(line(18.5, 5.5, 18.5, 15.5));
        p.drawPath(line(18.5, 15.5, 15.5, 15.5));
        break;
    case icon::Kind::Close:
        p.drawPath(line(7, 7, 17, 17));
        p.drawPath(line(17, 7, 7, 17));
        break;
    case icon::Kind::Menu: // hamburger
        p.drawPath(line(4.5, 7, 19.5, 7));
        p.drawPath(line(4.5, 12, 19.5, 12));
        p.drawPath(line(4.5, 17, 19.5, 17));
        break;
    case icon::Kind::Brush: // appearance
        p.drawPath(line(12, 4, 12, 9));
        p.drawPath(line(6.5, 7, 17.5, 7));
        p.drawPath(roundedRectPath(7, 11, 10, 9, 2));
        p.drawPath(line(12, 7, 12, 11));
        break;
    case icon::Kind::Copy: // two overlapping cards (clone)
        p.drawPath(roundedRectPath(4.5, 4.5, 11, 11, 2.5));
        p.drawPath(roundedRectPath(8.5, 8.5, 11, 11, 2.5));
        break;
    case icon::Kind::Filter: { // funnel
        QPainterPath funnel;
        funnel.moveTo(4.5, 5.5);
        funnel.lineTo(19.5, 5.5);
        funnel.lineTo(14.5, 11.5);
        funnel.lineTo(14.5, 18.5);
        funnel.lineTo(9.5, 16);
        funnel.lineTo(9.5, 11.5);
        funnel.closeSubpath();
        p.drawPath(funnel);
        break;
    }
    case icon::Kind::ListRows: // sort: three rows with the order bar
        p.drawPath(line(8, 6.5, 19.5, 6.5));
        p.drawPath(line(8, 12, 19.5, 12));
        p.drawPath(line(8, 17.5, 19.5, 17.5));
        p.drawPath(line(4.5, 6.5, 5.5, 6.5));
        p.drawPath(line(4.5, 12, 5.5, 12));
        p.drawPath(line(4.5, 17.5, 5.5, 17.5));
        break;
    case icon::Kind::MinusCircle:
        p.drawPath(circlePath(12, 12, 8));
        p.drawPath(line(8.5, 12, 15.5, 12));
        break;
    case icon::Kind::Globe:
        p.drawPath(circlePath(12, 12, 8));
        p.drawPath(line(4, 12, 20, 12));
        p.drawPath(line(12, 4, 12, 20));
        p.drawArc(QRectF(7.5, 7.5, 9, 9), 200 * 16, 140 * 16);
        p.drawArc(QRectF(7.5, 7.5, 9, 9), 20 * 16, 140 * 16);
        break;
    case icon::Kind::Key:
        p.drawPath(circlePath(7.5, 10.5, 3.5));
        p.drawPath(line(10, 13, 17.5, 20.5));
        p.drawPath(line(14.5, 17.5, 17.5, 14.5));
        break;
    case icon::Kind::User: // head circle + shoulders
        p.drawPath(circlePath(12, 7, 4));
        p.drawPath(line(5, 21, 5, 19));
        p.drawArc(QRectF(5, 15, 8, 8), 90 * 16, 90 * 16);
        p.drawPath(line(9, 15, 15, 15));
        p.drawArc(QRectF(11, 15, 8, 8), 0, 90 * 16);
        p.drawPath(line(19, 19, 19, 21));
        break;
    case icon::Kind::ExternalLink: // external arrow out of a box
        p.drawPath(line(14, 5, 19.5, 5));
        p.drawPath(line(19.5, 5, 19.5, 10.5));
        p.drawPath(line(11, 13, 19.5, 5));
        p.drawPath(line(11, 8, 5, 8));
        p.drawPath(line(5, 8, 5, 19));
        p.drawPath(line(5, 19, 16, 19));
        p.drawPath(line(16, 19, 16, 13));
        break;
    case icon::Kind::ImportDown: // tray with a downward arrow
        p.drawPath(line(12, 4, 12, 13.5));
        p.drawPath(line(8.5, 10.5, 12, 13.5));
        p.drawPath(line(15.5, 10.5, 12, 13.5));
        p.drawPath(line(4.5, 16, 4.5, 19.5));
        p.drawPath(line(4.5, 19.5, 19.5, 19.5));
        p.drawPath(line(19.5, 19.5, 19.5, 16));
        break;
    case icon::Kind::LogOut: // door frame with an arrow leaving it
        p.drawPath(line(9, 3.5, 5, 3.5));
        p.drawPath(line(5, 3.5, 5, 20.5));
        p.drawPath(line(5, 20.5, 9, 20.5));
        p.drawPath(line(9, 12, 20.5, 12));
        p.drawPath(line(16, 7.5, 20.5, 12));
        p.drawPath(line(16, 16.5, 20.5, 12));
        break;
    }

    const QIcon result(c.pm);
    cache.insert(key, result);
    return result;
}

QIcon makeSolidIcon(icon::Kind kind, const QColor& color, int size)
{
    // Variantă PLINĂ pt glyph-urile care arată slab ca simplu contur subțire
    // (Play/Stop pe rânduri = controale media; conturul gol părea „amatoricesc").
    // Umplem path-ul Lucide (fill=color) și renunțăm la stroke.
    const QString key = QStringLiteral("solid|%1|%2|%3")
                            .arg(static_cast<int>(kind))
                            .arg(color.rgba())
                            .arg(size);
    static QHash<QString, QIcon> cache;
    const auto hit = cache.constFind(key);
    if (hit != cache.constEnd())
        return hit.value();

    QFile f(svgPathFor(kind));
    if (f.open(QIODevice::ReadOnly)) {
        QString svg = QString::fromUtf8(f.readAll());
        svg.replace(QStringLiteral("fill=\"none\""),
                    QStringLiteral("fill=\"%1\"").arg(color.name()));
        svg.replace(QStringLiteral("stroke=\"currentColor\""),
                    QStringLiteral("stroke=\"none\""));
        QSvgRenderer renderer(svg.toUtf8());
        if (renderer.isValid()) {
            const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
            QPixmap pm(qRound(size * dpr), qRound(size * dpr));
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            renderer.render(&p, QRectF(0, 0, size, size));
            p.end();
            const QIcon result(pm);
            cache.insert(key, result);
            return result;
        }
    }
    // Fallback: conturul normal.
    return makeIcon(kind, color, size, 2.0);
}

QIcon makeCaptionIcon(icon::Kind kind, const QColor& color, int size)
{
    const QString key = QStringLiteral("cap|%1|%2|%3")
                            .arg(static_cast<int>(kind))
                            .arg(color.rgba())
                            .arg(size);
    static QHash<QString, QIcon> capCache;
    const auto hit = capCache.constFind(key);
    if (hit != capCache.constEnd())
        return hit.value();

    // Win11 caption glyphs on a 16 grid (10×10 body centered in 3..13),
    // thin flat stroke — geometrically precise, not „stock" icons.
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(qRound(size * dpr), qRound(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 16.0, size / 16.0);
    QPen pen(color, 1.3, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    switch (kind) {
    case icon::Kind::Minimize: // horizontal line at the center (3..13)
        p.drawLine(QLineF(3, 8, 13, 8));
        break;
    case icon::Kind::Maximize: // 10×10 square (the outline covers exactly 3..13)
        p.drawRect(QRectF(3.65, 3.65, 8.7, 8.7));
        break;
    case icon::Kind::Restore: { // two overlapping squares; the back one is
        // „cut" under the front one (DestinationOut) — classic Windows outline.
        const QRectF front(3, 5, 8, 8);  // (3,5)-(11,13)
        const QRectF back(5, 3, 8, 8);   // (5,3)-(13,11)
        p.drawRect(back);
        const qreal m = pen.widthF() / 2 + 0.3;
        p.save();
        p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        p.drawRect(front.adjusted(-m, -m, m, m));
        p.restore();
        p.setPen(pen);
        p.drawRect(front);
        break;
    }
    case icon::Kind::Close: // X with flat caps
        p.drawLine(QLineF(3.5, 3.5, 12.5, 12.5));
        p.drawLine(QLineF(12.5, 3.5, 3.5, 12.5));
        break;
    default:
        break;
    }
    p.end();

    const QIcon result(pm);
    capCache.insert(key, result);
    return result;
}

// The duotone navigation glyphs — richer compositions than the
// line-only variant: filled elements (alpha), accent dots, extra details.
static void drawNavGlyph(QPainter& p, icon::Kind kind, const QColor& color)
{
    const QColor tint = color;
    const QColor soft(color.red(), color.green(), color.blue(), 70);

    switch (kind) {
    case icon::Kind::Dashboard: {
        // 2x2 with one filled plate (accent) + corner dots
        p.drawPath(roundedRectPath(3, 3, 8.2, 8.2, 2.6));
        p.drawPath(roundedRectPath(12.8, 3, 8.2, 8.2, 2.6));
        p.drawPath(roundedRectPath(3, 12.8, 8.2, 8.2, 2.6));
        p.setBrush(soft);
        p.drawPath(roundedRectPath(12.8, 12.8, 8.2, 8.2, 2.6));
        p.setBrush(Qt::NoBrush);
        p.drawPath(dotPath(16.9, 17, 1.0));
        break;
    }
    case icon::Kind::Profiles: {
        // Identity card with avatar (head + shoulders) and data lines
        p.drawPath(roundedRectPath(2.8, 4.8, 18.4, 14.4, 3.2));
        p.drawPath(circlePath(8.3, 10.2, 2.1));
        QPainterPath shoulders;
        shoulders.moveTo(5.2, 15.8);
        shoulders.quadTo(8.3, 12.6, 11.4, 15.8);
        p.drawPath(shoulders);
        p.drawPath(line(14, 9.4, 19, 9.4));
        p.drawPath(line(14, 12, 19, 12));
        p.drawPath(line(14, 14.6, 17.2, 14.6));
        break;
    }
    case icon::Kind::Sessions: {
        // Monitor + tripod + filled play triangle
        p.drawPath(roundedRectPath(2.8, 3.8, 18.4, 12.4, 2.6));
        p.drawPath(line(12, 16.2, 12, 19.4));
        p.drawPath(line(8, 20.4, 16, 20.4));
        QPainterPath play;
        play.moveTo(10.2, 7.2);
        play.lineTo(15.4, 10.0);
        play.lineTo(10.2, 12.8);
        play.closeSubpath();
        p.setBrush(tint);
        p.drawPath(play);
        p.setBrush(Qt::NoBrush);
        break;
    }
    case icon::Kind::Flows: {
        // Connected nodes + arrow
        p.drawPath(circlePath(5.2, 6.4, 2.3));
        p.drawPath(circlePath(18.6, 9.2, 2.3));
        p.setBrush(soft);
        p.drawPath(circlePath(9.4, 18.4, 2.6));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line(7.2, 7.6, 16.4, 8.8));
        p.drawPath(line(6.6, 8.4, 8.6, 16.2));
        p.drawPath(line(11.6, 17.2, 15.4, 11.2));
        break;
    }
    case icon::Kind::Pages: {
        // Document with a fold + code lines
        QPainterPath doc;
        doc.moveTo(6, 2.8);
        doc.lineTo(14.6, 2.8);
        doc.lineTo(18.8, 7.0);
        doc.lineTo(18.8, 21.2);
        doc.lineTo(6, 21.2);
        doc.closeSubpath();
        p.drawPath(doc);
        p.drawPath(line(14.6, 2.8, 14.6, 7.0));
        p.drawPath(line(14.6, 7.0, 18.8, 7.0));
        p.drawPath(line(8.8, 11.4, 15.8, 11.4));
        p.drawPath(line(8.8, 14.4, 13.8, 14.4));
        p.drawPath(line(8.8, 17.4, 11.8, 17.4));
        break;
    }
    case icon::Kind::Plugins: {
        // Puzzle with a knob and notch
        QPainterPath piece;
        piece.moveTo(5.0, 7.2);
        piece.lineTo(10.2, 7.2);
        piece.arcTo(QRectF(9.0, 4.4, 5.2, 5.2), 180, -180);
        piece.lineTo(18.6, 7.2);
        piece.lineTo(18.6, 16.4);
        piece.lineTo(13.6, 16.4);
        piece.arcTo(QRectF(12.4, 13.6, 5.2, 5.2), 0, 180);
        piece.lineTo(5.0, 18.8);
        piece.closeSubpath();
        p.drawPath(piece);
        p.drawPath(dotPath(11.6, 12.2, 1.0));
        break;
    }
    case icon::Kind::Traffic: {
        // ECG pulse with a filled end dot
        QPainterPath pulse;
        pulse.moveTo(2.8, 12.6);
        pulse.lineTo(7.6, 12.6);
        pulse.lineTo(9.8, 6.8);
        pulse.lineTo(13.4, 17.6);
        pulse.lineTo(15.6, 12.6);
        pulse.lineTo(18.4, 12.6);
        p.drawPath(pulse);
        p.setBrush(tint);
        p.drawPath(dotPath(20.6, 12.6, 1.4));
        p.setBrush(Qt::NoBrush);
        break;
    }
    case icon::Kind::Cloud: {
        // Cloud with two bulges + upload arrow
        QPainterPath cloud;
        cloud.moveTo(6.4, 17.4);
        cloud.quadTo(2.6, 17.4, 2.6, 13.8);
        cloud.quadTo(2.6, 10.8, 5.6, 10.4);
        cloud.quadTo(6.2, 5.4, 10.8, 5.4);
        cloud.quadTo(14.6, 5.4, 15.6, 9.0);
        cloud.quadTo(21.2, 9.2, 21.2, 13.4);
        cloud.quadTo(21.2, 17.4, 17.2, 17.4);
        cloud.closeSubpath();
        p.drawPath(cloud);
        p.drawPath(line(12, 20.2, 12, 14.6));
        p.drawPath(line(9.6, 17.0, 12, 14.6));
        p.drawPath(line(14.4, 17.0, 12, 14.6));
        break;
    }
    case icon::Kind::Shield: {
        // Shield with a checkmark
        QPainterPath shield;
        shield.moveTo(12, 2.8);
        shield.lineTo(19.6, 5.8);
        shield.lineTo(19.6, 11.4);
        shield.quadTo(19.6, 17.2, 12, 21.2);
        shield.quadTo(4.4, 17.2, 4.4, 11.4);
        shield.lineTo(4.4, 5.8);
        shield.closeSubpath();
        p.drawPath(shield);
        QPainterPath check;
        check.moveTo(8.4, 11.6);
        check.lineTo(11.0, 14.2);
        check.lineTo(15.8, 8.8);
        p.drawPath(check);
        break;
    }
    case icon::Kind::Mail: {
        // Envelope with a seal bulge + dot
        p.drawPath(roundedRectPath(2.8, 5.0, 18.4, 14.0, 2.8));
        QPainterPath flap;
        flap.moveTo(3.4, 6.4);
        flap.quadTo(12, 12.6, 20.6, 6.4);
        p.drawPath(flap);
        p.setBrush(soft);
        p.drawPath(dotPath(12, 15.4, 1.2));
        p.setBrush(Qt::NoBrush);
        break;
    }
    case icon::Kind::Logs: {
        // Terminal with a „>_" prompt
        p.drawPath(roundedRectPath(2.8, 3.8, 18.4, 16.4, 2.8));
        p.drawPath(line(2.8, 7.6, 21.2, 7.6));
        p.setBrush(soft);
        p.drawPath(dotPath(5.4, 5.7, 0.9));
        p.drawPath(dotPath(8.0, 5.7, 0.9));
        p.setBrush(Qt::NoBrush);
        QPainterPath gt;
        gt.moveTo(6.4, 11.0);
        gt.lineTo(9.6, 13.6);
        gt.lineTo(6.4, 16.2);
        p.drawPath(gt);
        p.drawPath(line(11.4, 16.4, 16.4, 16.4));
        break;
    }
    case icon::Kind::Settings: {
        // Horizontal sliders with filled knobs
        p.drawPath(line(3.0, 8.0, 21.0, 8.0));
        p.drawPath(line(3.0, 16.0, 21.0, 16.0));
        p.setBrush(tint);
        p.drawPath(circlePath(15.6, 8.0, 2.4));
        p.setBrush(soft);
        p.drawPath(circlePath(8.0, 16.0, 2.4));
        p.setBrush(Qt::NoBrush);
        break;
    }
    default: {
        // The remaining cases: the existing line-only glyph is drawn by makeIcon;
        // for duotone we fall back to a simple diamond with a dot.
        p.drawPath(roundedRectPath(5, 5, 14, 14, 4));
        p.drawPath(dotPath(12, 12, 1.2));
        break;
    }
    }
}

QIcon makeDuotoneIcon(icon::Kind kind, const QColor& color, int size)
{
    const QString key = QStringLiteral("duo|%1|%2|%3")
                            .arg(static_cast<int>(kind))
                            .arg(color.rgba())
                            .arg(size);
    static QHash<QString, QIcon> duoCache;
    const auto hit = duoCache.constFind(key);
    if (hit != duoCache.constEnd())
        return hit.value();

    Canvas c(size, color, 1.9);

    // The duotone plate: rounded, tinted (~18% alpha) — visible but discreet.
    QColor plate(color.red(), color.green(), color.blue(), 46);
    c.p.setPen(Qt::NoPen);
    c.p.setBrush(plate);
    c.p.drawRoundedRect(QRectF(0.6, 0.6, 22.8, 22.8), 7.0, 7.0);
    c.p.setBrush(Qt::NoBrush);

    // Glyph: tinted Lucide; fallback to the duotone vector drawing.
    bool usedLucide = false;
    if (const QByteArray svg = tintedLucideSvg(kind, color); !svg.isEmpty()) {
        QSvgRenderer renderer(svg);
        if (renderer.isValid()) {
            renderer.render(&c.p, QRectF(3.4, 3.4, 17.2, 17.2));
            usedLucide = true;
        }
    }
    if (!usedLucide) {
        QPen pen(color, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        c.p.setPen(pen);
        drawNavGlyph(c.p, kind, color);
    }
    const QIcon result(c.pm);
    duoCache.insert(key, result);
    return result;
}

// ---- Official brand logos (Services) ----

namespace {

QIcon googleLogo(int size)
{
    // Google's four-color „G": ring of 4 official arcs + horizontal bar.
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(qRound(size * dpr), qRound(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 24.0, size / 24.0);

    const QColor red(0xEA, 0x43, 0x35), yellow(0xFB, 0xBC, 0x05),
        green(0x34, 0xA8, 0x53), blue(0x42, 0x85, 0xF4);
    const QRectF ring(2.7, 2.7, 18.6, 18.6); // centered, radius ~9.3
    const auto arc = [&p, &ring](const QColor& c, int start16, int span16) {
        p.setPen(QPen(c, 3.2, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        p.drawArc(ring, start16, span16);
    };
    arc(red, 30 * 16, 90 * 16);    // top
    arc(yellow, 120 * 16, 90 * 16); // left
    arc(green, 210 * 16, 80 * 16);  // bottom
    arc(blue, 290 * 16, 70 * 16);   // bottom-right → end of the bar
    p.setPen(Qt::NoPen);
    p.setBrush(blue);
    p.drawRoundedRect(QRectF(10.6, 10.4, 11.2, 3.2), 1.6, 1.6);
    p.end();
    return QIcon(pm);
}

QIcon microsoftLogo(int size)
{
    // The 4 Microsoft squares, official colors.
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(qRound(size * dpr), qRound(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 24.0, size / 24.0);
    p.setPen(Qt::NoPen);
    const qreal s = 9.6, g = 2.2, m = 12.4;
    const QRectF tl(g, g, s, s), tr(m, g, s, s), bl(g, m, s, s), br(m, m, s, s);
    p.setBrush(QColor(0xF2, 0x50, 0x22));
    p.drawRoundedRect(tl, 0.6, 0.6);
    p.setBrush(QColor(0x7F, 0xBA, 0x00));
    p.drawRoundedRect(tr, 0.6, 0.6);
    p.setBrush(QColor(0x00, 0xA4, 0xEF));
    p.drawRoundedRect(bl, 0.6, 0.6);
    p.setBrush(QColor(0xFF, 0xB9, 0x00));
    p.drawRoundedRect(br, 0.6, 0.6);
    p.end();
    return QIcon(pm);
}

QIcon monogramFallback(const QString& slug, const QString& displayName, int size)
{
    // Service with no known brand: the initial on a colored disc (hash of the slug).
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(qRound(size * dpr), qRound(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const int r = size / 2;
    // Paletă curată, cohezivă (nu HSL aleator) — aceeași ca avatarurile Profiles.
    static const QColor kAvatar[] = {
        QColor(0x3E, 0x9E, 0x86), QColor(0xC9, 0x97, 0x5A), QColor(0xC5, 0x77, 0x61),
        QColor(0x93, 0xA0, 0x5E), QColor(0xA8, 0x7E, 0xA0), QColor(0x6F, 0x93, 0xA6),
        QColor(0xC7, 0x7B, 0x86), QColor(0x7E, 0x9A, 0x8C),
    };
    p.setBrush(kAvatar[qHash(slug) % (sizeof(kAvatar) / sizeof(kAvatar[0]))]);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(r, r), r - 1.0, r - 1.0);
    QFont f = qApp ? qApp->font() : QFont();
    f.setPixelSize(qMax(10, qRound(size * 0.52)));
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(0xFF, 0xFF, 0xFF));
    p.drawText(QRectF(0, 0, size, size), Qt::AlignCenter,
               displayName.left(1).toUpper());
    p.end();
    return QIcon(pm);
}

} // namespace

QIcon makeBrandLogo(const QString& slug, const QString& displayName, int size)
{
    const QString key = QStringLiteral("brand|%1|%2|%3")
                            .arg(slug, QString::number(size))
                            .arg(displayName.left(1));
    static QHash<QString, QIcon> brandCache;
    const auto hit = brandCache.constFind(key);
    if (hit != brandCache.constEnd())
        return hit.value();

    QIcon result;
    if (slug == QLatin1String("google") || slug == QLatin1String("google-rp"))
        result = googleLogo(size);
    else if (slug == QLatin1String("microsoft"))
        result = microsoftLogo(size);
    if (result.isNull())
        result = monogramFallback(slug, displayName, size);

    brandCache.insert(key, result);
    return result;
}

} // namespace app::ui
