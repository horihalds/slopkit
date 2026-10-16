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
    // The node grid, painted rather than assembled out of a tree view: the three
    // leading fields are sized to the text the class draws, the rest of the row
    // flows after them, no header band is drawn and nothing about the layout can
    // be dragged or measured again on a pass. The rows come from the structure
    // model through the fused proxy, so the widget itself only lays out, paints
    // and handles input.
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
        // The rect the `byte_index`th byte of a cell draws in, when that cell
        // offers a per-byte editor: a pair of a hex dump or a character of the
        // Ascii column.
        [[nodiscard]] std::optional<QRect> byte_rect(const QModelIndex& index, std::size_t byte_index) const;
        // The rect the `[N]` run of a Type cell draws in, when the cell offers
        // one: a text node's length or an array's element count.
        [[nodiscard]] std::optional<QRect> extent_rect(const QModelIndex& index) const;

        // The rects an array's Type cell draws its index control in: the arrow
        // that steps back, the bracketed index the editor takes over and the
        // arrow that steps on. Nothing when the row offers no control.
        struct ElementRects
        {
            QRect previous;
            QRect index;
            QRect next;
        };

        [[nodiscard]] std::optional<ElementRects> element_rects(const QModelIndex& index) const;
        [[nodiscard]] QString                     cell_text(const QModelIndex& index) const;
        // The full text of the cell under the cursor, when it does not fit.
        [[nodiscard]] QString                     hover_full_text(const QPoint& position) const;

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

        // A digit pressed on the grid: insert that many bytes at the cursor row,
        // the same command the row menu's `Insert Bytes` entries run.
        void insertBytesRequested(std::size_t count);

        // `Ctrl` and 1, 2, 4 or 8 on the grid: retype the cursor row as the hex
        // node of that byte size, the command `Change Type` runs.
        void changeKindRequested(reclass::NodeKind kind);

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
            // The byte of a hex dump or of the Ascii column the point stands
            // for, when a double-click resolved one; -1 for a whole field.
            int                       byte_index {-1};
            // The point fell on the Type cell's bracketed run, which the editor
            // opens on and which the commit routes to the structure model.
            bool                      extent {false};
            // The point fell on the array's bracketed index between the two
            // arrows, which the editor opens on.
            bool                      element {false};
            // The step the arrow under the point takes - back or on - and zero
            // when no arrow is under it.
            int                       element_step {0};
        };

        // Where each of the row's nine fields stands once the row's own content
        // has been placed: the offset, address and ascii fields on their measured
        // columns, the type, name and value in the one content slot, and the
        // note, the readings and the pointer immediately behind the value they
        // annotate.
        struct RowLayout
        {
            QRect rects[models::ReclassStructureModel::column_count];
        };

        void update_layout();
        void update_rows();
        // Sizes every column to the text the flattened rows draw - the hints and
        // the pointer field from the kinds rather than from what a live pass
        // happens to read. A structure change, never a live pass.
        void update_content_widths();
        // Re-measures the horizontal bar's span from the text the flattened rows
        // draw now: the widest row's last drawn field. A structure change and a
        // moved live cell both re-run this.
        void measure_drawn_extent();
        void update_view();
        // Places the measured columns at their on-screen x for the current pane
        // and horizontal offset, showing the bar whenever the widest drawn row
        // overflows the pane; a resize, a scroll and a structure change all
        // re-run this.
        void update_columns();
        void flatten(const QModelIndex& parent, std::size_t depth);
        void resolve_identity();

        [[nodiscard]] const models::ReclassRow* row_of(const QModelIndex& index) const;
        [[nodiscard]] const models::ReclassRow* row_for_node(std::size_t node_id) const;
        [[nodiscard]] int                       visual_index_of(std::size_t node_id) const;
        [[nodiscard]] QModelIndex               index_for(const models::ReclassRow& row, int column) const;
        [[nodiscard]] std::optional<Hit>        hit_test(const QPoint& position) const;
        // The width one flowing field draws now: its display text's advance in
        // the face the paint uses, capped by the column's measured width; nothing
        // for an empty field. layout_row() and the bar's span share it.
        [[nodiscard]] int                       flowed_field_width(const VisualRow& entry, int column) const;
        // The end of the last field a row draws, measured from the head of the
        // content slot; nothing when the row draws no flowing field.
        [[nodiscard]] int                       drawn_flow_end(const VisualRow& entry) const;
        // Lays a row's nine fields out for the row's own text: the three aligned
        // fields on their measured columns, then type, name, value, comment,
        // hints and pointer flowing one behind the other from the head of the
        // content slot.
        [[nodiscard]] RowLayout                 layout_row(const VisualRow& entry, int row_index) const;
        // The field a point inside a row falls on: an empty field is skipped, a
        // point past the row's last field belongs to that field.
        [[nodiscard]] int                       field_at(const RowLayout& layout, int x) const noexcept;
        // The x the text of an aligned field starts at, right-aligned on the
        // column's content box; a flowing field starts on its own rect.
        [[nodiscard]] int                       aligned_x(const VisualRow& entry, int column) const noexcept;
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
        void open_byte_editor(int row_index, int column, int byte_index);
        // Opens the one editor over the Type cell's bracketed run, anchored on
        // the rect the run draws in.
        void open_extent_editor(int row_index, const QRect& run);
        // Opens the one editor over the array's bracketed index, anchored on the
        // rect the number draws in.
        void open_element_editor(int row_index, const QRect& run);
        // The rect the `byte_index`th byte of a value cell's hex dump draws in,
        // or of the character the Ascii column draws for it; nothing when the
        // cell elides its text, so no byte sits at a fixed place.
        [[nodiscard]] std::optional<QRect>
        byte_rect_in(const VisualRow& entry, int row_index, int column, std::size_t byte_index) const;
        // The rect the Type cell's `[N]` run draws in, when the row offers one
        // and the text it sits in is drawn whole.
        [[nodiscard]] std::optional<QRect>
        extent_rect_in(const VisualRow& entry, int row_index, const QPoint& position) const;
        // The rects the array's index control draws in, when the row offers one
        // and the text it sits in is drawn whole: the arrow that steps back, the
        // bracketed index the editor takes over and the arrow that steps on.
        [[nodiscard]] std::optional<ElementRects> element_rects_in(const VisualRow& entry, int row_index) const;
        // What a hover over an array's arrow says: the element that arrow shows,
        // counted from one.
        [[nodiscard]] QString                     element_tooltip(const models::ReclassRow& row, int delta) const;
        // The text the editor opens with: the byte the row draws, bare and upper
        // case, for a per-byte edit, and the model's own text for a whole cell.
        [[nodiscard]] QString                     edit_text(const models::ReclassRow& row) const;
        // The byte a point of a cell stands for - the dump pair or the Ascii
        // character under it - clamped to the bytes the cell covers; -1 when the
        // cell draws no byte there.
        [[nodiscard]] int byte_at(const VisualRow& entry, int row_index, int column, const QPoint& position) const;
        void              commit_edit();
        void              scroll_rows(long long delta);
        void              ensure_visible(int row_index);

        models::ReclassProxyModel*     model_ {};
        models::ReclassStructureModel* structure_ {};
        models::ReclassValueModel*     values_ {};

        std::vector<VisualRow> rows_;
        int                    visible_rows_ {1};
        int                    first_row_ {0};
        int                    row_height_ {1};
        int                    column_x_[models::ReclassStructureModel::column_count] {};
        // The measured width of an aligned field, or the cap a flowing field is
        // clipped at; update_content_widths() owns every entry.
        int                    column_width_[models::ReclassStructureModel::column_count] {};
        // The head of the row's content slot, right after the ascii field, and
        // the widest row's last drawn field - the horizontal scrollbar's span.
        int                    content_x_ {};
        int                    content_width_ {};
        // Whether the horizontal bar is up, so a range change can tell a
        // visibility flip from a span-only change.
        bool                   bar_up_ {};

        std::vector<std::size_t> selected_;
        std::size_t              current_ {};
        bool                     has_current_ {false};
        int                      hovered_ {-1};

        // What an open editor edits: one cell, or one byte of a hex dump or of
        // the Ascii column when `byte_index` is not -1, or the Type cell's
        // bracketed run when `extent` is set, or the array's bracketed index
        // when `element` is set.
        struct EditTarget
        {
            std::size_t node_id {};
            int         column {};
            int         byte_index {-1};
            bool        extent {false};
            bool        element {false};
            QRect       rect;
        };

        QLineEdit* editor_ {};
        bool       editing_ {false};
        EditTarget target_ {};

        NeutralScroller scroller_;
    };

}
