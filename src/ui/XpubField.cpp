#include "ui/XpubField.h"

#include "chain/Address.h"
#include "ui/Icons.h"
#include "ui/PageKit.h"
#include "ui/TableUtil.h"
#include "ui/ThemeManager.h"
#include "wallet/Types.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>

#include <exception>

namespace app::ui {

namespace {

QString accountPath(const wallet::AccountKey& key)
{
    try {
        return QString::fromStdString(chain::accountRootPath(key.type, key.account, key.format()));
    } catch (const std::exception&) {
        return QString();
    }
}

QIcon copyIcon()
{
    return makeIcon(icon::Kind::Copy, page::accent(), 16);
}

// An xpub reveals every address of its account. QLineEdit's own copy (Ctrl+C,
// the context menu) puts plain text on the clipboard, which Windows keeps in
// its history and may sync; every copy here goes through copyTextToClipboard()
// instead, which keeps it out of both.
class XpubField : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->matches(QKeySequence::Copy)) {
            copySelection();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

    // Read-only: Copy and Select All are all the standard menu would offer.
    void contextMenuEvent(QContextMenuEvent* event) override
    {
        QMenu menu(this);
        menu.addAction(QStringLiteral("Copy"), this, [this] { copySelection(); });
        menu.addAction(QStringLiteral("Select All"), this, &QLineEdit::selectAll);
        menu.exec(event->globalPos());
    }

private:
    void copySelection() { copyTextToClipboard(this, hasSelectedText() ? selectedText() : text()); }
};

} // namespace

QString accountLabel(const wallet::AccountKey& key)
{
    QString label = QString::fromLatin1(chain::scriptTypeName(key.type));
    if (key.account > 0)
        label += QStringLiteral(", account %1").arg(key.account);
    return label;
}

QLineEdit* makeXpubField(QWidget* parent, const wallet::AccountKey& key)
{
    auto* edit = new XpubField(key.xpub, parent);
    edit->setReadOnly(true);
    edit->setCursorPosition(0); // the prefix (xpub / ypub / zpub) stays visible
    edit->setToolTip(QStringLiteral("%1\n%2").arg(accountPath(key), key.xpub));
    QAction* copy = edit->addAction(copyIcon(), QLineEdit::TrailingPosition);
    copy->setToolTip(QStringLiteral("Copy"));
    const QString xpub = key.xpub;
    QObject::connect(copy, &QAction::triggered, edit, [edit, xpub] { copyTextToClipboard(edit, xpub); });
    QObject::connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed, copy,
                     [copy] { copy->setIcon(copyIcon()); });
    return edit;
}

} // namespace app::ui
