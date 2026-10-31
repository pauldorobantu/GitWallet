#include "ui/ThemeManager.h"
#include "ui/AppIcon.h"
#include "ui/DialogChrome.h"
#include "ui/HoverRowTable.h"

#include "core/SettingsRepo.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QEvent>
#include <QFile>
#include <QFont>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QProgressDialog>
#include <QPalette>
#include <QAbstractItemView>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QSettings>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace app::ui::theme {

namespace {

// The stylesheet template (res/theme.qss, in the resources): plain QSS whose
// %TOKEN% placeholders generateQss() fills from the palette and the user's
// appearance. Read once; text mode, so a CRLF checkout reads the same.
const QString& qssTemplate()
{
    static const QString tmpl = [] {
        QFile f(QStringLiteral(":/theme.qss"));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "[theme] cannot open :/theme.qss";
            return QString();
        }
        return QString::fromUtf8(f.readAll());
    }();
    return tmpl;
}

struct Pal
{
    QColor bg, side, surface, surfaceAlt, text, textSoft, muted;
    QColor border, borderStrong, hover;
    QColor success, warning, error, info;
};

const Pal& palette(bool dark)
{
    // Premium palette (dark, warm-NEUTRAL gray — no blue) with clear elevation:
    // bg (deepest) < surface (cards/tables) < surfaceAlt (hover/alt).
    static const Pal darkPal = {
        QColor(0x1B, 0x1A, 0x18),   // bg (app, deepest)
        QColor(0x16, 0x15, 0x13),   // side (sidebar, even deeper)
        QColor(0x23, 0x22, 0x20),   // surface (cards, rows)
        QColor(0x2B, 0x2A, 0x27),   // surfaceAlt (alt-row / hover)
        QColor(0xEC, 0xEA, 0xE6),   // text
        QColor(0xB4, 0xB1, 0xAB),   // textSoft
        QColor(0x96, 0x91, 0x88),   // muted (ridicat pt lizibilitate text secundar)
        QColor(0x30, 0x2E, 0x2B),   // border
        QColor(0x40, 0x3D, 0x39),   // borderStrong
        QColor(255, 255, 255, 14),  // hover
        QColor(0x2B, 0xBD, 0x8E),   // success (teal brand)
        QColor(0xF0, 0xA8, 0x3B),   // warning
        QColor(0xEC, 0x5B, 0x5B),   // error
        QColor(0x5E, 0xA8, 0xD8),   // info
    };
    // Light WARM-neutru (fara albastru - pereche calda a dark-ului), contrast ridicat.
    static const Pal lightPal = {
        QColor(0xF3, 0xF1, 0xED),   // bg (gri cald deschis)
        QColor(0xFB, 0xFA, 0xF7),   // side (aproape alb, cald)
        QColor(0xFF, 0xFF, 0xFF),   // surface (carduri = alb curat)
        QColor(0xEC, 0xE8, 0xE2),   // surfaceAlt (alt-row / hover cald)
        QColor(0x21, 0x1E, 0x19),   // text (negru cald)
        QColor(0x4C, 0x47, 0x3F),   // textSoft
        QColor(0x79, 0x72, 0x68),   // muted
        QColor(0xE6, 0xE1, 0xD9),   // border
        QColor(0xCE, 0xC7, 0xBC),   // borderStrong
        QColor(28, 22, 14, 18),     // hover (overlay cald închis)
        QColor(0x12, 0x8A, 0x5B),   // success (teal-verde, contrast pe alb)
        QColor(0xB4, 0x53, 0x09),   // warning (ambră/maro)
        QColor(0xC9, 0x2A, 0x2A),   // error
        QColor(0x3F, 0x6B, 0x80),   // info (slate, nu albastru pur)
    };
    return dark ? darkPal : lightPal;
}

