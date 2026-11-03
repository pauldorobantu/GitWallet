#pragma once

#include <QIcon>
#include <QWidget>
#include <QVector>
#include <QPair>

class QToolButton;
class QLabel;
class QEvent;

namespace app::ui {

// Title bar 38px: hamburger + live status (chip with a green dot when there's
// activity) + Windows 11 control buttons (38×38 squares). Drag to
// move, double-click to maximize.
class TitleBar : public QWidget
{
    Q_OBJECT
public:
    explicit TitleBar(QWidget* window, QWidget* parent = nullptr);

    // A few words, the whole of it as a tooltip, and a dot in the colour of what that means.
    void setStatus(const QString& lead, const QString& whole, const QColor& dot);
    // The bell and Refresh: hidden behind the lock, where nothing of it shows.
    void setUtilityButtonsVisible(bool on);
    void setRefreshAvailable(bool available);
    void setRefreshEnabled(bool enabled);
    // What came in and has not been looked at, on the bell.
    void setUnreadPayments(int unread);
    int unreadPayments() const { return unreadPayments_; }
    // There is a password to lock with.
    void setLockAvailable(bool available);

signals:
    void minimised();
    void maximised();
    void closed();
    void languageSelected(const QString& code);
    void paymentsRequested();
    void lockRequested();
    void statusRequested();
    void refreshRequested();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QToolButton* makeWindowButton(int iconKind, const char* objectName);
    void refreshMaxButton(); // glyph Maximize ↔ Restore după starea ferestrei
    void refreshThemeIcon(); // sun/moon după tema curentă
    void refreshUtilityIcons(); // the bell, the lock and Refresh
    void refreshLangButton(); // codul limbii curente (EN/ES/FR/DE/RU)

    QWidget* window_ = nullptr;
    QWidget* statusChip_ = nullptr;
    QLabel* statusDot_ = nullptr;
    QLabel* statusText_ = nullptr;
    QToolButton* maxBtn_ = nullptr;
    QToolButton* themeBtn_ = nullptr;
    QToolButton* langBtn_ = nullptr;
    QToolButton* refreshBtn_ = nullptr;
    QToolButton* bellBtn_ = nullptr;
    QToolButton* lockBtn_ = nullptr;
    int unreadPayments_ = 0;
    bool lockAvailable_ = false;
    bool refreshAvailable_ = false;
    QColor statusDotColor_;
    bool utilityVisible_ = true;
    QVector<QPair<QToolButton*, QIcon>> closeButtons_; // icon alb la hover (Win11)
    bool dragging_ = false;
    QPoint dragOrigin_;
};

} // namespace app::ui
