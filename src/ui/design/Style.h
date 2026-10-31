#pragma once

#include "ui/design/Tokens.h"

#include <QColor>

class QLabel;
class QString;
class QWidget;

// ============================================================
// Style — the component API of the design system.
//
// A widget says WHAT it is through a dynamic property and res/theme.qss
// decides how that looks, in both themes and for every accent and corner
// setting:
//   tone    — the meaning of a label's colour   QLabel[tone="error"]
//   variant — the kind of component             QLabel[variant="chip"]
// The stylesheet follows a theme change by itself: nothing styled this way
// needs re-tinting by hand, and no page writes colours into setStyleSheet().
// Guide: docs/design-system.md.
// ============================================================

namespace app::ui {

// The live value of a colour role (ThemeManager::color), for painting code.
QColor color(Color role);

// What a coloured text means. Neutral is the label's default colour.
enum class Tone { Neutral, Muted, Accent, Success, Warning, Error };

// A label's text in the colour of its meaning; Neutral clears it.
void setTone(QWidget* widget, Tone tone);
// The colour of a tone, for painting code (Neutral is TextSoft).
QColor toneColor(Tone tone);

// A status line (a dialog's "Testing… / Saved. / Failed: …"): the text in
// its tone, shown only while there is text.
void setStatus(QLabel* label, const QString& text, Tone tone);

// A chip: a small tag tinted in its tone (a zone status, the SSL mode).
// Neutral reads as Muted.
void setChip(QLabel* label, const QString& text, Tone tone);

// Re-matches the stylesheet after a property its selectors read changed
// (the helpers above do it themselves).
void repolish(QWidget* widget);

} // namespace app::ui
