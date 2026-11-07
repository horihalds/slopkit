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
    class AddressTableView : public QTableView
    {
        Q_OBJECT

    public:
        struct DropTarget
        {
            int  row {};
            bool nest {};
        };

        static constexpr int kEdgeZonePercent = 25;

        explicit AddressTableView(QWidget* parent = nullptr);

        [[nodiscard]] static DropTarget resolve_drop(const QRect& row_rect, int row, const QPoint& position);

    protected:
        void dragMoveEvent(QDragMoveEvent* event) override;
        void dragLeaveEvent(QDragLeaveEvent* event) override;
        void dropEvent(QDropEvent* event) override;
        void paintEvent(QPaintEvent* event) override;

    private:
        [[nodiscard]] DropTarget drop_target_at(const QPoint& position) const;
        [[nodiscard]] bool       can_drop(const DropTarget& target, const QMimeData* data) const;

        int  drag_row_ {-1};
        bool drag_nest_ {false};
        bool has_drag_ {false};
    };

}
