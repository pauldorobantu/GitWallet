#pragma once

#include <QList>
#include <QModelIndex>
#include <QString>

#include <functional>

class QAbstractItemView;
class QTableWidgetItem;
class QWidget;

namespace app::ui {

// Standard table cell (read-only; optional muted foreground; tooltip
// for long text). Single source — replaces the `cell()` duplicated in each page.
QTableWidgetItem* makeCell(const QString& text, bool muted = false);

// Human-readable timestamp: normalizes "yyyy-MM-ddTHH:mm:ss.ffffff" or the space
// form to "yyyy-MM-dd HH:mm" (without milliseconds/"T"). Empty → "—".
QString shortTimestamp(const QString& raw);

// Copies to the clipboard + a discreet confirmation next to the cursor.
// Truncates long text in the confirmation (keys/URLs stay whole in the clipboard).
// On Windows the copy is kept out of clipboard history and cloud clipboard sync.
void copyTextToClipboard(QWidget* host, const QString& text,
                         const QString& label = QString());

// Click-to-copy, OPT-IN pe coloane: doar coloanele cu valori-rezultat pe care
// utilizatorul chiar le lipește în altă parte (e-mail, IP, URL, endpoint, cod
// TOTP…). Restul celulelor (nume, stări, date, contoare, etichete) NU se copiază
// la click. Peste celulele copiabile cursorul devine „mână". Fără coloane = nimic.
// O celulă poate da valoarea exactă de copiat prin kCopyRole (ex. URL-ul întreg
// când afișează doar host-ul); kCopyRole setat dar gol = celula nu se copiază.
constexpr int kCopyRole = 0x0100 + 50; // Qt::UserRole + 50
// `resolve`, if given, says at the moment of the click what goes to the clipboard; empty: nothing does.
void installCopyOnClick(QAbstractItemView* view, const QList<int>& columns,
                        const std::function<QString(const QModelIndex&)>& resolve = {});

} // namespace app::ui
