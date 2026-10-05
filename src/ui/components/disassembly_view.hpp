#pragma once

#include <cstddef>
#include <cstdint>

#include "ui/components/disassembly_document.hpp"

#include <QAbstractScrollArea>
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
        // the user-space ceiling); Go To and the dialog's open-at address use this.
        void                        set_first_address(std::uint64_t address);
        // Parses `text` (absolute, module+RVA or a bare module name) and jumps
        // there; false when the text does not name an address.
        bool                        go_to(const QString& text);
        [[nodiscard]] std::size_t   visible_rows() const noexcept;

        // Re-fits the rows to the current viewport; the context menu calls this.
        void                  relayout();
        // The instruction text painted on the visible row `index` (0 = the top
        // row), for tests.
        [[nodiscard]] QString row_text(std::size_t index) const;

        // Fills `menu` with `Go To...` and `Copy address`. Public so tests can
        // inspect it without opening a menu.
        void populate_menu(QMenu& menu);

        // The persistent "Go To..." action of the listing. It carries no
        // shortcut - the byte view owns the window's single Ctrl+G.
        [[nodiscard]] QAction* goto_action() const noexcept;

    signals:
        void gotoRequested(); // the user picked "Go To..." from the pane's menu

    protected:
        void resizeEvent(QResizeEvent* event) override;
        void paintEvent(QPaintEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void contextMenuEvent(QContextMenuEvent* event) override;
        void showEvent(QShowEvent* event) override;
        void hideEvent(QHideEvent* event) override;

    private:
        // Re-derives the row height and column widths, then re-windows the rows.
        void recompute_layout();
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

        void copy_address();

        DisassemblyDocument& document_;

        std::uint64_t first_address_ {0};
        std::size_t   first_row_ {0};
        std::size_t   visible_rows_ {1};
        int           row_height_ {1};
        int           header_height_ {1};
        int           address_width_ {0};
        int           bytes_width_ {1};

        QAction* goto_action_ {};
    };

} // namespace slopkit::ui::components
