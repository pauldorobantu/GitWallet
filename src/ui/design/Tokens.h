#pragma once

#include <QtGlobal>

// ============================================================
// Design tokens — the named values the GUI is drawn with.
//
//  * Colours are semantic ROLES (Color::Error, not a hex). ThemeManager
//    resolves a role for the active palette (dark / light) and the user's
//    accent and border colour: ThemeManager::color(Color), or ui::color()
//    from ui/design/Style.h.
//  * Type sizes, layout spacing and the corner-radius rules are constants
//    here. Type sizes are px before the user's size step (Settings →
//    Appearance), which ThemeManager::px() adds; the radii follow the user's
//    corner setting (radiiFor).
//
// res/theme.qss receives these values as %TOKEN% placeholders, filled by
// ThemeManager::qssTokens() — each value below names its token. Guide:
// docs/design-system.md.
// ============================================================

namespace app::ui {

// Semantic colour roles.
enum class Color {
    // The accent (Settings → Appearance)
    Accent,        // primary actions, "on" states, focus        %ACCENT% %ACCENTRGB% %SEL%
    AccentText,    // text and glyphs on the accent (dark on light accents)  %ACCENTTEXT%
    AccentHover,   // the primary button, hovered                %ACCENTHOVER%
    AccentPressed, // the primary button, pressed                %ACCENTPRESSED%
    // Surfaces
    Bg,            // window and page background                 %BG%
    Side,          // the sidebar                                %SIDE%
    Surface,       // cards, tables, menus, tooltips             %SURFACE% %TBBG% %TBHEADBG%
    SurfaceAlt,    // inputs, secondary buttons                  %SURFACEALT%
    SurfaceHover,  // a secondary button, hovered                %HOVERSURF%
    // Text
    Text,          // primary text and values                    %TEXT%
    TextSoft,      // the default text of controls and labels    %TEXTSOFT%
    Muted,         // secondary text, captions, placeholders     %MUTED% %MUTEDRGB%
    // Lines
    Border,        // dividers, card and table borders           %BORDER% %TBBORDER%
    BorderStrong,  // input and button borders; the user's border colour  %BORDERS%
    Frame,         // the 1px frame of the main window and dialogs  %WINDOWFRAME%
    // Translucent veils
    Hover,         // anything hovered                           %HOVER%
    Press,         // anything pressed, stronger than Hover      %PRESS%
    Selection,     // selected text and items, the palette's Highlight  %SEL%
    RowSelection,  // the band of a selected table row
    Zebra,         // the band of an alternate table row
    // Status
    Success,       //                                            %SUCCESS%
    Warning,       //                                            %WARNING%
    Error,         //                                            %ERROR%
    Info,
};

namespace tokens {

// Type scale: px in the stylesheet, before the user's size step.
namespace font {
constexpr int kBasePt = 10;      // the application font, in pt (QFont)
constexpr int kTitle = 20;       // page titles                               %FTITLE%
constexpr int kBig = 14;         // section, group-box and empty-state titles %FBIG%
constexpr int kBody = 13;        // text, inputs, buttons, tables, tabs       %FBODY%
constexpr int kDescription = 13; // page descriptions                         %FDESC%
constexpr int kNav = 13;         // sidebar items, the profile chip           %FNAV%
constexpr int kNavSub = 12;      // profile menu details                      %FNAVSUB%
constexpr int kSmall = 11;       // table headers, tooltips                   %FSMALL%
} // namespace font

// Layout spacing, px. ui::page (PageKit.h) re-exports these under the same names.
namespace layout {
constexpr int kPageMarginX = 16;   // page margin, left and right
constexpr int kPageMarginTop = 14; // page margin, top
constexpr int kPageMarginBot = 14; // page margin, bottom
constexpr int kPageSpacing = 12;   // between the toolbar, the table, the bars
constexpr int kToolbarSpacing = 8; // between the controls of a bar
constexpr int kRowHeight = 40;     // a table row
constexpr int kIconBtn = 26;       // a row's square icon button (QSS #mimIconBtn)
constexpr int kCellPadX = 10;      // a table item's horizontal padding (QSS QTableView::item)
constexpr int kSearchW = 220;      // the search field's width, with room to spare
constexpr int kSearchMinW = 140;   // … on a window too narrow for what is beside it
constexpr int kControlH = 32;      // the border-box height of buttons and fields (QSS)
constexpr int kFormHSpacing = 14;  // form: label → field
constexpr int kFormVSpacing = 10;  // form: between rows
constexpr int kSectionPadX = 20;   // a section card's padding, left and right
constexpr int kSectionPadY = 16;   // … top and bottom
constexpr int kFormLabelW = 180;   // the label column of the settings forms
constexpr int kFieldMaxW = 460;    // the widest single-line field in a section
constexpr int kDialogPadX = 22;    // a dialog's root layout (applyDialogLayout)
constexpr int kDialogPadTop = 20;
constexpr int kDialogPadBot = 18;
constexpr int kDialogSpacing = 14;
} // namespace layout

// Corner radii. The user picks one radius (Settings → Appearance; 0 is
// "Square", where everything is square) and each kind of element derives
// its own from it — radiiFor().
namespace radius {
constexpr int kDefault = 8;                       // the radius out of the box
constexpr int kSmallMin = 2;                      // small: half the radius, at least this
constexpr int kBigMin = 6;                        // big: the radius, at least this
constexpr int kCheckMax = 4;                      // check boxes: the small one, at most this
constexpr int kNavMax = 8;                        // sidebar items: the radius, at most this
constexpr int kButtonMax = layout::kControlH / 2; // buttons never turn into pills
constexpr int kPill = 11;                         // pills (title-bar status, details)
constexpr int kChip = 9;                          // chips (ui::setChip)
constexpr int kBar = 4;                           // scroll bars, progress bars
constexpr int kSearch = layout::kControlH / 2;    // the search field is a pill
} // namespace radius

struct Radii
{
    int base = 0;   // inputs, menus                                %R%
    int small = 0;  // tool buttons, list and menu items, tooltips  %RSMALL%
    int big = 0;    // cards, tables, group boxes                   %RBIG% %TBRADIUS%
    int check = 0;  // check-box indicators                         %RCHECK%
    int nav = 0;    // sidebar and title-bar items                  %RNAV%
    int pill = 0;   // pills                                        %RPILL%
    int chip = 0;   // chips                                        %RCHIP%
    int bar = 0;    // scroll and progress bars                     %RBAR%
    int button = 0; // push buttons                                 %RBTN%
    int search = 0; // the search field                             %RSEARCH%
};

// The radii for the user's corner setting `r` (Appearance::radius).
constexpr Radii radiiFor(int r)
{
    const bool square = r == 0;
    Radii out;
    out.base = r;
    out.small = square ? 0 : qMax(radius::kSmallMin, r / 2);
    out.big = square ? 0 : qMax(r, radius::kBigMin);
    out.check = qMin(radius::kCheckMax, out.small);
    out.nav = qMin(r, radius::kNavMax);
    out.pill = square ? 0 : radius::kPill;
    out.chip = square ? 0 : radius::kChip;
    out.bar = square ? 0 : radius::kBar;
    out.button = qMin(r, radius::kButtonMax);
    out.search = square ? 0 : radius::kSearch;
    return out;
}

} // namespace tokens

} // namespace app::ui
