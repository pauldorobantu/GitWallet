#include "ui/SideNav.h"
#include "ui/Icons.h"
#include "ui/ThemeManager.h"

#include <QLabel>
#include <QEasingCurve>
#include <QHBoxLayout>
#include <QPainter>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace app::ui {

namespace {
constexpr int kRailWidth = 76;   // colapsat (icon-only, butoane 50px)
constexpr int kSideWidth = 188;  // extins (icon + etichetă)
constexpr int kFooterGap = 8;    // list → rule → footer (8px grid)
constexpr int kRailItemSize = 50; // rail items + footer: the 50px square column

QColor iconColor(bool active)
{
    auto* theme = theme::ThemeManager::instance();
    return active ? theme->color(QStringLiteral("accent"))
                  : theme->color(QStringLiteral("textSoft"));
}
} // namespace

static_assert(SideNav::kExpandedInnerWidth == kSideWidth - 20,
              "keep in sync with the root layout's side margins");

// Navigation item: expanded = 20px icon + label; collapsed = centered 24px icon.
class SideNav::NavButton : public QToolButton
{
    Q_OBJECT
public:
    NavButton(const QString& title, int iconKind, QWidget* parent)
        : QToolButton(parent)
        , iconKind_(iconKind)
    {
        setText(title);
        setCursor(Qt::PointingHandCursor);
        setToolTip(title);
        setObjectName(QStringLiteral("navItem"));
        setFixedHeight(SideNav::kItemHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMode(false);
    }

    void setMode(bool collapsed)
    {
        collapsed_ = collapsed;
        // On the rail the item is a centered square with a dominant icon; expanded fills the width.
        if (collapsed) {
            setToolButtonStyle(Qt::ToolButtonIconOnly);
            setIconSize(QSize(SideNav::kRailGlyph, SideNav::kRailGlyph));
            setFixedSize(kRailItemSize, kRailItemSize);
        } else {
            setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            setIconSize(QSize(SideNav::kExpandedGlyph, SideNav::kExpandedGlyph));
            setMinimumSize(0, SideNav::kItemHeight);
            setMaximumSize(QWIDGETSIZE_MAX, SideNav::kItemHeight);
        }
        refreshIcon();
    }

    void refreshIcon()
    {
        // Icon line curat, fără plăcuța duotone din spate (owner) — ca în MEmu.
        const int glyph = collapsed_ ? SideNav::kRailGlyph : SideNav::kExpandedGlyph;
        const QIcon src = makeIcon(static_cast<icon::Kind>(iconKind_), iconColor(active_),
                                   glyph, 1.7);
        if (collapsed_) {
            setIcon(src);
            setIconSize(QSize(glyph, glyph));
            return;
        }
        // QToolButton does not expose icon-text spacing via QSS; we extend the pixmap
        // with a transparent right padding → the label starts kIconTextGap
        // further to the right, keeping the existing QSS style.
        const QPixmap pm = src.pixmap(QSize(glyph, glyph));
        const qreal dpr = pm.devicePixelRatio();
        QPixmap canvas(qRound((glyph + SideNav::kIconTextGap) * dpr), qRound(glyph * dpr));
        canvas.setDevicePixelRatio(dpr);
        canvas.fill(Qt::transparent);
        QPainter p(&canvas);
        p.drawPixmap(0, 0, pm);
        p.end();
        setIcon(QIcon(canvas));
        setIconSize(QSize(glyph + SideNav::kIconTextGap, glyph));
    }

    void setActive(bool active)
    {
        active_ = active;
        setProperty("active", active);
        refreshIcon();
        style()->unpolish(this);
        style()->polish(this);
        update();
    }

protected:
    // Itemul activ e marcat prin fundalul tentat + textul în accent (QSS
    // #navItem[active="true"]); fără bară verticală în fața textului (cerință owner).
    void paintEvent(QPaintEvent* event) override
    {
        QToolButton::paintEvent(event);
    }

private:
    int iconKind_;
    bool active_ = false;
    bool collapsed_ = false;
};

SideNav::SideNav(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("sideNav"));
    // Subclasă QWidget: fără atribut, fundalul din QSS (#sideNav) nu se
    // pictează și se vede #appRoot (culoarea ramei) prin sidebar.
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(kSideWidth);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout_ = rootLayout;
    // Sus 0: rândul-cap (38px) stă pe aceeași linie cu controalele barei de titlu.
    rootLayout->setContentsMargins(10, 0, 10, 8);
    rootLayout->setSpacing(0);

