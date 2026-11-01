#include "ui/TableUtil.h"
#include "ui/PageKit.h"
#include "ui/Feedback.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QSet>
#include <QTableWidgetItem>

namespace app::ui {

QTableWidgetItem* makeCell(const QString& text, bool muted)
{
    auto* it = new QTableWidgetItem(text);
    it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    if (muted)
        it->setForeground(page::muted());
    if (text.size() > 40)
        it->setToolTip(text);
    return it;
}

QString shortTimestamp(const QString& raw)
{
    const QString t = raw.trimmed();
    if (t.isEmpty() || t == QLatin1String("never"))
        return t.isEmpty() ? QStringLiteral("—") : t;
    // "2026-10-15T20:07:41.7243" / "2026-10-20 23:24:20.0241716" → "2026-10-15 20:07"
    QString s = t;
    s.replace(QLatin1Char('T'), QLatin1Char(' '));
    return s.left(16); // yyyy-MM-dd HH:mm
}

void copyTextToClipboard(QWidget* host, const QString& text, const QString& label)
{
    const QString value = text.trimmed();
    if (value.isEmpty())
        return;
    auto* data = new QMimeData;
    data->setText(value);
#ifdef Q_OS_WIN
    // Windows clipboard history (Win+V) would keep the copy and cloud clipboard
    // sync would upload it, and an xpub lists every address of its account.
    // These formats opt it out of both; the text pastes as before. Qt registers
    // each custom format under the name in quotes.
    const QByteArray off(4, '\0'); // DWORD 0 (the first format takes any data)
    for (const char* format : {"ExcludeClipboardContentFromMonitorProcessing", "CanIncludeInClipboardHistory",
                               "CanUploadToCloudClipboard"})
        data->setData(QStringLiteral("application/x-qt-windows-mime;value=\"%1\"").arg(QLatin1StringView(format)),
                      off);
#endif
    QApplication::clipboard()->setMimeData(data); // takes ownership
    QString shown = value;
    if (shown.size() > 72)
        shown = shown.left(69) + QStringLiteral("…");
    Feedback::show(host ? host->window() : nullptr, Feedback::Type::Success,
                label.isEmpty() ? QStringLiteral("Copied: %1").arg(shown)
                                : QStringLiteral("%1: %2").arg(label, shown));
}

namespace {

// Valoarea de copiat dintr-o celulă din coloanele permise (gol = nu se copiază).
// Placeholder-ele („—", valori mascate/„(locked)") nu sunt valori.
QString copyValue(const QModelIndex& index, const QSet<int>& columns)
{
    if (!index.isValid() || !columns.contains(index.column()))
        return QString();
    const QVariant custom = index.data(kCopyRole);
    const QString text = (custom.isValid() ? custom.toString() : index.data().toString())
                             .trimmed();
    if (text.isEmpty() || text == QLatin1String("—")
        || text.endsWith(QLatin1String("(locked)")))
        return QString();
    return text;
}

// Cursor „mână" doar peste celulele care chiar se copiază.
class CopyCursorFilter : public QObject
{
public:
    CopyCursorFilter(QAbstractItemView* view, const QSet<int>& columns)
        : QObject(view)
        , view_(view)
        , columns_(columns)
    {
    }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        if (event->type() == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(event);
            const bool copyable =
                !copyValue(view_->indexAt(me->position().toPoint()), columns_).isEmpty();
            if (copyable != pointing_) {
                pointing_ = copyable;
                if (copyable)
                    view_->viewport()->setCursor(Qt::PointingHandCursor);
                else
                    view_->viewport()->unsetCursor();
            }
        } else if (event->type() == QEvent::Leave && pointing_) {
            pointing_ = false;
            view_->viewport()->unsetCursor();
        }
        return QObject::eventFilter(obj, event);
    }

private:
    QAbstractItemView* view_;
    QSet<int> columns_;
    bool pointing_ = false;
};

} // namespace

void installCopyOnClick(QAbstractItemView* view, const QList<int>& columns)
{
    if (!view || columns.isEmpty())
        return;
    const QSet<int> allowed(columns.begin(), columns.end());
    view->setMouseTracking(true);
    view->viewport()->installEventFilter(new CopyCursorFilter(view, allowed));
    QObject::connect(view, &QAbstractItemView::clicked, view,
                     [view, allowed](const QModelIndex& index) {
                         const QString text = copyValue(index, allowed);
                         if (!text.isEmpty())
                             copyTextToClipboard(view, text);
                     });
}

} // namespace app::ui
