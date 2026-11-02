#include "ui/CoinIcon.h"

#include <QApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QScreen>

namespace app::ui {

namespace {

QColor madeUpColor(const QString& key)
{
    // Stable from run to run, unlike qHash, and in the same range of strength as real ones.
    uint hash = 2166136261u;
    for (const QChar c : key)
        hash = (hash ^ c.unicode()) * 16777619u;
    return QColor::fromHsv(int(hash % 360), 150, 190);
}

QIcon roundMark(const QColor& color, QString glyph, const QString& fallback, int size)
{
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap canvas(qRound(size * ratio), qRound(size * ratio));
    canvas.setDevicePixelRatio(ratio);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(0, 0, size, size));
    QFont font = QApplication::font();
    font.setBold(true);
    font.setPixelSize(qRound(size * 0.56));
    // A font without the glyph would draw an empty box.
    const QFontMetricsF metrics(font);
    if (glyph.isEmpty() || !metrics.inFontUcs4(glyph.toUcs4().value(0)))
        glyph = fallback;
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, glyph);
    p.end();
    return QIcon(canvas);
}

} // namespace

QColor coinColor(const wallet::AssetInfo& info)
{
    const QColor listed(info.iconColor);
    return listed.isValid() ? listed : madeUpColor(info.key);
}

QIcon coinIcon(const wallet::AssetInfo& info, int size)
{
    return roundMark(coinColor(info), info.iconGlyph, info.symbol.left(1).toUpper(), size);
}

QIcon nameIcon(const QString& name, int size)
{
    const QString letter = name.trimmed().left(1).toUpper();
    return roundMark(madeUpColor(name), letter, QStringLiteral("?"), size);
}

} // namespace app::ui
