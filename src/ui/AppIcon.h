#pragma once

class QByteArray;
class QColor;
class QImage;

namespace app::ui {

// The application icon: the brand mark (the accent dot of the account pages)
// in the Settings → Appearance accent — a disc on the rounded corner settings,
// a square on "Square", like the accent swatches and the profile avatar.
// Every size is painted on its own pixel grid, never scaled.

// The mark at `size` × `size` px, transparent around it.
QImage renderAppIcon(int size, const QColor& accent, bool square);

// The mark as a Windows .ico (16…256 px): the exe's own icon, written at build
// time by make-app-icon (src/tools/MakeAppIcon.cpp) and embedded by app.rc.
QByteArray appIconIco(const QColor& accent, bool square);

// The mark as the application's window icon — title bar, taskbar, Alt+Tab,
// every window and dialog. ThemeManager::apply() calls it, so the icon follows
// each appearance change; a no-op while the same mark is already set.
void setAppIcon(const QColor& accent, bool square);

} // namespace app::ui
