#pragma once

#include "wallet/Registry.h"

#include <QColor>
#include <QIcon>

namespace app::ui {

// A coin's colour and its round mark, from the list of coins: one more coin is one more line
// there, and without those it still gets a colour of its own and its first letter.
QColor coinColor(const wallet::AssetInfo& info);
QIcon coinIcon(const wallet::AssetInfo& info, int size = 28);
// The same kind of mark for a name, a wallet's: what tells rows apart at a glance in a long list.
QIcon nameIcon(const QString& name, int size = 28);

} // namespace app::ui
