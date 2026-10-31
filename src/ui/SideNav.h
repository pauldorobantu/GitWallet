#pragma once

#include <QList>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace app::ui {

// MEmu-style sidebar: expanded (196px) with icon+label items (airy
// icon→text gap); collapsible (Ctrl+M / hamburger) to an 84px icon-only rail.
// Flat list, no sub-menus (owner: "they are not correct") — the child
// sections live as tabs in their respective pages.
class SideNav : public QWidget
{
    Q_OBJECT
public:
    struct Child // kept for API compatibility — ignored (sub-menus
    {            // were removed; child navigation is done from pages)
        QString title;
        int subIndex = 0;
    };

    // The item column's width at rest when expanded (kSideWidth minus the 10px
    // root margins on each side): the footer's / an item's width.
    static constexpr int kExpandedInnerWidth = 168;
    // Item metrics (also asserted by --smoke-nav).
    static constexpr int kItemHeight = 38;    // expanded row height
    static constexpr int kExpandedGlyph = 19; // icon size, expanded
    static constexpr int kRailGlyph = 24;     // icon size, collapsed rail
    static constexpr int kIconTextGap = 12;   // icon → label spacing

    explicit SideNav(QWidget* parent = nullptr);

    void addSection(const QString& title); // category label (hidden on the rail)
    // Navigation item; children ignored (the tabs live in the pages).
    void addItem(int pageId, const QString& title, int iconKind,
                 const QList<Child>& children = {});

    void select(int pageId, int subIndex = -1);

    // Collapse = toggles expanded (196px, labels) ↔ icon-only rail (84px).
    void setCollapsed(bool collapsed, bool animate = true);
    bool isCollapsed() const { return collapsed_; }

    // A widget pinned to the bottom of the sidebar, below the item list (the
    // profile section). The sidebar takes ownership and keeps the widget's
    // "navCollapsed" property in step with its own mode — switched together
    // with the nav items, at the start of the slide animation. Replaces (and
    // deletes) a previous footer.
    void setFooter(QWidget* footer);
    QWidget* footer() const { return footer_; }

signals:
    void navigate(int pageId, int subIndex);
    void collapseChanged(bool collapsed);
    void toggleRequested(); // the collapse button in the sidebar header

private:
    class NavButton;
    struct Entry
    {
        int pageId = -1;
        NavButton* button = nullptr;
        QString title;
    };

    void applyCollapsed(bool collapsed, bool animate);
    void showFloatingLabel(const QString& text, NavButton* nearButton);

    QVBoxLayout* rootLayout_ = nullptr;
    QVBoxLayout* menuLayout_ = nullptr;
    QWidget* footerHost_ = nullptr; // #navFooter: the rule + the footer widget
    QWidget* footer_ = nullptr;
    QHBoxLayout* headerLayout_ = nullptr; // rândul hamburger-ului (margine stânga per mod)
    QToolButton* toggleBtn_ = nullptr;
    QLabel* floating_ = nullptr;
    QTimer* floatingTimer_ = nullptr;
    QList<Entry> entries_;
    bool collapsed_ = false;
};

} // namespace app::ui