// WM_SETTINGCHANGE (ImmersiveColorSet) → the system theme changed.
class ThemeNativeFilter : public QAbstractNativeEventFilter
{
public:
    bool nativeEventFilter(const QByteArray& eventType, void* message,
                            qintptr*) override
    {
#ifdef Q_OS_WIN
        if (eventType != "windows_generic_MSG")
            return false;
        const MSG* msg = static_cast<MSG*>(message);
        if (msg->message == WM_SETTINGCHANGE
            && msg->lParam != 0
            && qstrcmp(reinterpret_cast<const char*>(msg->lParam),
                       "ImmersiveColorSet") == 0) {
            // Re-apply (fires the changed signal if isDark() flipped).
            QTimer::singleShot(200, ThemeManager::instance(), []() {
                ThemeManager::instance()->apply();
            });
        }
#else
        Q_UNUSED(eventType);
        Q_UNUSED(message);
#endif
        return false;
    }
};

// Tabelele simple (fără HoverRowTable): zebra/hover/selecția ca benzi pe tot
// rândul, pictate pe viewport ÎNAINTE de celule — aceeași nuanță uniformă ca
// la HoverRow (celulele sunt transparente în QSS).
class RowBandPainter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        if (event->type() == QEvent::Paint) {
            auto* vp = static_cast<QWidget*>(obj);
            if (auto* view = qobject_cast<QTableView*>(vp->parentWidget())) {
                // Hover pe rând doar la selecție pe rânduri: atunci Qt
                // re-pictează tot rândul când se mută mouse-ul.
                int hoverRow = -1;
                if (view->selectionBehavior() == QAbstractItemView::SelectRows
                    && vp->underMouse())
                    hoverRow = view->indexAt(vp->mapFromGlobal(QCursor::pos())).row();
                QPainter p(vp);
                ui::detail::paintRowStrips(view, p, hoverRow);
            }
        }
        return QObject::eventFilter(obj, event);
    }
};

// Stilul nativ Windows 11 își pictează propria selecție în FIECARE celulă /
// item selectat (un marcaj vertical în culoarea de accent a sistemului —
// roz/lila — plus blocuri rotunjite gri pt selecție/hover). Item-urile ajung
// la stilul nativ FĂRĂ aceste stări:
//  - tabele: zebra/hover/selecția vin din banda de rând pictată dedesubt;
//  - liste/arbori: pictăm aici o nuanță plată, aceeași ca la tabele.
class ItemViewStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt, QPainter* p,
                       const QWidget* w) const override
    {
        if ((pe == PE_PanelItemViewItem || pe == PE_PanelItemViewRow)
            && qobject_cast<const QAbstractItemView*>(w))
            if (auto* vi = qstyleoption_cast<const QStyleOptionViewItem*>(opt)) {
                if (pe == PE_PanelItemViewItem && !qobject_cast<const QTableView*>(w))
                    fillItem(*vi, p, w);
                const QStyleOptionViewItem plain = stripped(*vi, w);
                QProxyStyle::drawPrimitive(pe, &plain, p, w);
                return;
            }
        QProxyStyle::drawPrimitive(pe, opt, p, w);
    }

    void drawControl(ControlElement ce, const QStyleOption* opt, QPainter* p,
                     const QWidget* w) const override
    {
        if (ce == CE_ItemViewItem && qobject_cast<const QAbstractItemView*>(w))
            if (auto* vi = qstyleoption_cast<const QStyleOptionViewItem*>(opt)) {
                // Liste/arbori: fundalul (al nostru) + conținutul nativ fără marcaj.
                if (!qobject_cast<const QTableView*>(w))
                    fillItem(*vi, p, w);
                QStyleOptionViewItem plain = stripped(*vi, w);
                if (!qobject_cast<const QTableView*>(w))
                    plain.backgroundBrush = Qt::NoBrush;
                QProxyStyle::drawControl(ce, &plain, p, w);
                lastWidget_ = nullptr; // item terminat: următoarea pictare umple iar
                return;
            }
        QProxyStyle::drawControl(ce, opt, p, w);
    }

