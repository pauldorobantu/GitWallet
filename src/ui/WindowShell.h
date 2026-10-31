#pragma once

#include <QRect>
#include <QtGlobal>

class QByteArray;
class QLayout;
class QWidget;

namespace app::ui {

// The application window's shell, shared by MainWindow and the sign-in window
// (LoginDialog) so the two read as the same window: frameless, the same size
// and minimum size, a 1px frame and resizing from the edges.

// Where a shell window is: its restored rectangle and whether it is maximised.
struct ShellPlacement
{
    QRect normalGeometry;
    bool maximised = false;
};

// Frameless top-level window at the shell's default / minimum size
// (page::kWindowSize / page::kWindowMinSize).
void initShellWindow(QWidget* window);

ShellPlacement shellPlacement(const QWidget* window);

// Puts `window` where `placement` says (restored rectangle + maximised state).
// A hidden window takes the state when shown. No-op for an empty placement.
void applyShellPlacement(QWidget* window, const ShellPlacement& placement);

// The 1px frame on all four sides; none when maximised / full screen.
void updateShellFrame(const QWidget* window, QLayout* root);

// Edge / corner resizing for the frameless window: answers WM_NCHITTEST with
// HTLEFT/HTTOPRIGHT/... inside a thin band along the border, except when the
// window is maximised / full screen (the band would cover the caption buttons
// at the screen edge). Call first from nativeEvent(); true = handled and
// *result set, false = let the base class answer. Always false off Windows.
bool shellResizeHitTest(const QWidget* window, const QByteArray& eventType, void* message,
                        qintptr* result);

} // namespace app::ui
