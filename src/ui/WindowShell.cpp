#include "ui/WindowShell.h"

#include "ui/PageKit.h"

#include <QByteArray>
#include <QLayout>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#endif

namespace app::ui {

namespace {
constexpr int kResizeBorder = 6;
} // namespace

void initShellWindow(QWidget* window)
{
    window->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    window->resize(page::kWindowSize);
    window->setMinimumSize(page::kWindowMinSize);
}

ShellPlacement shellPlacement(const QWidget* window)
{
    // normalGeometry() is the restored rectangle, also while maximised and
    // after the window was hidden (done() keeps the window state).
    return {window->normalGeometry(), window->isMaximized()};
}

void applyShellPlacement(QWidget* window, const ShellPlacement& placement)
{
    if (!placement.normalGeometry.isValid())
        return;
    const Qt::WindowStates others = window->windowState() & ~Qt::WindowMaximized;
    if (window->isMaximized() && !placement.maximised)
        window->setWindowState(others); // restore first, then move
    // Already maximised and staying so: keep its own restore rectangle.
    if (!window->isMaximized())
        window->setGeometry(placement.normalGeometry);
    if (placement.maximised)
        window->setWindowState(others | Qt::WindowMaximized);
}

void updateShellFrame(const QWidget* window, QLayout* root)
{
    if (!window || !root)
        return;
    const int m = (window->isMaximized() || window->isFullScreen()) ? 0 : 1;
    root->setContentsMargins(m, m, m, m);
}

bool shellResizeHitTest(const QWidget* window, const QByteArray& eventType, void* message,
                        qintptr* result)
{
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG")
        return false;
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg->message != WM_NCHITTEST)
        return false;
    if (window->isMaximized() || window->isFullScreen())
        return false;

    const LONG x = GET_X_LPARAM(msg->lParam);
    const LONG y = GET_Y_LPARAM(msg->lParam);
    RECT rc;
    GetWindowRect(msg->hwnd, &rc);

    const bool left = x >= rc.left && x < rc.left + kResizeBorder;
    const bool right = x < rc.right && x >= rc.right - kResizeBorder;
    const bool top = y >= rc.top && y < rc.top + kResizeBorder;
    const bool bottom = y < rc.bottom && y >= rc.bottom - kResizeBorder;

    LRESULT hit = HTCLIENT;
    if (top && left) hit = HTTOPLEFT;
    else if (top && right) hit = HTTOPRIGHT;
    else if (bottom && left) hit = HTBOTTOMLEFT;
    else if (bottom && right) hit = HTBOTTOMRIGHT;
    else if (left) hit = HTLEFT;
    else if (right) hit = HTRIGHT;
    else if (top) hit = HTTOP;
    else if (bottom) hit = HTBOTTOM;

    if (hit == HTCLIENT)
        return false;
    *result = hit;
    return true;
#else
    Q_UNUSED(window)
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
    return false;
#endif
}

} // namespace app::ui
