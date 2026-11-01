#pragma once

#include "ui/design/Tokens.h"

#include <QColor>
#include <QSize>
#include <QString>

class QAbstractButton;
class QAbstractItemView;
class QBoxLayout;
class QDialog;
class QFormLayout;
class QFrame;
class QIcon;
class QLabel;
class QLineEdit;
class QWidget;
class QVBoxLayout;
class QHBoxLayout;
class QToolButton;

// ============================================================
// PageKit — piese de UI comune tuturor paginilor, ca totul să fie
// uniform și simetric: aceleași margini, același câmp de căutare,
// același empty-state, aceeași configurare de tabel. Înlocuiește
// duplicarea (margini 12/10, culori #2bbd8e hardcodate, empty-state-uri
// desenate de mână) care făcea ecranele să arate diferit de la o pagină
// la alta. Culorile vin din ThemeManager (accent ales + dark/light).
// ============================================================

namespace app::ui::page {

// ----- Tokens de layout: o singură sursă, ui/design/Tokens.h (tokens::layout) -----
using tokens::layout::kCellPadX;
using tokens::layout::kControlH;
using tokens::layout::kFieldMaxW;
using tokens::layout::kFormHSpacing;
using tokens::layout::kFormLabelW;
using tokens::layout::kFormVSpacing;
using tokens::layout::kIconBtn;
using tokens::layout::kPageMarginBot;
using tokens::layout::kPageMarginTop;
using tokens::layout::kPageMarginX;
using tokens::layout::kPageSpacing;
using tokens::layout::kRowHeight;
using tokens::layout::kSearchMax;
using tokens::layout::kSearchMin;
using tokens::layout::kSectionPadX;
using tokens::layout::kSectionPadY;
using tokens::layout::kToolbarSpacing;

// Fereastra aplicației — aceeași coajă pentru MainWindow și LoginDialog (WindowShell).
inline constexpr QSize kWindowSize{1600, 950};    // mărimea implicită
inline constexpr QSize kWindowMinSize{1280, 800}; // mărimea minimă

// ----- Culori din temă (citite live din ThemeManager: urmează accentul ales
// și tema dark/light — nicio culoare hardcodată pe pagini) -----
QColor accent();  // acțiuni + stări „pornit": play, link, copy, enabled, running
QColor success(); // rezultat reușit (Success / OK / allow)
QColor danger();  // eroare, ștergere, stop, failed
QColor warning(); // pending / challenge
QColor muted();   // text și iconițe secundare
QColor text();    // text principal (celule cu valori)
// Raza pt. desen custom (QPainter / QSS inline): `natural` pe temele rotunjite,
// 0 pe „Square". Pastile, avatare, benzi de rând, badge-uri urmează astfel
// aceeași formă ca bifele și cardurile din QSS (nicio rază fixă pe pagini).
qreal corner(qreal natural);

// Applies the standard margins and spacing to the page's root layout.
void applyPageLayout(QVBoxLayout* root);

// ----- Bara de acțiuni de JOS (standard unic pe toate paginile) -----
// Butoanele unei pagini stau JOS, sub conținut; sus rămân doar taburile
// (QTabWidget). Ordinea e aceeași peste tot:
//   left : selecție/bulk (bifa „toate", Start/Close…), căutare, filtre, apoi
//          count/status (#mimCount). O acțiune distructivă a întregii pagini
//          (ex. „Reset to defaults") stă prima în `left`.
//   right: acțiunile secundare (Refresh, Import, Open folder…) și, ULTIMUL,
//          butonul primar (#mimNew / #primaryButton) — extrema dreaptă, ca OK-ul
//          din dialoguri.
// `widget` se adaugă ULTIMUL în layout-ul paginii / tab-ului (stretch 0). Pe
// paginile fără conținut extensibil (formulare), pune un addStretch(1) înainte
// ca bara să stea lipită de marginea de jos.
struct ActionBar
{
    QWidget* widget = nullptr;
    QHBoxLayout* left = nullptr;
    QHBoxLayout* right = nullptr;
};
ActionBar makeActionBar(QWidget* parent);
// A count at the end of the left side: on a narrow window it is what gets cut, not a button.
void addBarCount(const ActionBar& bar, QLabel* count);

// Formular standard: etichete stânga + centrate vertical pe câmp, spațiere
// kFormHSpacing/kFormVSpacing, câmpurile cresc pe lățime.
void applyFormLayout(QFormLayout* form);

// Aliniază coloana de etichete a mai multor formulare (carduri succesive dintr-o
// pagină): toate etichetele primesc lățimea celei mai late → câmpurile încep la
// același x în toate cardurile. Se cheamă după ce rândurile au fost adăugate.
void alignFormLabels(const QList<QFormLayout*>& forms, int minWidth = 0);

// ----- Secțiuni (Settings și ecranele de tip formular) -----
// Un card pe TOATĂ lățimea conținutului: titlu, descriere opțională, apoi
// `body` (padding kSectionPadX/Y, spacing kFormVSpacing). Toate secțiunile
// arată și se aliniază la fel — nu blocuri izolate de lățimi diferite.
struct Section
{
    QFrame* card = nullptr;
    QVBoxLayout* body = nullptr;
};
Section makeSection(QWidget* parent, const QString& title,
                    const QString& description = QString());
// Formular standard adăugat în corpul secțiunii.
QFormLayout* addSectionForm(const Section& section);
// Finalizează formularele unui ecran: aceeași coloană de etichete (minim
// kFormLabelW) și câmpuri plafonate la kFieldMaxW, aliniate stânga — nu
// combo-uri de 1300px pe ecrane late. Se cheamă după adăugarea rândurilor.
void finishForms(const QList<QFormLayout*>& forms);

// Standard search field (object „mimSearch", magnifier icon, fixed width).
QLineEdit* makeSearch(QWidget* parent, const QString& placeholder);

// Uniform empty-state: large muted icon + title + subtitle, centered.
// `iconKind` is a ui::icon::Kind (int so we don't force the include here).
QWidget* makeEmptyState(QWidget* parent, int iconKind, const QString& title,
                        const QString& subtitle);

// "No results" state for a list whose search / filter matched nothing — kept
// apart from the first-run empty state, which would wrongly say the list is
// empty (or, on Services, offer to locate the folder again).
QWidget* makeNoResultsState(QWidget* parent);

// Shows exactly one of: the table (rows > 0), the "No results" state (no rows
// because of `query` or an active `filtered`), or the first-run `emptyState`
// (no rows, nothing narrowing the list). `query` is the trimmed search text.
void showListState(QWidget* table, QWidget* emptyState, QWidget* noResults, int rows,
                   const QString& query, bool filtered = false);

// Standard table configuration (row selection, no grid, alternating
// rows, no vertical header, row height). Click-to-copy NU e implicit — vezi
// installCopyOnClick (opt-in pe coloane).
void configureTable(QAbstractItemView* view, int rowHeight = kRowHeight);

// ----- Acțiuni pe rând (uniforme în toate grid-urile) -----
// Celula de acțiuni: un widget cu layout orizontal centrat, margini/spacing
// standard; butoanele au înălțime fixă, deci nu depășesc niciodată rândul.
// `outLayout` primește layout-ul (adaugi butoanele la stânga, apoi un stretch).
QWidget* rowActions(QWidget* parent, QHBoxLayout** outLayout);

// ----- Ferestre de dialog: mărimi standard (o singură sursă) -----
// Înainte fiecare dialog își alegea 440-560px fără legătură cu conținutul sau
// cu fereastra principală → formulare tăiate, butoane înghesuite. Clasa dă
// lățimea de pornire; înălțimea = conținutul (sizeHint), cel puțin minHeight.
// Totul e plafonat la ~90% din fereastra principală / ecran.
enum class DialogSize {
    Compact, // 600px: confirmări/editări scurte (1-3 câmpuri)
    Form,    // 760px: formulare obișnuite
    Wide,    // 980×700: liste, tabele, detalii
    Large,   // 1240×820: ferestre de lucru (setări serviciu, detalii sesiune)
};
void fitDialog(QDialog* dialog, DialogSize size, int minHeight = 0);

// Marginile/spacing-ul standard ale layout-ului rădăcină dintr-un dialog.
void applyDialogLayout(QBoxLayout* root);

// Icon pe un buton CU TEXT (toolbar, dialog). Cu QSS activ, Qt lasă ~2-4px
// hardcodat între icon și text (iconul arăta lipit: „⟳Refresh") și centrează
// iconul geometric (pare mai sus decât textul). Pixmap-ul primește un spațiu
// transparent în dreapta + glyph coborât 1px; setează și iconSize. Pentru
// butoanele doar-icon folosește setIcon() normal.
void setLabeledIcon(QAbstractButton* button, const QIcon& icon);

// Buton-icon standard de rând: pătrat kIconBtn, autoRaise, tooltip. `iconKind`
// e un ui::icon::Kind; `color` o culoare de temă (ex. page::danger()).
QToolButton* rowIconButton(QWidget* parent, int iconKind, const QColor& color,
                           const QString& tooltip);

} // namespace app::ui::page
