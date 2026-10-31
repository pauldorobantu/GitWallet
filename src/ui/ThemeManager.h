#pragma once

#include "ui/design/Tokens.h"

#include <QColor>
#include <QHash>
#include <QObject>

class QAbstractNativeEventFilter;

namespace app::ui::theme {

// Appearance settings (Settings → Appearance) — persisted in system_settings
// with the "appearance." prefix.
struct Appearance
{
    QString theme = QStringLiteral("dark"); // dark | light | system
    QString accent = QStringLiteral("#2BBD8E"); // teal MEmu Multi
    QString border = QString();             // empty → the palette default
    int borderWidth = 1;                    // 0 | 1 | 2
    int radius = tokens::radius::kDefault;  // 0 | 6 | 10
    int fontDelta = 0;                      // -1 | 0 | +2 (pt)
    bool animations = true;
    bool navCollapsed = false;

    // "Square" corners: the whole-shape marks (swatches, avatar, app icon)
    // turn from discs into squares.
    bool squareCorners() const { return radius == 0; }
};

// The single source of truth for style: the palette (dark/light) and the
// user's Appearance resolve the design tokens (ui/design/Tokens.h), which fill
// the stylesheet template res/theme.qss. apply() changes the theme instantly
// (stylesheet + palette + font), without a restart.
class ThemeManager : public QObject
{
    Q_OBJECT
public:
    static ThemeManager* instance();

    void loadFromSettings();
    void saveToSettings() const;

    const Appearance& appearance() const { return app_; }
    void updateAppearance(const Appearance& app); // apply + save + changed

    bool isDark() const;              // resolves "system" via the Windows registry
    bool animationsEnabled() const { return app_.animations; }
    // In-memory only (not in the repo) — used by shoot-ui for captures without fade.
    void setAnimationsEnabled(bool on) { app_.animations = on; }
#ifdef APP_SMOKE
    // --smoke-theme: generateQss() for any appearance — no save, no apply.
    void setAppearanceForTest(const Appearance& app) { app_ = app; }
#endif

    // The value of a colour role for the active theme, accent and border.
    QColor color(Color role) const;
    // The same by name: the Color in lower camel case ("accent", "textSoft",
    // "rowSelection", …). Older call sites; an unknown name gives TextSoft.
    QColor color(const QString& role) const;
    // The corner radii for the user's corner setting.
    tokens::Radii radii() const { return tokens::radiiFor(app_.radius); }
    int fontDelta() const { return app_.fontDelta; }
    int px(int base) const { return base + app_.fontDelta; } // for QSS sizes
    // Raza pt. desen custom (QPainter, QSS inline): `natural` pe temele
    // rotunjite, 0 pe „Square" — pastile, avatare, benzi de rând, swatch-uri
    // urmează aceeași formă ca bifele/cardurile din QSS.
    bool squareCorners() const { return app_.squareCorners(); }
    qreal corner(qreal natural) const { return squareCorners() ? 0.0 : natural; }

    // %TOKEN% → value, for the placeholders of res/theme.qss.
    QHash<QString, QString> qssTokens() const;
    QString generateQss() const;
    void apply();

signals:
    void changed();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    ThemeManager();

    Appearance app_;
    bool systemDarkCache_ = true;
    QAbstractNativeEventFilter* nativeFilter_ = nullptr;
};

// Accent presets (Settings → Appearance) — teal MEmu first (default).
inline const char* const kAccentPresets[] = {
    "#2BBD8E", // teal MEmu Multi (default)
    "#6C8CFF", // indigo
    "#38BDF8", // ocean
    "#34D399", // emerald
    "#FBBF24", // amber
    "#FB7185", // rose
    "#A78BFA", // violet
};

} // namespace app::ui::theme
