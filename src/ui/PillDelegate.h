#pragma once

#include <QStyledItemDelegate>

namespace app::ui {

// Draws the cell value as a "pill" badge: colored dot + text, on a colored
// translucent background. Colors derived from the text (running/ok, error/failed, …).
class PillDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static QString normalize(const QString& raw);

protected:
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
};

} // namespace app::ui
