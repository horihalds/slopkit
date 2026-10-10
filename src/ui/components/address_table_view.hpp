#pragma once

#include <QPoint>
#include <QRect>
#include <QTableView>

class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QMimeData;
class QPaintEvent;

namespace slopkit::ui::components
{

    // The address list's table view. It adds one gesture to a plain QTableView: a
    // drag onto the middle of a row nests the dragged row under it, while a drag
    // on a row's top or bottom edge inserts in front of (respectively after) it.
    // The resolution lives here so both the feedback and the drop agree, and the
    // actual move is left to the model's drop path.
    class AddressTableView : public QTableView
    {
        Q_OBJECT

    public:
        // Where a drag would land. With `nest`, the dragged row becomes a child
        // of the row at `row`; otherwise it is inserted in front of `row`, so
        // `row == rowCount()` appends.
        struct DropTarget
        {
            int  row {};
            bool nest {};
        };

        // The fraction of a row's height that acts as its insert edge; the middle
        // nests. In percent so the comparison stays integral.
        static constexpr int kEdgeZonePercent = 25;

        explicit AddressTableView(QWidget* parent = nullptr);

        // Resolves a cursor, in viewport coordinates, against a row's rectangle:
        // the top edge band inserts in front of the row, the bottom edge band
        // after it (`row + 1`) and the middle nests under it.
        [[nodiscard]] static DropTarget resolve_drop(const QRect& row_rect, int row, const QPoint& position);

    protected:
        void dragMoveEvent(QDragMoveEvent* event) override;
        void dragLeaveEvent(QDragLeaveEvent* event) override;
        void dropEvent(QDropEvent* event) override;
        void paintEvent(QPaintEvent* event) override;

    private:
        [[nodiscard]] DropTarget drop_target_at(const QPoint& position) const;
        [[nodiscard]] bool       can_drop(const DropTarget& target, const QMimeData* data) const;

        int  drag_row_ {-1}; // the nest parent, or the row to insert in front of
        bool drag_nest_ {false};
        bool has_drag_ {false};
    };

} // namespace slopkit::ui::components
