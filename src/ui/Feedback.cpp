#include "ui/Feedback.h"
#include "core/AppLogger.h"
#include "core/Database.h"
#include "diag/Diag.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

namespace app::ui {

namespace {

constexpr int kRingMax = 18;    // raza finală a pulsului
constexpr int kPillH = 26;      // înălțimea pastilei
constexpr int kPillPadX = 10;   // padding orizontal în pastilă
constexpr int kIconD = 14;      // diametrul discului cu icon
constexpr int kGap = 7;         // icon → text
constexpr int kTextMaxW = 300;  // textul lung se elidează
constexpr int kOffsetX = 14;    // pastila: la dreapta-jos de vârful cursorului
constexpr int kOffsetY = 16;
constexpr int kMargin = 8;      // distanța minimă față de marginile ferestrei
constexpr int kShadow = 6;      // loc pt umbra pastilei

QColor statusColor(Feedback::Type type)
{
    auto* tm = theme::ThemeManager::instance();
    switch (type) {
    case Feedback::Type::Success: return tm->color(QStringLiteral("success"));
    case Feedback::Type::Warning: return tm->color(QStringLiteral("warning"));
    case Feedback::Type::Error: return tm->color(QStringLiteral("error"));
    default: return tm->color(QStringLiteral("accent"));
    }
}

// Pictat integral (fără QSS): pulsul + pastila, cu opacitate animată.
// Un singur widget-copil al ferestrei, transparent la mouse.
class FeedbackWidget : public QWidget
{
public:
    FeedbackWidget(QWidget* window, Feedback::Type type, const QString& message)
        : QWidget(window)
        , type_(type)
        , color_(statusColor(type))
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setFocusPolicy(Qt::NoFocus);

        font_ = window->font();
        const QFontMetrics fm(font_);
        text_ = fm.elidedText(message.simplified(), Qt::ElideMiddle, kTextMaxW);
        const int pillW = kPillPadX + kIconD + kGap + fm.horizontalAdvance(text_) + kPillPadX;

        // Vârful cursorului, în coordonatele ferestrei (limitat la fereastră).
        QPoint tip = window->mapFromGlobal(QCursor::pos());
        if (!window->rect().contains(tip))
            tip = QPoint(window->width() / 2, window->height() / 2);

        // Pastila stă la dreapta-jos de cursor; lângă margini trece în partea opusă.
        QRect pill(tip + QPoint(kOffsetX, kOffsetY), QSize(pillW, kPillH));
        if (pill.right() > window->width() - kMargin)
            pill.moveRight(tip.x() - kOffsetX);
        if (pill.bottom() > window->height() - kMargin)
            pill.moveBottom(tip.y() - kOffsetY / 2);
        pill.moveLeft(qMax(kMargin, pill.left()));
        pill.moveTop(qMax(kMargin, pill.top()));

        const QRect ring(tip - QPoint(kRingMax, kRingMax), QSize(2 * kRingMax, 2 * kRingMax));
        const QRect area = ring.united(pill.adjusted(-kShadow, -kShadow, kShadow, kShadow + 2));
        setGeometry(area);
        tip_ = tip - area.topLeft();
        pill_ = pill.translated(-area.topLeft());

        animated_ = theme::ThemeManager::instance()->animationsEnabled();
        // Erorile/avertismentele stau mai mult (trebuie citite); confirmările scurt.
        const bool important =
            type == Feedback::Type::Error || type == Feedback::Type::Warning;
        const int total = important ? 2600 : 1400;
        total_ = total;

        raise();
        show();
        if (animated_) {
            auto* anim = new QVariantAnimation(this);
            anim->setStartValue(0.0);
            anim->setEndValue(1.0);
            anim->setDuration(total);
            connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
                elapsed_ = v.toDouble() * total_;
                update();
            });
            connect(anim, &QVariantAnimation::finished, this, &QObject::deleteLater);
            anim->start(QAbstractAnimation::DeleteWhenStopped);
        } else {
            QTimer::singleShot(total, this, &QObject::deleteLater);
        }
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        auto* tm = theme::ThemeManager::instance();

