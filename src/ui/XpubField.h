#pragma once

class QLineEdit;
class QString;
class QWidget;

namespace wallet {
struct AccountKey;
}

namespace app::ui {

// "Native SegWit (bech32)"; accounts past the first are numbered.
QString accountLabel(const wallet::AccountKey& key);

// A read-only xpub with a copy button at its end. An xpub reveals every address
// of its account: all its copies stay out of clipboard history and sync.
QLineEdit* makeXpubField(QWidget* parent, const wallet::AccountKey& key);

} // namespace app::ui
