#pragma once

#include <QString>

// Minimal key/value settings store used by the reused GUI kit (ThemeManager,
// TitleBar, LanguageManager) to persist appearance + language. Backed by a
// per-user INI file (QSettings) under the app's config dir. This is a small
// standalone replacement for FlowSuite's core/SettingsRepo — the GUI only ever
// calls get()/set() with a handful of "appearance.*" / "ui.language" keys.
namespace app {

class SettingsRepo
{
public:
    SettingsRepo() = default;

    QString get(const QString& key) const;
    void set(const QString& key, const QString& value);
};

} // namespace app

// The GUI files reference SettingsRepo unqualified (they live in app::ui and
// find it via the enclosing app namespace).
