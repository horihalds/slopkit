#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <QAbstractScrollArea>
#include <QModelIndex>
#include <QPoint>
#include <QRect>
#include <QString>

#include "ui/components/neutral_scroller.hpp"
#include "ui/models/reclass_structure_model.hpp"

class QEvent;
class QKeyEvent;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QResizeEvent;
class QWheelEvent;

namespace slopkit::ui::models
{
    class ReclassProxyModel;
    class ReclassValueModel;
}

namespace slopkit::ui::components
{
    // The node grid, painted rather than assembled out of a tree view: the seven
    // columns are sized to the text the class draws, no header band is drawn and
    // nothing about the layout can be dragged or measured again on a pass. The
    // rows come from the structure model through the fused proxy, so the widget
    // itself only lays out, paints and handles input.
    class ReClassView : public QAbstractScrollArea
    {
        Q_OBJECT

    public:
        ReClassView(models::ReclassProxyModel&     model,
                    models::ReclassStructureModel& structure,
                    models::ReclassValueModel&     values,
                    QWidget*                       parent = nullptr);

        [[nodiscard]] models::ReclassProxyModel*     model() const noexcept;
        [[nodiscard]] models::ReclassStructureModel* structure() const noexcept;

        // The cursor row; an invalid index clears it.
        [[nodiscard]] QModelIndex currentIndex() const;
        void                      setCurrentIndex(const QModelIndex& index);
        void                      scrollTo(const QModelIndex& index);

        [[nodiscard]] bool isExpanded(const QModelIndex& index) const;
        void               expand(const QModelIndex& index);
        void               collapse(const QModelIndex& index);

        [[nodiscard]] std::vector<QModelIndex> selected_rows() const;

        [[nodiscard]] std::size_t row_count() const noexcept;
        [[nodiscard]] std::size_t visible_row_count() const noexcept;

        [[nodiscard]] std::optional<QRect> cell_rect(const QModelIndex& index) const;
        [[nodiscard]] QString              cell_text(const QModelIndex& index) const;
        // The full text of the cell under the cursor, when it does not fit.
        [[nodiscard]] QString              hover_full_text(const QPoint& position) const;
        [[nodiscard]] int                  column_x(int column) const noexcept;
        [[nodiscard]] int                  column_width(int column) const noexcept;

        // The inline cell editor, or nullptr until a cell was opened.
        [[nodiscard]] QLineEdit* editor() const noexcept;
        void                     close_editor();

        // Re-measures the columns and the row height after a font or style
        // change; a live pass never calls this.
        void relayout();

    signals:
        void selectionChanged();
        void rowExpansionChanged();
        void rowMenuRequested(const QPoint& position);

        // The user clicked a hidden node's collapsed `...` row, asking for the
        // node back.
        void hiddenRowActivated(std::size_t node_id);

    protected:
        void resizeEvent(QResizeEvent* event) override;
        void changeEvent(QEvent* event) override;
        void paintEvent(QPaintEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseDoubleClickEvent(QMouseEvent* event) override;
        void contextMenuEvent(QContextMenuEvent* event) override;
        void leaveEvent(QEvent* event) override;
        bool eventFilter(QObject* watched, QEvent* event) override;

        // Paints the visible rows into `painter`; the event handler only hands it
        // the viewport's painter.
        void paint_rows(QPainter& painter) const;

    private:
        // A visible row: the model row it shows and how deep it nests.
        struct VisualRow
        {
            const models::ReclassRow* row {};
            std::size_t               depth {};
        };

        struct Hit
        {
            const models::ReclassRow* row {};
            int                       row_index {-1};
            int                       column {-1};
            bool                      on_triangle {false};
            QRect                     rect;
        };

        void update_layout();
        void update_rows();
        // Sizes every column to the text the flattened rows draw - the hints and
        // the pointer field from the kinds rather than from what a live pass
        // happens to read. A structure change, never a live pass.
        void update_content_widths();
        void update_view();
        // Places the measured columns at their on-screen x for the current pane
        // and horizontal offset, showing the bar when the row overflows the pane;
        // a resize, a scroll and a structure change all re-run this.
        void update_columns();
        void flatten(const QModelIndex& parent, std::size_t depth);
        void resolve_identity();

        [[nodiscard]] const models::ReclassRow* row_of(const QModelIndex& index) const;
        [[nodiscard]] const models::ReclassRow* row_for_node(std::size_t node_id) const;
        [[nodiscard]] int                       visual_index_of(std::size_t node_id) const;
        [[nodiscard]] QModelIndex               index_for(const models::ReclassRow& row, int column) const;
        [[nodiscard]] std::optional<Hit>        hit_test(const QPoint& position) const;
        [[nodiscard]] int                       column_at(int x) const noexcept;
        [[nodiscard]] int                       content_x(const VisualRow& entry, int column) const noexcept;
        [[nodiscard]] QRect                     cell_rect_at(int row_index, int column) const;
        [[nodiscard]] QRect                     triangle_rect(const VisualRow& entry, int row_index) const;
        [[nodiscard]] bool                      expandable(const models::ReclassRow& row) const;
        [[nodiscard]] bool                      is_selected(const models::ReclassRow& row) const;
        [[nodiscard]] bool                      is_current(const models::ReclassRow& row) const;

        void paint_cell(QPainter& painter, const VisualRow& entry, int column, const QRect& rect, bool selected) const;
        void paint_triangle(QPainter& painter, const VisualRow& entry, int row_index, bool selected) const;

        void select_row(const Hit& hit, Qt::KeyboardModifiers modifiers);
        void move_cursor(int row_index);
        void toggle_row(int row_index);
        void open_editor_at(int row_index, int column);
        void open_editor(const Hit& hit);
        void commit_edit();
        void scroll_rows(long long delta);
        void ensure_visible(int row_index);

        models::ReclassProxyModel*     model_ {};
        models::ReclassStructureModel* structure_ {};
        models::ReclassValueModel*     values_ {};

        std::vector<VisualRow> rows_;
        int                    visible_rows_ {1};
        int                    first_row_ {0};
        int                    row_height_ {1};
        int                    column_x_[models::ReclassStructureModel::column_count] {};
        int                    column_width_[models::ReclassStructureModel::column_count] {};
        // The widest `// …` note and the widest value the shown class draws;
        // update_columns() lays them out at that width and never narrows them.
        int                    comment_content_ {};
        int                    value_content_ {};

        std::vector<std::size_t> selected_;
        std::size_t              current_ {};
        bool                     has_current_ {false};
        int                      hovered_ {-1};

        QLineEdit*  editor_ {};
        bool        editing_ {false};
        bool        commit_ran_ {false};
        std::size_t edit_node_id_ {};
        int         edit_column_ {};

        NeutralScroller scroller_;
    };

}
