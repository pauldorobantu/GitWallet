#include "ui/TitleBar.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/ThemeManager.h"
#include "core/SettingsRepo.h"

#include <QAction>
#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QVector>

namespace {
// Cele 5 limbi suportate (fără română) — cod + nume nativ.
struct Lang { const char* code; const char* name; };
const Lang kLangs[] = {
    { "en", "English" }, { "es", "Español" }, { "fr", "Français" },
    { "de", "Deutsch" }, { "ru", "Русский" },
};
} // namespace

namespace app::ui {

namespace {

QIcon tbIcon(icon::Kind kind, const QColor& color)
{
    // The captions (minimize/maximize/restore/close) use the vector-drawn
    // Win11 glyphs — Lucide's "Maximize"=expand arrows and
    // "Restore"=copy are wrong for window buttons.
    switch (kind) {
    case icon::Kind::Minimize:
    case icon::Kind::Maximize:
    case icon::Kind::Restore:
    case icon::Kind::Close:
        return makeCaptionIcon(kind, color, 16);
    default:
        return makeIcon(kind, color, 16, 1.5);
    }
}

// The bell, with how many wait on it.
QIcon bellIcon(const QColor& color, const QColor& accent, int unread, qreal dpr)
{
    constexpr int kSide = 22;
    QPixmap out(qRound(kSide * dpr), qRound(kSide * dpr));
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    makeIcon(icon::Kind::Bell, color, 17, 1.8).paint(&p, QRect(2, 4, 17, 17));
    if (unread > 0) {
        constexpr qreal kBadge = 13.0;
        const QRectF badge(kSide - kBadge, 0.0, kBadge, kBadge);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawEllipse(badge);
        QFont font = p.font();
        font.setPixelSize(9);
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(0xFF, 0xFF, 0xFF));
        p.drawText(badge, Qt::AlignCenter, unread > 9 ? QStringLiteral("9+") : QString::number(unread));
    }
    p.end();
    return QIcon(out);
}

QPixmap statusDotPixmap(const QColor& color, qreal dpr)
{
    const int d = 8;
    QPixmap pm(qRound(d * dpr), qRound(d * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(0.5, 0.5, d - 1, d - 1));
    p.end();
    return pm;
}

} // namespace

TitleBar::TitleBar(QWidget* window, QWidget* parent)
    : QWidget(parent)
    , window_(window)
{
    setObjectName(QStringLiteral("titleBar"));
    // Fundalul #titleBar din QSS se pictează doar cu atributul ăsta (altfel
    // bara arată culoarea ramei #appRoot de dedesubt).
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(38);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The collapse hamburger now lives at the top of the sidebar (Claude
    // desktop style); the title bar stays a drag zone + status + window controls.
    layout->addStretch(1);

    // Selector limbă (5 limbi, fără română) — buton cu codul curent + popup.
    langBtn_ = new QToolButton(this);
    langBtn_->setObjectName(QStringLiteral("tbLang"));
    langBtn_->setCursor(Qt::PointingHandCursor);
    langBtn_->setPopupMode(QToolButton::InstantPopup);
    langBtn_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    langBtn_->setFixedHeight(34); // aceeași formă de hover ca themeBtn_ (34px)
    {
        auto* menu = new QMenu(langBtn_);
        for (const Lang& l : kLangs) {
            QAction* a = menu->addAction(QString::fromUtf8(l.name));
            a->setData(QString::fromLatin1(l.code));
            connect(a, &QAction::triggered, this, [this, a]() {
                const QString code = a->data().toString();
                SettingsRepo().set(QStringLiteral("ui.language"), code);
                emit languageSelected(code);
                refreshLangButton();
            });
        }
        langBtn_->setMenu(menu);
    }
    langBtn_->setToolTip(QStringLiteral("Interface language"));
    refreshLangButton();
    // The language can also change from Settings → Application.
    connect(LanguageManager::instance(), &LanguageManager::changed, this,
            [this]() { refreshLangButton(); });
    layout->addWidget(langBtn_, 0, Qt::AlignVCenter);

    // Comutator temă light/dark.
    themeBtn_ = new QToolButton(this);
    themeBtn_->setObjectName(QStringLiteral("tbTheme"));
    themeBtn_->setCursor(Qt::PointingHandCursor);
    themeBtn_->setFixedSize(34, 34);
    themeBtn_->setIconSize(QSize(17, 17));
    refreshThemeIcon();
    connect(themeBtn_, &QToolButton::clicked, this, []() {
        auto* tm = theme::ThemeManager::instance();
        auto app = tm->appearance();
        app.theme = tm->isDark() ? QStringLiteral("light") : QStringLiteral("dark");
        tm->updateAppearance(app); // salvează + aplică instant
    });
    connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed, this,
            [this]() { refreshThemeIcon(); });
    layout->addWidget(themeBtn_, 0, Qt::AlignVCenter);

    // Settings + About — utilitățile stau în bara de sus, nu în sidebar.
    const auto makeUtilityBtn = [this](const QString& tip) {
        auto* btn = new QToolButton(this);
        btn->setObjectName(QStringLiteral("tbTheme"));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedSize(34, 34);
        btn->setIconSize(QSize(17, 17));
        btn->setToolTip(tip);
        return btn;
    };
    bellBtn_ = makeUtilityBtn(QString());
    bellBtn_->setObjectName(QStringLiteral("tbBell"));
    bellBtn_->setIconSize(QSize(22, 22));
    lockBtn_ = makeUtilityBtn(QString());
    lockBtn_->setObjectName(QStringLiteral("tbLock"));
    lockBtn_->hide();
    refreshBtn_ = makeUtilityBtn(QString());
    refreshBtn_->setObjectName(QStringLiteral("tbRefresh"));
    refreshBtn_->hide();
    refreshUtilityIcons();
    connect(bellBtn_, &QToolButton::clicked, this, &TitleBar::paymentsRequested);
    connect(lockBtn_, &QToolButton::clicked, this, &TitleBar::lockRequested);
    connect(LanguageManager::instance(), &LanguageManager::changed, this, [this]() { refreshUtilityIcons(); });
    connect(refreshBtn_, &QToolButton::clicked, this, &TitleBar::refreshRequested);
    connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed, this,
            [this]() { refreshUtilityIcons(); });
    layout->addWidget(bellBtn_, 0, Qt::AlignVCenter);
    layout->addWidget(lockBtn_, 0, Qt::AlignVCenter);
    layout->addSpacing(8);

