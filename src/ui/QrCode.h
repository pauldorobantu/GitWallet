#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QVector>

namespace app::ui {

// A QR code (ISO/IEC 18004, model 2) of a run of bytes: the smallest symbol that holds them, in
// byte mode, with whichever mask the standard's penalty rules like best. --store-test reads its
// symbols back whole, and CI has two other decoders read them too.
class QrCode
{
public:
    enum class Correction { Low, Medium, Quartile, High };
    static constexpr int kMaxVersion = 40;

    // A null code when `data` does not fit the largest symbol there is.
    static QrCode encode(const QByteArray& data, Correction correction = Correction::Medium);

    bool isNull() const { return size_ == 0; }
    int size() const { return size_; } // modules along a side, without the quiet zone
    int version() const { return version_; }
    int mask() const { return mask_; }
    bool dark(int x, int y) const { return modules_.at(y * size_ + x) != 0; }

    // Dark on light whatever the theme: phones read that. `scale` pixels a module, with the four
    // modules of quiet zone the standard asks for.
    QImage toImage(int scale, const QColor& dark = Qt::black, const QColor& light = Qt::white) const;

    // How many bytes a symbol of this version holds, and how it splits them. For tests.
    static int capacity(int version, Correction correction);
    static int blockCount(int version, Correction correction);
    static int blockCorrection(int version, Correction correction);

private:
    QVector<quint8> modules_;
    int size_ = 0;
    int version_ = 0;
    int mask_ = 0;
};

} // namespace app::ui
