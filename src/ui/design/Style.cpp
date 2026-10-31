#include "ui/design/Style.h"

#include "ui/ThemeManager.h"

#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QStyle>
#include <QVariant>

namespace app::ui {

namespace {

const char* toneName(Tone tone)
{
    switch (tone) {
    case Tone::Neutral:
        return nullptr;
    case Tone::Muted:
        return "muted";
    case Tone::Accent:
        return "accent";
    case Tone::Success:
        return "success";
    case Tone::Warning:
        return "warning";
    case Tone::Error:
        return "error";
    }
    return nullptr;
}

// Sets a property the stylesheet's selectors read (nullptr clears it);
// true when the value changed.
bool setStyleProperty(QWidget* widget, const char* name, const char* value)
{
    const QVariant next = value ? QVariant(QString::fromLatin1(value)) : QVariant();
    if (widget->property(name) == next)
        return false;
    widget->setProperty(name, next);
    return true;
}

} // namespace

QColor color(Color role)
{
    return theme::ThemeManager::instance()->color(role);
}

void setTone(QWidget* widget, Tone tone)
{
    if (widget && setStyleProperty(widget, "tone", toneName(tone)))
        repolish(widget);
}

QColor toneColor(Tone tone)
{
    switch (tone) {
    case Tone::Neutral:
        return color(Color::TextSoft);
    case Tone::Muted:
        return color(Color::Muted);
    case Tone::Accent:
        return color(Color::Accent);
    case Tone::Success:
        return color(Color::Success);
    case Tone::Warning:
        return color(Color::Warning);
    case Tone::Error:
        return color(Color::Error);
    }
    return color(Color::TextSoft);
}

void setStatus(QLabel* label, const QString& text, Tone tone)
{
    if (!label)
        return;
    label->setText(text);
    setTone(label, tone);
    label->setVisible(!text.isEmpty());
}

void setChip(QLabel* label, const QString& text, Tone tone)
{
    if (!label)
        return;
    label->setText(text);
    const bool variant = setStyleProperty(label, "variant", "chip");
    const bool toned =
        setStyleProperty(label, "tone", toneName(tone == Tone::Neutral ? Tone::Muted : tone));
    if (variant || toned)
        repolish(label);
}

void repolish(QWidget* widget)
{
    // Not polished yet: the first polish (on show) reads the properties anyway.
    if (!widget || !widget->testAttribute(Qt::WA_WState_Polished))
        return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    // As setStyleSheet() does: a frame (a QLabel) re-reads its border and
    // padding only on StyleChange — without it a chip kept its old size.
    QEvent styleChange(QEvent::StyleChange);
    QCoreApplication::sendEvent(widget, &styleChange);
    widget->update();
}

} // namespace app::ui