    // Status live: chip pill cu punct colorat + text (page · running · proxy).
    statusChip_ = new QWidget(this);
    statusChip_->setObjectName(QStringLiteral("tbStatusChip"));
    auto* chipLayout = new QHBoxLayout(statusChip_);
    chipLayout->setContentsMargins(10, 4, 12, 4);
    chipLayout->setSpacing(7);
    statusDot_ = new QLabel(statusChip_);
    statusDot_->setObjectName(QStringLiteral("tbStatusDot"));
    statusText_ = new QLabel(statusChip_);
    statusText_->setObjectName(QStringLiteral("tbStatusText"));
    chipLayout->addWidget(statusDot_, 0, Qt::AlignVCenter);
    chipLayout->addWidget(statusText_, 0, Qt::AlignVCenter);
    statusChip_->hide();
    statusChip_->setCursor(Qt::PointingHandCursor);
    statusChip_->installEventFilter(this);

    layout->addSpacing(10);
    layout->addWidget(statusChip_, 0, Qt::AlignVCenter);
    layout->addWidget(refreshBtn_, 0, Qt::AlignVCenter);
    layout->addSpacing(6);

    auto* minBtn = makeWindowButton(static_cast<int>(icon::Kind::Minimize), "tbMin");
    auto* maxBtn = makeWindowButton(static_cast<int>(icon::Kind::Maximize), "tbMax");
    auto* closeBtn = makeWindowButton(static_cast<int>(icon::Kind::Close), "tbClose");
    minBtn->setToolTip(QStringLiteral("Minimize"));
    maxBtn->setToolTip(QStringLiteral("Maximize / Restore"));
    closeBtn->setToolTip(QStringLiteral("Close"));
    // The button glyph tracks the window state: maximized → Restore.
    maxBtn_ = maxBtn;
    window_->installEventFilter(this);
    refreshMaxButton();

    layout->addWidget(minBtn);
    layout->addWidget(maxBtn);
    layout->addWidget(closeBtn);

    connect(minBtn, &QToolButton::clicked, this, &TitleBar::minimised);
    connect(maxBtn, &QToolButton::clicked, this, &TitleBar::maximised);
    connect(closeBtn, &QToolButton::clicked, this, &TitleBar::closed);
}

void TitleBar::setUtilityButtonsVisible(bool on)
{
    utilityVisible_ = on;
    bellBtn_->setVisible(on);
    refreshBtn_->setVisible(on && refreshAvailable_);
    lockBtn_->setVisible(on && lockAvailable_);
}

void TitleBar::setRefreshAvailable(bool available)
{
    refreshAvailable_ = available;
    refreshBtn_->setVisible(utilityVisible_ && available);
}

void TitleBar::setRefreshEnabled(bool enabled)
{
    refreshBtn_->setEnabled(enabled);
}

void TitleBar::setUnreadPayments(int unread)
{
    unreadPayments_ = unread;
    refreshUtilityIcons();
}

void TitleBar::setLockAvailable(bool available)
{
    lockAvailable_ = available;
    lockBtn_->setVisible(utilityVisible_ && available);
}

void TitleBar::setStatus(const QString& lead, const QString& whole, const QColor& dot)
{
    if (!statusChip_ || !statusText_)
        return;
    if (lead.trimmed().isEmpty()) {
        statusChip_->hide();
        return;
    }
    statusText_->setText(lead);
    statusChip_->setToolTip(whole);
    statusDot_->setPixmap(statusDotPixmap(dot, devicePixelRatioF()));
    statusChip_->show();
}