    // The sidebar header (Claude desktop style): a row the height of the title
    // bar with the collapse (hamburger) button on the left. Aligns the first menu
    // entry below the control-bar line and puts the collapse button in its
    // place, at the head of the menu — not floating over the content.
    auto* headerRow = new QWidget(this);
    headerRow->setFixedHeight(38);
    auto* headerLayout = new QHBoxLayout(headerRow);
    // Extins: 6px → hamburger-ul în coloana iconițelor; rail: 8 (vezi applyCollapsed).
    headerLayout->setContentsMargins(6, 0, 0, 0);
    headerLayout->setSpacing(0);
    headerLayout_ = headerLayout;
    auto* toggleBtn = new QToolButton(headerRow);
    toggleBtn->setObjectName(QStringLiteral("navToggle"));
    toggleBtn->setCursor(Qt::PointingHandCursor);
    toggleBtn->setAutoRaise(true);
    toggleBtn->setFixedSize(34, 34);
    toggleBtn->setIconSize(QSize(18, 18));
    toggleBtn->setIcon(makeIcon(icon::Kind::Menu,
                                theme::ThemeManager::instance()->color(
                                    QStringLiteral("textSoft")),
                                18, 1.8));
    toggleBtn->setToolTip(QStringLiteral("Collapse / expand menu (Ctrl+M)"));
    connect(toggleBtn, &QToolButton::clicked, this, &SideNav::toggleRequested);
    headerLayout->addWidget(toggleBtn, 0, Qt::AlignVCenter);
    headerLayout->addStretch(1);
    rootLayout->addWidget(headerRow);
    rootLayout->addSpacing(14); // primul item rămâne la y=52 (0 + 38 + 14)
    toggleBtn_ = toggleBtn;

    // No brand row (owner: takes up space). The item list starts below the header.
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("navScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* menuHost = new QWidget(scroll);
    menuHost->setObjectName(QStringLiteral("navScrollHost"));
    menuLayout_ = new QVBoxLayout(menuHost);
    menuLayout_->setContentsMargins(0, 0, 0, 0);
    menuLayout_->setSpacing(1); // compact rows, no wasted space
    menuLayout_->addStretch(1);
    scroll->setWidget(menuHost);
    rootLayout->addWidget(scroll, 1);

    // Floating label: the page name next to the icon, only in rail mode.
    QWidget* host = window() ? window() : static_cast<QWidget*>(this);
    floating_ = new QLabel(host);
    floating_->setObjectName(QStringLiteral("navFloating"));
    floating_->setAttribute(Qt::WA_TransparentForMouseEvents);
    floating_->hide();

    floatingTimer_ = new QTimer(this);
    floatingTimer_->setSingleShot(true);
    connect(floatingTimer_, &QTimer::timeout, this, [this]() { floating_->hide(); });

    // The nav icons follow the theme accent.
    connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed, this,
            [this]() {
                for (Entry& e : entries_)
                    e.button->refreshIcon();
                if (toggleBtn_)
                    toggleBtn_->setIcon(makeIcon(
                        icon::Kind::Menu,
                        theme::ThemeManager::instance()->color(QStringLiteral("textSoft")),
                        18, 1.8));
            });
}

void SideNav::addSection(const QString& title)
{
    Q_UNUSED(title);
    // The sections (OPERATE/AUTOMATION/…) were removed (owner: "those menu
    // lines are no longer needed") — flat list, uniform rows, without
    // large gaps between groups. The menu keeps the item width and size.
}

void SideNav::addItem(int pageId, const QString& title, int iconKind,
                      const QList<Child>& children)
{
    Q_UNUSED(children); // sub-menus removed (owner) — the child tabs
    // live in their respective pages; the sidebar stays a flat list.
    entries_.append(Entry{});
    const int idx = entries_.size() - 1;
    Entry& e = entries_[idx];
    e.pageId = pageId;
    e.title = title;
    e.button = new NavButton(title, iconKind, this);
    e.button->setMode(collapsed_);
    e.button->setActive(false);
    menuLayout_->insertWidget(menuLayout_->count() - 1, e.button);

    connect(e.button, &QToolButton::clicked, this, [this, pageId, idx]() {
        for (int i = 0; i < entries_.size(); ++i)
            entries_[i].button->setActive(i == idx);
        if (collapsed_)
            showFloatingLabel(entries_[idx].title, entries_[idx].button);
        emit navigate(pageId, -1);
    });
}