        // ---- Pulsul (doar cu animații): un inel care se lărgește și se stinge ----
        if (animated_ && elapsed_ < 420.0) {
            const qreal t = elapsed_ / 420.0;                 // 0 → 1
            const qreal ease = 1.0 - (1.0 - t) * (1.0 - t);   // out-quad
            const qreal r = 4.0 + (kRingMax - 5.0) * ease;
            QColor c = color_;
            c.setAlphaF(0.55 * (1.0 - t));
            p.setPen(QPen(c, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(tip_), r, r);
        }

        // ---- Pastila: intră (fade + 4px în sus), stă, iese (fade) ----
        qreal opacity = 1.0;
        qreal lift = 0.0;
        if (animated_) {
            const qreal in = 160.0, out = 260.0;
            if (elapsed_ < in) {
                opacity = elapsed_ / in;
                lift = 4.0 * (1.0 - opacity);
            } else if (elapsed_ > total_ - out) {
                opacity = qMax(0.0, (total_ - elapsed_) / out);
            }
        }
        if (opacity <= 0.0)
            return;
        p.setOpacity(opacity);
        const QRectF pill = QRectF(pill_).translated(0, lift);
        const qreal radius = tm->corner(kPillH / 2.0);

        // Umbră moale (câteva straturi translucide) — pastila „plutește" discret.
        p.setPen(Qt::NoPen);
        for (int i = 1; i <= 3; ++i) {
            QPainterPath sh;
            const qreal g = i * 1.6;
            sh.addRoundedRect(pill.adjusted(-g, -g + 1.5, g, g + 1.5), radius + g, radius + g);
            p.fillPath(sh, QColor(0, 0, 0, tm->isDark() ? 22 : 10));
        }

        QPainterPath body;
        body.addRoundedRect(pill, radius, radius);
        p.fillPath(body, tm->color(QStringLiteral("surface")));
        // Ramă fină tentată în culoarea stării — singurul semnal de culoare în afară de icon.
        QColor edge = color_;
        edge.setAlpha(110);
        p.setPen(QPen(edge, 1.0));
        p.drawPath(body);

        // Icon: disc plin în culoarea stării + glif desenat (bifă / x / ! / i).
        const QRectF disc(pill.left() + kPillPadX, pill.center().y() - kIconD / 2.0,
                          kIconD, kIconD);
        p.setPen(Qt::NoPen);
        p.setBrush(color_);
        p.drawEllipse(disc);
        p.setPen(QPen(tm->color(QStringLiteral("surface")), 1.6, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        const QPointF c = disc.center();
        switch (type_) {
        case Feedback::Type::Success: {
            QPainterPath tick;
            tick.moveTo(c + QPointF(-3.2, 0.2));
            tick.lineTo(c + QPointF(-0.9, 2.5));
            tick.lineTo(c + QPointF(3.3, -2.3));
            p.setBrush(Qt::NoBrush);
            p.drawPath(tick);
            break;
        }
        case Feedback::Type::Error:
            p.drawLine(c + QPointF(-2.6, -2.6), c + QPointF(2.6, 2.6));
            p.drawLine(c + QPointF(2.6, -2.6), c + QPointF(-2.6, 2.6));
            break;
        case Feedback::Type::Warning:
            p.drawLine(c + QPointF(0, -3.2), c + QPointF(0, 0.8));
            p.drawPoint(c + QPointF(0, 3.2));
            break;
        default:
            p.drawPoint(c + QPointF(0, -3.0));
            p.drawLine(c + QPointF(0, -0.6), c + QPointF(0, 3.2));
            break;
        }

        p.setFont(font_);
        p.setPen(tm->color(QStringLiteral("text")));
        const QRectF textRect(disc.right() + kGap, pill.top(),
                              pill.right() - kPillPadX - disc.right() - kGap, pill.height());
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text_);
    }

private:
    Feedback::Type type_;
    QColor color_;
    QFont font_;
    QString text_;
    QPoint tip_;
    QRect pill_;
    bool animated_ = false;
    qreal elapsed_ = 0.0;
    qreal total_ = 1.0;
};

} // namespace

void Feedback::show(QWidget* context, Type type, const QString& message)
{
    // Every failure shown to the user also lands in the log (Logs page / file):
    // the pill fades in seconds, the diagnostic trail must not. Local only: the
    // text may name a profile or a visitor's IP, and the curated APP_DIAG_*
    // call next to each toast already reports the failure to diagnostics.
    if (type == Type::Error && !message.trimmed().isEmpty())
        qCWarning(app::lcLocal).noquote() << "[ui] error shown:" << message.simplified();

    QWidget* window = context ? context->window() : QApplication::activeWindow();
    if (!window || message.trimmed().isEmpty())
        return;

    // Una singură odată: o confirmare nouă o înlocuiește imediat pe cea veche.
    static QPointer<FeedbackWidget> current;
    if (current)
        current->deleteLater();
    current = new FeedbackWidget(window, type, message);
}

bool Feedback::showRowResult(QWidget* context, db::RowResult result,
                             const QString& doneMessage, const QString& verb,
                             const QString& subject)
{
    switch (result) {
    case db::RowResult::Ok:
        show(context, Type::Success, doneMessage);
        return true;
    case db::RowResult::NotFound:
        show(context, Type::Info,
             QStringLiteral("%1 was already removed — list refreshed").arg(subject));
        return true;
    case db::RowResult::Error:
        break;
    }
    // Every page's single-row DB action ends here: the verb and the page say
    // which one (the subject is a user-chosen name — not reported).
    APP_DIAG_ERROR("ui.row.fail", "verb", verb, "page",
                   context ? context->metaObject()->className() : "?");
    show(context, Type::Error,
         QStringLiteral("Could not %1 %2 — see Logs").arg(verb, subject));
    return false;
}

} // namespace app::ui