void TitleBar::refreshThemeIcon()
{
    if (!themeBtn_)
        return;
    const bool dark = theme::ThemeManager::instance()->isDark();
    const QColor c = theme::ThemeManager::instance()->color(QStringLiteral("textSoft"));
    // Dark → arată soarele (comută spre light); light → luna.
    themeBtn_->setIcon(makeIcon(dark ? icon::Kind::Sun : icon::Kind::Moon, c, 17, 1.8));
    themeBtn_->setToolTip(dark ? QStringLiteral("Switch to light theme")
                               : QStringLiteral("Switch to dark theme"));
}

void TitleBar::refreshUtilityIcons()
{
    const QColor c = theme::ThemeManager::instance()->color(QStringLiteral("textSoft"));
    if (refreshBtn_) {
        refreshBtn_->setIcon(makeIcon(icon::Kind::Refresh, c, 17, 1.8));
        refreshBtn_->setToolTip(trText(QStringLiteral("Fetch prices and read the balances that are not followed live, now (F5)")));
    }
    if (bellBtn_) {
        bellBtn_->setIcon(bellIcon(c, theme::ThemeManager::instance()->color(QStringLiteral("accent")), unreadPayments_, devicePixelRatioF()));
        bellBtn_->setToolTip(unreadPayments_ > 0 ? trText(QStringLiteral("Incoming payments: %1 not looked at yet")).arg(unreadPayments_) : trText(QStringLiteral("Incoming payments")));
    }
    if (lockBtn_) {
        lockBtn_->setIcon(makeIcon(icon::Kind::Lock, c, 17, 1.8));
        lockBtn_->setToolTip(trText(QStringLiteral("Lock GitWallet")));
    }
}

void TitleBar::refreshLangButton()
{
    if (!langBtn_)
        return;
    QString code = SettingsRepo().get(QStringLiteral("ui.language"));
    if (code.isEmpty())
        code = QStringLiteral("en");
    langBtn_->setText(code.left(2).toUpper());
}

QToolButton* TitleBar::makeWindowButton(int iconKind, const char* objectName)
{
    auto* btn = new QToolButton(this);
    const auto kind = static_cast<icon::Kind>(iconKind);
    const bool isClose = kind == icon::Kind::Close;
    const auto baseIcon = [kind](const QColor& c) {
        return tbIcon(kind, c);
    };
    btn->setIcon(baseIcon(theme::ThemeManager::instance()->color(
        QStringLiteral("textSoft"))));
    btn->setIconSize(QSize(16, 16));
    btn->setFixedSize(38, 38); // square, full bar height (Win11 style)
    btn->setCursor(Qt::ArrowCursor);
    btn->setObjectName(QString::fromLatin1(objectName));

    // Close (Win11): on hover, red background + white glyph — the icon switches
    // on enter/leave; the other buttons keep their glyph.
    if (isClose) {
        const QIcon white = baseIcon(QColor(0xFF, 0xFF, 0xFF));
        btn->installEventFilter(this);
        closeButtons_.append({ btn, white });
    }

    connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed, btn,
            [this, btn, kind, baseIcon, isClose]() {
                if (isClose && btn->underMouse())
                    return;
                if (btn == maxBtn_) {
                    refreshMaxButton();
                    return;
                }
                btn->setIcon(baseIcon(theme::ThemeManager::instance()->color(
                    QStringLiteral("textSoft"))));
            });
    return btn;
}

void TitleBar::refreshMaxButton()
{
    if (!maxBtn_)
        return;
    const bool maximized = window_ && window_->isMaximized();
    maxBtn_->setIcon(tbIcon(maximized ? icon::Kind::Restore : icon::Kind::Maximize,
                            theme::ThemeManager::instance()->color(
                                QStringLiteral("textSoft"))));
    maxBtn_->setToolTip(maximized ? QStringLiteral("Restore")
                                  : QStringLiteral("Maximize / Restore"));
}

bool TitleBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == statusChip_ && event->type() == QEvent::MouseButtonRelease) {
        emit statusRequested();
        return true;
    }
    // Window state change (maximize/restore) → matching glyph.
    if (watched == window_ && event->type() == QEvent::WindowStateChange)
        refreshMaxButton();

    // Close: white icon while the button is under the mouse (the red hover comes from QSS).
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        for (const auto& [btn, whiteIcon] : closeButtons_) {
            if (btn == watched) {
                const bool entering = (event->type() == QEvent::Enter);
                if (entering)
                    btn->setIcon(whiteIcon);
                else
                    btn->setIcon(tbIcon(icon::Kind::Close,
                                        theme::ThemeManager::instance()->color(
                                            QStringLiteral("textSoft"))));
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TitleBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragOrigin_ = event->globalPosition().toPoint() - window_->frameGeometry().topLeft();
    }
    QWidget::mousePressEvent(event);
}

void TitleBar::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        window_->move(event->globalPosition().toPoint() - dragOrigin_);
    }
    QWidget::mouseMoveEvent(event);
}

void TitleBar::mouseReleaseEvent(QMouseEvent* event)
{
    dragging_ = false;
    QWidget::mouseReleaseEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        emit maximised();
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace app::ui