void SideNav::select(int pageId, int subIndex)
{
    Q_UNUSED(subIndex); // child navigation is done from the page's tabs
    for (int i = 0; i < entries_.size(); ++i)
        entries_[i].button->setActive(entries_[i].pageId == pageId);
}

void SideNav::setCollapsed(bool collapsed, bool animate)
{
    if (collapsed_ == collapsed)
        return;
    applyCollapsed(collapsed, animate);
}

void SideNav::applyCollapsed(bool collapsed, bool animate)
{
    collapsed_ = collapsed;

    for (Entry& e : entries_)
        e.button->setMode(collapsed);
    if (footer_)
        footer_->setProperty("navCollapsed", collapsed);
    if (footerHost_) // the hairline spans the item column, not the whole rail
        footerHost_->setMaximumWidth(collapsed ? kRailItemSize : QWIDGETSIZE_MAX);

    // Rail: 8px centrează toggle-ul (34px) peste itemii de 50px.
    if (headerLayout_)
        headerLayout_->setContentsMargins(collapsed ? 8 : 6, 0, 0, 0);

    const int from = width();
    const int to = collapsed ? kRailWidth : kSideWidth;
    if (!animate || !theme::ThemeManager::instance()->animationsEnabled()) {
        setFixedWidth(to);
        emit collapseChanged(collapsed);
        return;
    }

    // Smooth width slide; the items have already switched mode.
    auto* anim = new QPropertyAnimation(this, "minimumWidth", this);
    anim->setDuration(160);
    anim->setStartValue(from);
    anim->setEndValue(to);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim, &QPropertyAnimation::valueChanged, this, [this](const QVariant& v) {
        setFixedWidth(v.toInt());
    });
    connect(anim, &QPropertyAnimation::finished, this, [this, collapsed]() {
        emit collapseChanged(collapsed);
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void SideNav::setFooter(QWidget* footer)
{
    if (footer == footer_)
        return;
    delete footer_;
    footer_ = footer;
    if (!footer) {
        if (footerHost_)
            footerHost_->hide(); // no rule without a footer under it
        return;
    }
    if (!footerHost_) {
        // Below the list's stretch, on the items' margins; a hairline on top
        // (QSS #navFooter) separates it from the scrolling list.
        footerHost_ = new QWidget(this);
        footerHost_->setObjectName(QStringLiteral("navFooter"));
        footerHost_->setAttribute(Qt::WA_StyledBackground, true);
        auto* layout = new QVBoxLayout(footerHost_);
        layout->setContentsMargins(0, kFooterGap, 0, 0);
        layout->setSpacing(0);
        rootLayout_->addSpacing(kFooterGap);
        rootLayout_->addWidget(footerHost_);
    }
    footer->setParent(footerHost_);
    footerHost_->layout()->addWidget(footer);
    footer->setProperty("navCollapsed", collapsed_);
    footer->show();
    footerHost_->setMaximumWidth(collapsed_ ? kRailItemSize : QWIDGETSIZE_MAX);
    footerHost_->show();
}

void SideNav::showFloatingLabel(const QString& text, NavButton* nearButton)
{
    if (!floating_)
        return;
    floating_->setText(text);
    floating_->adjustSize();
    // Coordinates in the host widget (the main window): to the right of the rail,
    // vertically centered with the icon.
    QWidget* host = floating_->parentWidget();
    const QPoint btnInHost = nearButton->mapTo(host, QPoint(0, 0));
    const int x = mapTo(host, QPoint(width(), 0)).x() + 8;
    const int y = btnInHost.y() + (nearButton->height() - floating_->height()) / 2;
    floating_->move(x, y);
    floating_->show();
    floating_->raise();
    floatingTimer_->start(1600);
}

} // namespace app::ui

#include "SideNav.moc"
