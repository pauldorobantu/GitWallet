#include "ui/AppIcon.h"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QDataStream>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QPixmap>

#include <iterator>

namespace app::ui {

namespace {

// The sizes Windows asks a window icon for (SM_CXSMICON / SM_CXICON from 100 %
// to 300 % scaling) plus the large views — each one an exact match.
constexpr int kWindowSizes[] = { 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 96, 128, 256 };
// The exe's icon: Explorer's views and the pinned taskbar at the usual scalings.
constexpr int kIcoSizes[] = { 16, 20, 24, 32, 40, 48, 64, 256 };

// One 32-bit DIB image of an .ico: the header (height doubled for the mask),
// the BGRA rows bottom-up, then the 1-bit AND mask (set = transparent) for the
// readers that still use it.
QByteArray dibImage(const QImage& image)
{
    // ARGB32 is B, G, R, A in memory on x86/x64 — the DIB order, straight alpha.
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    const int w = argb.width();
    const int h = argb.height();
    const int maskStride = (w + 31) / 32 * 4;
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << quint32(40) << qint32(w) << qint32(h * 2) << quint16(1) << quint16(32)
       << quint32(0) << quint32(w * h * 4 + maskStride * h) << qint32(0) << qint32(0)
       << quint32(0) << quint32(0);
    for (int y = h - 1; y >= 0; --y)
        ds.writeRawData(reinterpret_cast<const char*>(argb.constScanLine(y)), w * 4);
    for (int y = h - 1; y >= 0; --y) {
        QByteArray row(maskStride, '\0');
        const auto* px = reinterpret_cast<const QRgb*>(argb.constScanLine(y));
        for (int x = 0; x < w; ++x)
            if (qAlpha(px[x]) == 0)
                row[x / 8] = char(row[x / 8] | (0x80 >> (x % 8)));
        ds.writeRawData(row.constData(), int(row.size()));
    }
    return out;
}

QByteArray pngImage(const QImage& image)
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return out;
}

} // namespace

QImage renderAppIcon(int size, const QColor& accent, bool square)
{
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    // A 1/16 margin in whole pixels: the square's edges sit on the pixel grid.
    const int margin = qMax(1, qRound(size / 16.0));
    const QRect box(margin, margin, size - 2 * margin, size - 2 * margin);
    QPainter p(&image);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(accent.rgb())); // always opaque
    if (square) {
        p.drawRect(box);
    } else {
        p.setRenderHint(QPainter::Antialiasing);
        p.drawEllipse(QRectF(box));
    }
    p.end();
    return image;
}

QByteArray appIconIco(const QColor& accent, bool square)
{
    const int count = int(std::size(kIcoSizes));
    QList<QByteArray> images;
    for (const int size : kIcoSizes) {
        const QImage image = renderAppIcon(size, accent, square);
        // 256 px as PNG (read since Vista); a 256 px DIB would weigh ~260 KB.
        images.append(size >= 256 ? pngImage(image) : dibImage(image));
    }

    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << quint16(0) << quint16(1) << quint16(count); // ICONDIR: type 1 = icon
    quint32 offset = 6 + 16 * count;
    for (int i = 0; i < count; ++i) {
        // ICONDIRENTRY: width, height (0 = 256), no palette, 1 plane, 32 bpp.
        const quint8 side = kIcoSizes[i] >= 256 ? 0 : quint8(kIcoSizes[i]);
        ds << side << side << quint8(0) << quint8(0) << quint16(1) << quint16(32)
           << quint32(images[i].size()) << offset;
        offset += quint32(images[i].size());
    }
    for (const QByteArray& image : images)
        ds.writeRawData(image.constData(), int(image.size()));
    return out;
}

void setAppIcon(const QColor& accent, bool square)
{
    // A new window icon re-creates the native icon of every top-level window,
    // and apply() also runs for changes the mark ignores (theme, font, …).
    static QRgb shownRgb = 0;
    static int shownShape = -1;
    if (accent.rgb() == shownRgb && int(square) == shownShape)
        return;
    shownRgb = accent.rgb();
    shownShape = int(square);

    QIcon icon;
    for (const int size : kWindowSizes)
        icon.addPixmap(QPixmap::fromImage(renderAppIcon(size, accent, square)));
    QGuiApplication::setWindowIcon(icon);
}

} // namespace app::ui
