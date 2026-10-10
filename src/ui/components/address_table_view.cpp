#include "ui/components/address_table_view.hpp"

#include <QAbstractItemModel>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QWidget>

namespace slopkit::ui::components
{
    namespace
    {
        // The nest band's opacity: readable over the row without hiding its text.
        constexpr int kNestBandAlpha = 0x60;
    } // namespace

    AddressTableView::AddressTableView(QWidget* parent) : QTableView(parent) {}

    AddressTableView::DropTarget AddressTableView::resolve_drop(const QRect& row_rect, int row, const QPoint& position)
    {
        const int height = row_rect.height();
        if (height <= 0)
        {
            return {.row = row, .nest = false};
        }

        const int offset = position.y() - row_rect.top();
        if (offset * 100 < height * kEdgeZonePercent)
        {
            return {.row = row, .nest = false};
        }
        if (offset * 100 > height * (100 - kEdgeZonePercent))
        {
            return {.row = row + 1, .nest = false};
        }
        return {.row = row, .nest = true};
    }

    AddressTableView::DropTarget AddressTableView::drop_target_at(const QPoint& position) const
    {
        const QModelIndex index = indexAt(position);
        if (index.isValid())
        {
            return resolve_drop(visualRect(index), index.row(), position);
        }
        // Below the last row: append at the last row's level.
        return {.row = model() != nullptr ? model()->rowCount() : 0, .nest = false};
    }

    bool AddressTableView::can_drop(const DropTarget& target, const QMimeData* data) const
    {
        if (model() == nullptr)
        {
            return false;
        }
        if (target.nest)
        {
            const QModelIndex parent = model()->index(target.row, 0);
            return model()->canDropMimeData(data, Qt::MoveAction, -1, 0, parent);
        }
        return model()->canDropMimeData(data, Qt::MoveAction, target.row, 0, QModelIndex());
    }

    void AddressTableView::dragMoveEvent(QDragMoveEvent* event)
    {
        // Let the toolkit set up its own insertion indicator, then refine the
        // target: an edge band inserts in front of the row, the middle nests.
        QTableView::dragMoveEvent(event);

        const DropTarget target = drop_target_at(event->position().toPoint());
        if (!can_drop(target, event->mimeData()))
        {
            drag_row_  = -1;
            drag_nest_ = false;
            has_drag_  = false;
            setDropIndicatorShown(true);
            event->ignore();
            viewport()->update();
            return;
        }

        drag_row_  = target.row;
        drag_nest_ = target.nest;
        has_drag_  = true;
        // A nest paints its own band below; an edge insert keeps the toolkit's
        // own insertion line.
        setDropIndicatorShown(!target.nest);
        event->acceptProposedAction();
        viewport()->update();
    }

    void AddressTableView::dragLeaveEvent(QDragLeaveEvent* event)
    {
        QTableView::dragLeaveEvent(event);
        drag_row_  = -1;
        drag_nest_ = false;
        has_drag_  = false;
        setDropIndicatorShown(true);
        viewport()->update();
    }

    void AddressTableView::dropEvent(QDropEvent* event)
    {
        const DropTarget target = drop_target_at(event->position().toPoint());
        drag_row_               = -1;
        drag_nest_              = false;
        has_drag_               = false;
        setDropIndicatorShown(true);

        bool dropped = false;
        if (model() != nullptr)
        {
            // The model's own drop path reads the mime payload and resolves the
            // move, so a drop made here and one made by the toolkit agree.
            if (target.nest)
            {
                const QModelIndex parent = model()->index(target.row, 0);
                dropped                  = model()->dropMimeData(event->mimeData(), Qt::MoveAction, -1, 0, parent);
            }
            else
            {
                dropped = model()->dropMimeData(event->mimeData(), Qt::MoveAction, target.row, 0, QModelIndex());
            }
        }

        if (dropped)
        {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        }
        else
        {
            event->ignore();
        }
        viewport()->update();
    }

    void AddressTableView::paintEvent(QPaintEvent* event)
    {
        QTableView::paintEvent(event);
        if (!has_drag_ || !drag_nest_ || model() == nullptr)
        {
            return;
        }

        const QModelIndex index = model()->index(drag_row_, 0);
        if (!index.isValid())
        {
            return;
        }
        const QRect cell = visualRect(index);
        QColor      band = palette().color(QPalette::Highlight);
        band.setAlpha(kNestBandAlpha);
        QPainter painter(viewport());
        painter.fillRect(QRect(0, cell.top(), viewport()->width(), cell.height()), band);
    }

} // namespace slopkit::ui::components
