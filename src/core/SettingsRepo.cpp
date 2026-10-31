#include "core/SettingsRepo.h"

#include <QSettings>
#include <QStandardPaths>
#include <QDir>

namespace app {

namespace {
QSettings& store()
{
    // One INI file per user, next to the wallet data. Lazily created.
    static QString path = [] {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir().mkpath(dir);
        return dir + QStringLiteral("/settings.ini");
    }();
    static QSettings s(path, QSettings::IniFormat);
    return s;
}
} // namespace

QString SettingsRepo::get(const QString& key) const
{
    return store().value(key).toString();
}

void SettingsRepo::set(const QString& key, const QString& value)
{
    store().setValue(key, value);
    store().sync();
}

} // namespace app
