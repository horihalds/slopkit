#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ui/components/disassembly_document.hpp"
#include "ui/components/navigation_history.hpp"
#include "ui/components/neutral_scroller.hpp"
#include "ui/theme.hpp"

#include <QAbstractScrollArea>
#include <QColor>
#include <QPoint>
#include <QString>

class QAction;
class QMenu;
class QMouseEvent;

namespace slopkit::ui::components
{

    // One run of the instruction text and the colour it is painted in.
    struct PaintedSegment
    {
        QString text;
        QColor  colour;
        // The untruncated `<module>+<RVA>` when the run is a shortened module
        // label; empty otherwise, so a run that was not shortened reveals
        // nothing on hover.
        QString full {};
    };

    // The colour one run is painted with: the syntax role for a classified run
    // and `base` (the row's own colour) for a plain one. Shared by paintEvent and
    // row_segments(), so the two can never disagree.
    [[nodiscard]] QColor syntax_colour(SegmentKind kind, const Theme& theme, const QColor& base);

    // The instruction listing: an address, byte and instruction column painted
    // from the document's decoded rows. It owns no process state - only the
    // scroll position and the menu, mirroring MemoryView for the byte view.
    class DisassemblyView : public QAbstractScrollArea
    {
        Q_OBJECT

    public:
        explicit DisassemblyView(DisassemblyDocument& document, QWidget* parent = nullptr);

        // The address of the top row; the pane's own cursor.
        [[nodiscard]] std::uint64_t first_address() const noexcept;
        // Jumps the top row to the instruction containing `address` (clamped to
        // the user-space ceiling) without touching the history; the dialog's
        // open-at address and internal re-fits use this.
        void                        set_first_address(std::uint64_t address);
        // Parses `text` (absolute, module+RVA or a bare module name) and jumps
        // there; false when the text does not name an address.
        bool                        go_to(const QString& text);
        // Jumps the top row to `address` and remembers where it left, so
        // `back()` can return there; records nothing when `address` is already
        // the top. Follow and Go To use this.
        void                        navigate_to(std::uint64_t address);
        // Returns to the top address remembered last; false when there is none.
        bool                        back();
        [[nodiscard]] bool          can_go_back() const noexcept;
        // Forgets the history; the dialog's open-at address starts a new one.
        void                        clear_history() noexcept;
        [[nodiscard]] std::size_t   visible_rows() const noexcept;

        // The selected instruction, or std::nullopt when nothing is selected.
        [[nodiscard]] std::optional<std::uint64_t> selected_address() const noexcept;
        void                                       set_selected_address(std::optional<std::uint64_t> address);

        // The current column widths; the tests read them to check a drag.
        [[nodiscard]] int address_width() const noexcept;
        [[nodiscard]] int bytes_width() const noexcept;

        // Re-fits the rows to the current viewport; the context menu calls this.
        void                                      relayout();
        // The instruction text painted on the visible row `index` (0 = the top
        // row), for tests.
        [[nodiscard]] QString                     row_text(std::size_t index) const;
        // The coloured runs the listing paints on the visible row `index`
        // (0 = the top row), for tests.
        [[nodiscard]] std::vector<PaintedSegment> row_segments(std::size_t index) const;
        // The untruncated `<module>+<RVA>` a hover at viewport `position` should
        // reveal, or an empty string when the surface there was not shortened
        // (or no row is under the cursor). Public for tests, like
        // `row_segments`.
        [[nodiscard]] QString                     hover_full_text(const QPoint& position) const;
        // How many byte tokens the wrapped bytes column shows per line.
        [[nodiscard]] std::size_t                 bytes_per_line() const noexcept;
        // The wrapped line count of the visible row `index` (0 = the top row).
        [[nodiscard]] std::size_t                 row_lines(std::size_t index) const;

        // Fills `menu` with `Go To...` and a `Copy` submenu for the decoded row
        // `index`. Public so tests can inspect it without opening a menu.
        void populate_menu(QMenu& menu, std::size_t row);

        // The persistent "Go To..." action of the listing. It carries no
        // shortcut - the byte view owns the window's single Ctrl+G.
        [[nodiscard]] QAction* goto_action() const noexcept;

