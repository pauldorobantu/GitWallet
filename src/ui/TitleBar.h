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

    void setStatus(const QString& text); // "page · N running · proxy :port"
    // Settings / About buttons — hidden on the sign-in window, where there is
    // nothing to configure yet (language + theme stay). The account lives in
    // the sidebar's profile section (ProfileChip).
    void setUtilityButtonsVisible(bool on);

signals:
    void minimised();
    void maximised();
    void closed();
    void menuToggled();
    void languageSelected(const QString& code);
    void settingsRequested();
    void aboutRequested();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QToolButton* makeWindowButton(int iconKind, const char* objectName);
    void refreshStatusDot();
    void refreshMaxButton(); // glyph Maximize ↔ Restore după starea ferestrei
    void refreshThemeIcon(); // sun/moon după tema curentă
    void refreshUtilityIcons(); // Settings / About
    void refreshLangButton(); // codul limbii curente (EN/ES/FR/DE/RU)

    QWidget* window_ = nullptr;
    QWidget* statusChip_ = nullptr;
    QLabel* statusDot_ = nullptr;
    QLabel* statusText_ = nullptr;
    QToolButton* maxBtn_ = nullptr;
    QToolButton* themeBtn_ = nullptr;
    QToolButton* langBtn_ = nullptr;
    QToolButton* settingsBtn_ = nullptr;
    QToolButton* aboutBtn_ = nullptr;
    QVector<QPair<QToolButton*, QIcon>> closeButtons_; // icon alb la hover (Win11)
    bool dragging_ = false;
    QPoint dragOrigin_;
};

} // namespace app::ui
