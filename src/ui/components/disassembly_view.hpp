#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ui/components/disassembly_document.hpp"

#include <QAbstractScrollArea>
#include <QPoint>
#include <QString>

class QAction;
class QMenu;

namespace slopkit::ui::components
{

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

        // Re-fits the rows to the current viewport; the context menu calls this.
        void                      relayout();
        // The instruction text painted on the visible row `index` (0 = the top
        // row), for tests.
        [[nodiscard]] QString     row_text(std::size_t index) const;
        // How many byte tokens the wrapped bytes column shows per line.
        [[nodiscard]] std::size_t bytes_per_line() const noexcept;
        // The wrapped line count of the visible row `index` (0 = the top row).
        [[nodiscard]] std::size_t row_lines(std::size_t index) const;

        // Fills `menu` with `Go To...` and a `Copy` submenu for the decoded row
        // `index`. Public so tests can inspect it without opening a menu.
        void populate_menu(QMenu& menu, std::size_t row);

        // The persistent "Go To..." action of the listing. It carries no
        // shortcut - the byte view owns the window's single Ctrl+G.
        [[nodiscard]] QAction* goto_action() const noexcept;

    signals:
        void gotoRequested();                                    // the user picked "Go To..." from the pane's menu
        void navigated();                                        // the top row moved (Follow / Back / Go To)
        void followInMemoryViewRequested(std::uint64_t address); // "Follow in Memory View"

    protected:
        void resizeEvent(QResizeEvent* event) override;
        void paintEvent(QPaintEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void contextMenuEvent(QContextMenuEvent* event) override;
        void showEvent(QShowEvent* event) override;
        void hideEvent(QHideEvent* event) override;

    private:
        // How many previous top addresses a pane remembers for Back.
        static constexpr std::size_t kHistoryLimit = 64;

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

        void setup_scrollbar();
        void recenter_scrollbar();
        void on_scroll_value(int value);
        // Moves the top row by whole instructions; positive scrolls down.
        void scroll_rows(long long delta);
        // Steps the whole aligned window by one page (+1 down, -1 up) and seats
        // the cursor on the new base, keeping the old rows until it lands.
        void step_window(int direction);

        void                      copy_row(std::size_t row, CopyFormat format);
        // The decoded row index under a viewport `position`, or the cursor row
        // when the position falls above or below every painted row.
        [[nodiscard]] std::size_t row_at_position(const QPoint& position) const;

        DisassemblyDocument& document_;

        std::uint64_t              first_address_ {0};
        // The top addresses visited through navigate_to(), oldest first.
        std::vector<std::uint64_t> history_;
        std::size_t                first_row_ {0};
        std::size_t                visible_rows_ {1};
        std::size_t                bytes_per_line_ {1};
        bool                       seat_last_row_ {false};
        int                        line_height_ {1};
        int                        header_height_ {1};
        int                        address_width_ {0};
        int                        bytes_width_ {1};

        QAction* goto_action_ {};
    };

} // namespace slopkit::ui::components