    signals:
        void gotoRequested(); // the user picked "Go To..." from the pane's menu
        // The selected row changed (including a jump that clears it).
        void selectionChanged(std::optional<std::uint64_t> address);
        void navigated();                                        // the top row moved (Follow / Back / Go To)
        void followInMemoryViewRequested(std::uint64_t address); // "Follow in Memory View"
        // "Find out what addresses this instruction accesses" on the decoded row.
        void instructionAccessesRequested(std::size_t row);
        // "NOP Instruction" on the decoded row.
        void nopRequested(std::size_t row);
        // "Edit Instruction..." on the decoded row, or a double-click on it.
        void editRequested(std::size_t row);
        // "Hook Instruction..." on the decoded row.
        void hookRequested(std::size_t row);
        // "Restore Original Instruction" on a row the session replaced.
        void restoreRequested(std::size_t row);

    protected:
        void resizeEvent(QResizeEvent* event) override;
        void paintEvent(QPaintEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void mouseDoubleClickEvent(QMouseEvent* event) override;
        void contextMenuEvent(QContextMenuEvent* event) override;
        void leaveEvent(QEvent* event) override;
        void showEvent(QShowEvent* event) override;
        void hideEvent(QHideEvent* event) override;

    private:
        // A draggable column boundary: the address column's right edge or the
        // bytes column's one.
        enum class HeaderDrag
        {
            none,
            address,
            bytes
        };

        // Re-derives the line height, column widths and the row fit, then
        // re-windows the rows.
        void recompute_layout();
        // Re-derives the address and wrapped bytes column widths.
        void update_columns();
        // Counts the rows that fit the viewport from first_row_ and updates the
        // scrollbar page step.
        void update_visible_rows();
        // Ensures the row containing first_address_ is decoded and points
        // first_row_ at it.
        void ensure_cursor_decoded();
        // Puts the document back in sync after its bytes or settings changed.
        void on_rows_changed();
        // Moves the cursor to the decoded row `index` (clamped) and repaints.
        void scroll_to_row(std::size_t index);

        // Moves the top row by whole instructions; positive scrolls down.
        void scroll_rows(long long delta);
        // Steps the whole aligned window by one page (+1 down, -1 up) and seats
        // the cursor on the new base, keeping the old rows until it lands.
        void step_window(int direction);

        void                      copy_row(std::size_t row, CopyFormat format);
        // The decoded row index under a viewport `position`, or the cursor row
        // when the position falls above or below every painted row.
        [[nodiscard]] std::size_t row_at_position(const QPoint& position) const;
        // Targets the row under a viewport `position`, clearing the selection
        // when it falls on the header, the empty tail or an undecodable row.
        void                      select_row_at(const QPoint& position);
        // The draggable column boundary under a viewport `position`, inside the
        // header band; `none` everywhere else.
        [[nodiscard]] HeaderDrag  boundary_at(const QPoint& position) const;
        // Reshapes the dragged column to follow the cursor and repaints.
        void                      apply_drag(const QPoint& position);

        DisassemblyDocument& document_;

        std::uint64_t     first_address_ {0};
        NavigationHistory history_;
        std::size_t       first_row_ {0};
        std::size_t       visible_rows_ {1};
        std::size_t       bytes_per_line_ {1};
        bool              seat_last_row_ {false};
        int               line_height_ {1};
        int               header_height_ {1};
        int               address_width_ {0};
        int               bytes_width_ {1};

        // User-chosen widths; 0 keeps the automatic width for that column.
        int        address_width_user_ {0};
        int        bytes_width_user_ {0};
        HeaderDrag header_drag_ {HeaderDrag::none};
        HeaderDrag hover_boundary_ {HeaderDrag::none};
        int        drag_origin_x_ {0};
        int        drag_origin_width_ {0};

        std::optional<std::uint64_t> selected_address_;

        QAction* goto_action_ {};

        // Owns the vertical bar and re-centres it after every jump or re-window.
        NeutralScroller scroller_;
    };

} // namespace slopkit::ui::components
