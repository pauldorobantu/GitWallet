#pragma once

#include <QString>

class QColor;
class QIcon;

namespace app::ui {

// Icon set 2026 — line icons drawn vectorially (stroke, rounded corners),
// v3's own; NOT the MDL2 font.
namespace icon {

enum class Kind {
    Dashboard,    // grid 2x2
    Profiles,     // identity card
    Sessions,     // monitor + play
    Flows,        // connected nodes
    Pages,        // document with code
    Plugins,      // puzzle
    Traffic,      // activity/pulse
    Cloud,        // cloud
    Shield,       // shield
    Mail,         // envelope
    Logs,         // terminal
    Settings,     // sliders
    Search,
    Plus,
    Cross,
    Play,
    Stop,
    Refresh,
    ChevronDown,
    Minimize,
    Maximize,
    Restore,
    Close,
    Menu,   // hamburger (toggle sidebar)
    Brush,  // appearance/tema
    // Set MEmu Multi (managerul de instanțe) — pagina Profiles.
    Copy,         // două dreptunghiuri suprapuse (clone)
    Filter,       // pâlnie (filtrare stare)
    ListRows,     // trei rânduri cu puncte (sortare)
    MinusCircle,  // cerc cu minus (ștergere rând)
    Globe,        // glob (open URL prin profil)
    ImportDown,   // săgeată în tavă (import/restore)
    Key,          // cheie (access keys)
    ExternalLink, // săgeată externă (deschide folder)
    LayoutGrid,   // grid 2x2 de carduri (toggle view profiles)
    Pencil,       // creion (editare rând)
    Check,        // bifă (test/verificare pe rând)
    Power,        // power (set active pe rând)
    Sun,          // temă light (toggle în bara de titlu)
    Moon,         // temă dark (toggle în bara de titlu)
    User,         // the profile avatar when there is no initial
    LogOut,       // door + arrow out (profile menu → Log out)
};

} // namespace icon

// Renders the icon at the given size and color (stroke ~1.6px, devicePixel aware).
QIcon makeIcon(icon::Kind kind, const QColor& color, int size = 20, qreal stroke = 1.6);

// Variantă plină (fill) a glyph-ului Lucide — pt Play/Stop pe rânduri, unde
// conturul gol arăta slab. Fallback la conturul normal dacă SVG-ul lipsește.
QIcon makeSolidIcon(icon::Kind kind, const QColor& color, int size = 16);

// Glyph de caption Windows 11 (Minimize/Maximize/Restore/Close): desenat
// vectorial direct, NU din setul Lucide („Maximize"=săgeți de expand,
// „Restore"=copy — greșite pt butoane de fereastră). Stroke plat subțire,
// centrat pe grilă 16. devicePixel aware.
QIcon makeCaptionIcon(icon::Kind kind, const QColor& color, int size = 16);

// „Advanced" duotone icon for navigation: rounded tinted plate (alpha ~15%)
// + more detailed glyph, richer than the line-only variant. devicePixel aware.
QIcon makeDuotoneIcon(icon::Kind kind, const QColor& color, int size = 26);

// The service's official logo (slug from services/_index.yaml):
//  - Google / Google (RP): the four-color „G" drawn vectorially;
//  - Microsoft: the 4 official squares;
//  - fallback: monogram with the initial (color derived from slug).
QIcon makeBrandLogo(const QString& slug, const QString& displayName, int size = 28);

} // namespace app::ui
