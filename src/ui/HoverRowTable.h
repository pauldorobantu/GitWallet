#pragma once

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QSet>
#include <QTableWidget>
#include <QTableView>

#include "ui/ThemeManager.h"

namespace app::ui {

// ============================================================
// Tables that mark the WHOLE row (not per cell/column):
//  - zebra, hover and selection are flat full-width bands drawn in
//    paintEvent BEFORE the cells — under text, pills and cell widgets;
//  - selection = ONE subtle uniform shade (theme role "rowSelection"):
//    no border, no accent bar, no extra hover tint on top of it.
// The per-cell QSS style (::item:hover / ::item:selected / alternate) is
// neutralized for these tables (see #rowTable / #mimTable in ThemeManager)
// — otherwise cells would paint their own patches over the band.
// ============================================================

namespace detail {

// Row backgrounds for the visible rows; rects in viewport coordinates.
inline void paintRowStrips(QTableView* view, QPainter& p, int hoverRow)
{
    const QAbstractItemModel* m = view->model();
    if (!m)
        return;

    auto* theme = theme::ThemeManager::instance();
    const QColor zebra = theme->color(QStringLiteral("zebra"));
    const QColor selection = theme->color(QStringLiteral("rowSelection"));
    QColor hover = theme->color(QStringLiteral("hover")); // alb/umbră după temă
    hover.setAlpha(qMin(255, hover.alpha() * 2 - 4));

    // the selected rows (set, for row selection)
    QSet<int> selected;
    if (auto* sel = view->selectionModel()) {
        const QModelIndexList rows = sel->selectedRows();
        if (!rows.isEmpty()) {
            for (const QModelIndex& idx : rows)
                selected.insert(idx.row());
        } else {
            for (const QModelIndex& idx : sel->selectedIndexes())
                selected.insert(idx.row());
        }
    }

    const bool alternate = view->alternatingRowColors();
    const int w = view->viewport()->width();
    const int vh = view->viewport()->height();
    for (int row = 0; row < m->rowCount(); ++row) {
        const int y = view->rowViewportPosition(row);
        const int h = view->rowHeight(row);
        if (y + h <= 0 || y >= vh)
            continue; // outside the visible area
        const QRect strip(0, y, w, h);
        if (selected.contains(row)) {
            p.fillRect(strip, selection); // flat, uniform — nothing else
            continue;
        }
        if (alternate && (row & 1))
            p.fillRect(strip, zebra);
        if (row == hoverRow)
            p.fillRect(strip, hover);
    }
}

// Common mouse-tracking behavior (mouse tracking ON).
template <typename Base>
class HoverRowBase : public Base
{
public:
    explicit HoverRowBase(QWidget* parent = nullptr)
        : Base(parent)
    {
        // Marcaj: selecția/hover se desenează ca bandă continuă pe tot rândul
        // (paintEvent), deci delegații NU trebuie să picteze selecție per-celulă
        // (ar ieși blocuri mai aprinse cu muchii verticale = „linii" de coloană).
        this->setProperty("appRowBand", true);
    }

    int hoverRow() const { return hoverRow_; }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        {
            // Row bands first, under the cells (the painter must end before
            // the base class opens its own on the same viewport).
            QPainter p(this->viewport());
            detail::paintRowStrips(this, p, hoverRow_);
        }
        Base::paintEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const int row = this->indexAt(event->pos()).row();
        if (row != hoverRow_) {
            hoverRow_ = row;
            this->viewport()->update();
        }
        Base::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override
    {
        if (hoverRow_ != -1) {
            hoverRow_ = -1;
            this->viewport()->update();
        }
        Base::leaveEvent(event);
    }

private:
    int hoverRow_ = -1;
};

} // namespace detail

// Model-based variant (QSqlQueryModel & co) — GenericTablePage and others.
using HoverRowTableView = detail::HoverRowBase<QTableView>;

// Item-based variant — Services and other pages using setItem/setCellWidget.
using HoverRowTableWidget = detail::HoverRowBase<QTableWidget>;

} // namespace app::ui