private:
    // O singură umplere per item: stratul QSS cere uneori fundalul separat
    // (PE_PanelItemViewItem) înainte de CE_ItemViewItem pt același item;
    // marcajul se resetează la finalul CE_ItemViewItem.
    void fillItem(const QStyleOptionViewItem& vi, QPainter* p, const QWidget* w) const
    {
        if (w == lastWidget_ && vi.rect == lastRect_ && vi.index == lastIndex_)
            return;
        lastWidget_ = w;
        lastRect_ = vi.rect;
        lastIndex_ = vi.index;
        auto* tm = ThemeManager::instance();
        if (vi.state & State_Selected)
            p->fillRect(vi.rect, tm->color(Color::RowSelection));
        else if (vi.state & State_MouseOver)
            p->fillRect(vi.rect, tm->color(Color::Hover));
    }

    mutable const QWidget* lastWidget_ = nullptr;
    mutable QRect lastRect_;
    mutable QModelIndex lastIndex_; // doar comparat, nu folosit

    static QStyleOptionViewItem stripped(const QStyleOptionViewItem& in, const QWidget* w)
    {
        QStyleOptionViewItem o(in);
        o.state &= ~(State_Selected | State_MouseOver | State_HasFocus);
        if (qobject_cast<const QTableView*>(w))
            o.features &= ~QStyleOptionViewItem::Alternate;
        return o;
    }
};

} // namespace

ThemeManager* ThemeManager::instance()
{
    static ThemeManager mgr;
    return &mgr;
}

ThemeManager::ThemeManager()
{
    // Înainte de primul QSS: stylesheet-ul se pune peste acest stil.
    qApp->setStyle(new ItemViewStyle(QApplication::style()->name()));
    nativeFilter_ = new ThemeNativeFilter();
    qApp->installNativeEventFilter(nativeFilter_);
    qApp->installEventFilter(this); // vezi eventFilter: aliniere antete tabele
}

bool ThemeManager::eventFilter(QObject* obj, QEvent* event)
{
    // Antetele orizontale de tabel stau la stânga, ca datele din celule —
    // centrate (defaultul Qt) păreau decalate față de coloana lor. O coloană
    // care vrea altă aliniere o cere prin model (TextAlignmentRole câștigă).
    if (event->type() == QEvent::Polish) {
        if (auto* hv = qobject_cast<QHeaderView*>(obj);
            hv && hv->orientation() == Qt::Horizontal)
            hv->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        // Grila e transparentă în toată aplicația; lăsată „pornită", rezervă
        // totuși 1px între celule și pe rândurile alternate apar dungi verticale
        // de fundal. Fără grilă, banda rândului e continuă.
        else if (auto* tv = qobject_cast<QTableView*>(obj)) {
            tv->setShowGrid(false);
            // Benzile de rând sub celule (HoverRowTable le pictează singur).
            if (!tv->property("appRowBand").toBool()
                && !tv->property("appRowBandFilter").toBool()) {
                static auto* bands = new RowBandPainter(qApp);
                tv->viewport()->installEventFilter(bands);
                tv->setProperty("appRowBandFilter", true);
            }
        }
    }
    // Mesajele/progresul/input-ul standard Qt: aceeași formă ca fereastra
    // principală (bară de titlu integrată + ramă), ca dialogurile noastre.
    // La primul copil adăugat (încă în constructor, fereastra nativă NU e
    // creată) — la Polish e prea târziu: schimbarea flag-urilor ar recrea-o.
    else if (event->type() == QEvent::ChildAdded) {
        if (auto* dlg = qobject_cast<QDialog*>(obj);
            dlg && dlg->isWindow() && !dlg->property("appChrome").toBool()
            && (qobject_cast<QMessageBox*>(dlg) || qobject_cast<QProgressDialog*>(dlg)
                || qobject_cast<QInputDialog*>(dlg)))
            ui::applyDialogChrome(dlg);
    }
    return QObject::eventFilter(obj, event);
}

bool ThemeManager::isDark() const
{
    if (app_.theme != QLatin1String("system"))
        return app_.theme == QLatin1String("dark");
    // Windows: AppsUseLightTheme (0 = dark).
    QSettings s(QStringLiteral(
                    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                QSettings::NativeFormat);
    return s.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
}

QColor ThemeManager::color(Color role) const
{
    const bool dark = isDark();
    const Pal& p = palette(dark);
    const QColor accent(app_.accent);

    switch (role) {
    case Color::Accent:
        return accent;
    case Color::AccentText:
        // Luminance-based contrast: dark on light accents (amber), else white.
        return (accent.red() * 299 + accent.green() * 587 + accent.blue() * 114) / 1000 > 140
            ? QColor(0x14, 0x16, 0x1A)
            : QColor(0xFF, 0xFF, 0xFF);
    case Color::AccentHover:
        return accent.lighter(dark ? 115 : 92);
    case Color::AccentPressed:
        // Pressed distinct from hover (tactile feedback): the accent darkens below
        // its base, like MEmu buttons (rest #3a4048 → hover #454c56 → pressed #333840).
        return accent.darker(dark ? 112 : 108);
    case Color::Bg:
        return p.bg;
    case Color::Side:
        return p.side;
    case Color::Surface:
        return p.surface;
    case Color::SurfaceAlt:
        return p.surfaceAlt;
    case Color::SurfaceHover:
        return p.surfaceAlt.lighter(118);
    case Color::Text:
        return p.text;
    case Color::TextSoft:
        return p.textSoft;
    case Color::Muted:
        return p.muted;
    case Color::Border:
        return p.border;
    case Color::BorderStrong:
        return app_.border.isEmpty() ? p.borderStrong : QColor(app_.border);
    case Color::Frame: {
        // Rama ferestrei (principală + dialoguri): border custom, altfel accentul
        // amestecat cu fundalul (opac) — fin, nu un neon plin.
        if (!app_.border.isEmpty())
            return QColor(app_.border);
        const qreal mix = dark ? 0.55 : 0.65;
        return QColor::fromRgbF(accent.redF() * mix + p.bg.redF() * (1 - mix),
                                accent.greenF() * mix + p.bg.greenF() * (1 - mix),
                                accent.blueF() * mix + p.bg.blueF() * (1 - mix));
    }
    case Color::Hover:
        return p.hover;
    case Color::Press: {
        QColor c = p.hover;
        c.setAlpha(qMin(255, p.hover.alpha() * 2 + 6));
        return c;
    }
    case Color::Selection:
        return QColor(accent.red(), accent.green(), accent.blue(), 80);
    // Rândul selectat dintr-un tabel: O SINGURĂ nuanță, discretă (fără ramă,
    // fără bară de accent) — pictată de HoverRow / RowBands sub conținut.
    case Color::RowSelection:
        return QColor(accent.red(), accent.green(), accent.blue(), dark ? 31 : 26);
    // Zebra discretă: la jumătatea drumului surface → surfaceAlt (surfaceAlt
    // plin, folosit și pt input-uri, făcea dungi prea apăsate pe light).
    case Color::Zebra:
        return QColor((p.surface.red() + p.surfaceAlt.red()) / 2,
                      (p.surface.green() + p.surfaceAlt.green()) / 2,
                      (p.surface.blue() + p.surfaceAlt.blue()) / 2);
    case Color::Success:
        return p.success;
    case Color::Warning:
        return p.warning;
    case Color::Error:
        return p.error;
    case Color::Info:
        return p.info;
    }
    return p.textSoft;
}

QColor ThemeManager::color(const QString& role) const
{
    static const QHash<QString, Color> byName = {
        { QStringLiteral("accent"), Color::Accent },
        { QStringLiteral("accentText"), Color::AccentText },
        { QStringLiteral("accentHover"), Color::AccentHover },
        { QStringLiteral("accentPressed"), Color::AccentPressed },
        { QStringLiteral("bg"), Color::Bg },
        { QStringLiteral("side"), Color::Side },
        { QStringLiteral("surface"), Color::Surface },
        { QStringLiteral("surfaceAlt"), Color::SurfaceAlt },
        { QStringLiteral("surfaceHover"), Color::SurfaceHover },
        { QStringLiteral("text"), Color::Text },
        { QStringLiteral("textSoft"), Color::TextSoft },
        { QStringLiteral("muted"), Color::Muted },
        { QStringLiteral("border"), Color::Border },
        { QStringLiteral("borderStrong"), Color::BorderStrong },
        { QStringLiteral("frame"), Color::Frame },
        { QStringLiteral("hover"), Color::Hover },
        { QStringLiteral("press"), Color::Press },
        { QStringLiteral("selection"), Color::Selection },
        { QStringLiteral("rowSelection"), Color::RowSelection },
        { QStringLiteral("zebra"), Color::Zebra },
        { QStringLiteral("success"), Color::Success },
        { QStringLiteral("warning"), Color::Warning },
        { QStringLiteral("error"), Color::Error },
        { QStringLiteral("info"), Color::Info },
    };
    return color(byName.value(role, Color::TextSoft));
}

void ThemeManager::loadFromSettings()
{
    SettingsRepo repo;
    // Temă dark SAU light (ambele tokenizate acum). Default dark.
    const QString storedTheme = repo.get(QStringLiteral("appearance.theme"));
    app_.theme = (storedTheme == QLatin1String("light")
                  || storedTheme == QLatin1String("dark"))
        ? storedTheme
        : QStringLiteral("dark");

    const QString accent = repo.get(QStringLiteral("appearance.accent"));
    if (QColor::isValidColor(accent))
        app_.accent = accent;
    // The old indigo default (before the MEmu skin) wasn't an explicit user
    // choice — migrate it to the MEmu teal; the other colors stay.
    if (app_.accent.compare(QLatin1String("#6C8CFF"), Qt::CaseInsensitive) == 0)
        app_.accent = QStringLiteral("#2BBD8E");

    const QString border = repo.get(QStringLiteral("appearance.border"));
    app_.border = QColor::isValidColor(border) ? border : QString();

    bool ok = false;
    const int bw = repo.get(QStringLiteral("appearance.border_width")).toInt(&ok);
    if (ok && bw >= 0 && bw <= 2)
        app_.borderWidth = bw;
    const int r = repo.get(QStringLiteral("appearance.radius")).toInt(&ok);
    if (ok && r >= 0 && r <= 12)
        app_.radius = r;
    const int fd = repo.get(QStringLiteral("appearance.font_delta")).toInt(&ok);
    if (ok && fd >= -2 && fd <= 4)
        app_.fontDelta = fd;
    app_.animations =
        repo.get(QStringLiteral("appearance.animations")) != QLatin1String("0");
    app_.navCollapsed =
        repo.get(QStringLiteral("appearance.nav_collapsed")) == QLatin1String("1");
}

void ThemeManager::saveToSettings() const
{
    SettingsRepo repo;
    repo.set(QStringLiteral("appearance.theme"), app_.theme);
    repo.set(QStringLiteral("appearance.accent"), app_.accent);
    repo.set(QStringLiteral("appearance.border"), app_.border);
    repo.set(QStringLiteral("appearance.border_width"),
             QString::number(app_.borderWidth));
    repo.set(QStringLiteral("appearance.radius"), QString::number(app_.radius));
    repo.set(QStringLiteral("appearance.font_delta"),
             QString::number(app_.fontDelta));
    repo.set(QStringLiteral("appearance.animations"),
             app_.animations ? QStringLiteral("1") : QStringLiteral("0"));
    repo.set(QStringLiteral("appearance.nav_collapsed"),
             app_.navCollapsed ? QStringLiteral("1") : QStringLiteral("0"));
}

void ThemeManager::updateAppearance(const Appearance& app)
{
    // Re-aplicăm stilul (și emitem changed()) DOAR când se schimbă ceva vizual.
    // Altfel un simplu colaps de sidebar (navCollapsed, salvat prin aceeași
    // cale) ar reîncărca fiecare pagină conectată la changed() → flicker/lag.
    const bool visualChanged =
        app.theme != app_.theme || app.accent != app_.accent
        || app.border != app_.border || app.borderWidth != app_.borderWidth
        || app.radius != app_.radius || app.fontDelta != app_.fontDelta
        || app.animations != app_.animations;
    app_ = app;
    saveToSettings();
    if (visualChanged)
        apply();
}

QHash<QString, QString> ThemeManager::qssTokens() const
{
    namespace font = tokens::font;
    const auto rgb = [](const QColor& c) {
        return QStringLiteral("%1,%2,%3").arg(c.red()).arg(c.green()).arg(c.blue());
    };
    const QColor accent = color(Color::Accent);
    const QColor hover = color(Color::Hover);
    const QColor press = color(Color::Press);
    const QColor selection = color(Color::Selection);
    const tokens::Radii r = radii();
    QHash<QString, QString> t;

    // Colours: #rrggbb, or "r,g,b" / "r,g,b,a" inside the template's rgb()/rgba().
    t.insert(QStringLiteral("BG"), color(Color::Bg).name());
    t.insert(QStringLiteral("SIDE"), color(Color::Side).name());
    t.insert(QStringLiteral("SURFACE"), color(Color::Surface).name());
    t.insert(QStringLiteral("SURFACEALT"), color(Color::SurfaceAlt).name());
    t.insert(QStringLiteral("HOVERSURF"), color(Color::SurfaceHover).name());
    t.insert(QStringLiteral("TEXT"), color(Color::Text).name());
    t.insert(QStringLiteral("TEXTSOFT"), color(Color::TextSoft).name());
    t.insert(QStringLiteral("MUTED"), color(Color::Muted).name());
    t.insert(QStringLiteral("MUTEDRGB"), rgb(color(Color::Muted)));
    t.insert(QStringLiteral("BORDER"), color(Color::Border).name());
    t.insert(QStringLiteral("BORDERS"), color(Color::BorderStrong).name());
    t.insert(QStringLiteral("WINDOWFRAME"), color(Color::Frame).name());
    t.insert(QStringLiteral("ACCENT"), accent.name());
    t.insert(QStringLiteral("ACCENTRGB"), rgb(accent));
    t.insert(QStringLiteral("ACCENTHOVER"), color(Color::AccentHover).name());
    t.insert(QStringLiteral("ACCENTPRESSED"), color(Color::AccentPressed).name());
    t.insert(QStringLiteral("ACCENTTEXT"), color(Color::AccentText).name());
    // Selected text and menu / combo items: Color::Selection, the translucent
    // accent of the palette's Highlight too — the text on it stays readable
    // whatever the accent.
    t.insert(QStringLiteral("SEL"),
             QStringLiteral("rgba(%1,%2)")
                 .arg(rgb(selection), QString::number(selection.alpha())));
    t.insert(QStringLiteral("HOVER"),
             QStringLiteral("%1,%2").arg(rgb(hover), QString::number(hover.alpha())));
    t.insert(QStringLiteral("PRESS"),
             QStringLiteral("%1,%2").arg(rgb(press), QString::number(press.alpha())));
    t.insert(QStringLiteral("SUCCESS"), rgb(color(Color::Success)));
    t.insert(QStringLiteral("WARNING"), rgb(color(Color::Warning)));
    t.insert(QStringLiteral("ERROR"), rgb(color(Color::Error)));

    // Tables: one "card" skin — surface over the page, a subtle border, the
    // big radius, no grid.
    t.insert(QStringLiteral("TBBG"), color(Color::Surface).name());
    t.insert(QStringLiteral("TBHEADBG"), color(Color::Surface).name());
    t.insert(QStringLiteral("TBBORDER"), color(Color::Border).name());
    t.insert(QStringLiteral("TBGRID"), QStringLiteral("transparent"));
    t.insert(QStringLiteral("TBRADIUS"), QStringLiteral("%1px").arg(r.big));
    // Cards and group boxes: a neutral 1px line on border width 0, otherwise
    // the chosen width (1-2px) in the border colour.
    t.insert(QStringLiteral("CARDBORDER"),
             app_.borderWidth == 0
                 ? QStringLiteral("1px solid %1").arg(color(Color::Border).name())
                 : QStringLiteral("%1px solid %2")
                       .arg(QString::number(app_.borderWidth),
                            color(Color::BorderStrong).name()));

    // Corner radii („Square" = 0: TOT pătrat — carduri, tabele, bife, chip-uri).
    t.insert(QStringLiteral("R"), QString::number(r.base));
    t.insert(QStringLiteral("RSMALL"), QString::number(r.small));
    t.insert(QStringLiteral("RBIG"), QString::number(r.big));
    t.insert(QStringLiteral("RCHECK"), QString::number(r.check));
    t.insert(QStringLiteral("RNAV"), QString::number(r.nav));
    t.insert(QStringLiteral("RPILL"), QString::number(r.pill));
    t.insert(QStringLiteral("RCHIP"), QString::number(r.chip));
    t.insert(QStringLiteral("RBAR"), QString::number(r.bar));
    t.insert(QStringLiteral("RBTN"), QString::number(r.button));
    t.insert(QStringLiteral("RSEARCH"), QString::number(r.search));

    // Scală tipografică compactă/modernă (ca VS Code / Claude desktop) + pasul
    // de mărime ales de user.
    t.insert(QStringLiteral("FTITLE"), QString::number(px(font::kTitle)));
    t.insert(QStringLiteral("FDESC"), QString::number(px(font::kDescription)));
    t.insert(QStringLiteral("FNAV"), QString::number(px(font::kNav)));
    t.insert(QStringLiteral("FNAVSUB"), QString::number(px(font::kNavSub)));
    t.insert(QStringLiteral("FBODY"), QString::number(px(font::kBody)));
    t.insert(QStringLiteral("FSMALL"), QString::number(px(font::kSmall)));
    t.insert(QStringLiteral("FBIG"), QString::number(px(font::kBig)));

    // Săgeata combo pe gri-ul temei; bifa în culoarea textului pe accent
    // (albă pe accente închise, închisă pe accente deschise ca amber).
    const bool dark = isDark();
    t.insert(QStringLiteral("CHEVMUTED"), dark ? QStringLiteral("ChevronDownMuted")
                                               : QStringLiteral("ChevronDownMutedLight"));
    t.insert(QStringLiteral("CHEVSOFT"), dark ? QStringLiteral("ChevronDownSoft")
                                              : QStringLiteral("ChevronDownSoftLight"));
    t.insert(QStringLiteral("CHECK"), color(Color::AccentText).lightness() > 128
                                          ? QStringLiteral("TickLight")
                                          : QStringLiteral("TickDark"));
    return t;
}

QString ThemeManager::generateQss() const
{
    // One pass over the template, each %NAME% → its value. An unknown name is
    // left in place and logged (--smoke-theme fails on any that is left).
    static const QRegularExpression tokenRx(QStringLiteral("%([A-Z_]+)%"));
    const QString& tmpl = qssTemplate();
    const QHash<QString, QString> values = qssTokens();
    QString qss;
    qss.reserve(tmpl.size() + tmpl.size() / 8);
    qsizetype last = 0;
    for (auto it = tokenRx.globalMatch(tmpl); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        qss += QStringView(tmpl).mid(last, m.capturedStart() - last);
        const auto value = values.constFind(m.captured(1));
        if (value != values.cend()) {
            qss += *value;
        } else {
            qWarning() << "[theme] unknown token in theme.qss:" << m.captured(0);
            qss += m.capturedView(0);
        }
        last = m.capturedEnd();
    }
    qss += QStringView(tmpl).mid(last);
    return qss;
}

void ThemeManager::apply()
{
    QFont appFont(QStringLiteral("Segoe UI"), tokens::font::kBasePt + app_.fontDelta);
    appFont.setStyleStrategy(QFont::PreferAntialias);
    qApp->setFont(appFont);

    QPalette pal;
    pal.setColor(QPalette::Window, color(Color::Bg));
    pal.setColor(QPalette::WindowText, color(Color::Text));
    pal.setColor(QPalette::Base, color(Color::Surface));
    pal.setColor(QPalette::AlternateBase, color(Color::SurfaceAlt));
    pal.setColor(QPalette::Text, color(Color::Text));
    pal.setColor(QPalette::Button, color(Color::Surface));
    pal.setColor(QPalette::ButtonText, color(Color::TextSoft));
    pal.setColor(QPalette::Highlight, color(Color::Selection));
    pal.setColor(QPalette::HighlightedText, color(Color::Text));
    pal.setColor(QPalette::ToolTipBase, color(Color::Surface));
    pal.setColor(QPalette::ToolTipText, color(Color::Text));
    pal.setColor(QPalette::PlaceholderText, color(Color::Muted));
    pal.setColor(QPalette::Disabled, QPalette::Text, color(Color::Muted));
    qApp->setPalette(pal);

    qApp->setStyleSheet(generateQss());
    // The application icon (title bar, taskbar, Alt+Tab) in the accent and
    // the corner shape.
    setAppIcon(QColor(app_.accent), squareCorners());
    emit changed();
}

} // namespace app::ui::theme

#include "ThemeManager.moc"
