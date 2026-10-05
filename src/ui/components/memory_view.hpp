#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ui/components/memory_view_document.hpp"

#include <QAbstractScrollArea>
#include <QRect>
#include <QString>

class QAction;
class QLineEdit;
class QMenu;

namespace slopkit::ui::components
{

    // A hex-editor style byte view: an address column, value cells painted per
    // byte and an optional text column. It owns no process state - only the
    // layout, the scroll position and the inline editor.
    class MemoryView : public QAbstractScrollArea
    {
        Q_OBJECT

    public:
        explicit MemoryView(MemoryViewDocument& document, QWidget* parent = nullptr);

        // The top-left address; kept exactly where it is by every layout change.
        [[nodiscard]] std::uint64_t first_byte() const noexcept;
        // Jumps the top address to exactly `address` (clamped to the user-space
        // ceiling); Go To and the dialog's open-at address use this.
        void                        set_first_byte(std::uint64_t address);
        // Parses `text` (absolute, module+RVA or a bare module name) and jumps
        // there; false when the text does not name an address.
        bool                        go_to(const QString& text);
        // Jumps the top address to `address` and remembers where it left, so
        // `back()` can return there; records nothing when `address` is already
        // the top. Used by Follow / Follow in Memory View and Go To.
        void                        navigate_to(std::uint64_t address);
        // Returns to the top address remembered last; false when there is none.
        bool                        back();
        [[nodiscard]] bool          can_go_back() const noexcept;
        // Forgets the history; the dialog's open-at address starts a new one.
        void                        clear_history() noexcept;
        void                        set_text_column_visible(bool visible);
        [[nodiscard]] bool          text_column_visible() const noexcept;
        [[nodiscard]] std::size_t   bytes_per_row() const noexcept;
        [[nodiscard]] std::size_t   visible_rows() const noexcept;

        // Re-fits the rows to the current format/encoding/text-column state;
        // the right-click actions call this after changing the document.
        void                               relayout();
        // The viewport rect of the value cell at `address`, or nullopt when the
        // address is not on a visible row.
        [[nodiscard]] std::optional<QRect> cell_rect(std::uint64_t address) const;
        // The value text the cell at `address` renders (a `?` placeholder when
        // unreadable, empty when no reading covers the address).
        [[nodiscard]] QString              cell_text(std::uint64_t address) const;
        // The hex offset text painted in the column header for `column` (no `0x`).
        [[nodiscard]] QString              column_offset_text(std::size_t column) const;

        // Fills `menu` with the value formats, the text encodings and the
        // text-column toggle, checked against the document's state, and wires
        // each entry to apply itself. Public so tests can inspect it without
        // opening a menu.
        void populate_options_menu(QMenu& menu);

        // The persistent "Go To..." action (Ctrl+G, window-scoped). It is a
        // child of the view and is added to the view, so the shortcut fires
        // anywhere in the dialog's window.
        [[nodiscard]] QAction* goto_action() const noexcept;

        // The inline editor (created on first use, hidden afterwards).
        [[nodiscard]] QLineEdit* editor() const noexcept;

    signals:
        void gotoRequested(); // the user picked "Go To..." from the options menu
        void bytesEdited();   // a write was submitted
        void navigated();     // the top address moved (Follow / Back / Go To)

    protected:
        void resizeEvent(QResizeEvent* event) override;
        void paintEvent(QPaintEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void mouseDoubleClickEvent(QMouseEvent* event) override;
        void contextMenuEvent(QContextMenuEvent* event) override;
        void showEvent(QShowEvent* event) override;
        void hideEvent(QHideEvent* event) override;
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        // How many previous top addresses a pane remembers for Back.
        static constexpr std::size_t kHistoryLimit = 64;

        struct Hit
        {
            std::uint64_t address {};
            int           column {-1};
            QRect         rect;
        };

        // The value cell under `position`, or nullopt over the address/text areas.
        [[nodiscard]] std::optional<Hit> hit_test(const QPoint& position) const;

        // Re-derives the row height, the address/cell/text widths, the bytes per
        // row and the visible rows, then re-windows the document. The top
        // address is preserved across the re-fit.
        void recompute_layout();

        void setup_scrollbar();
        void recenter_scrollbar();
        void on_scroll_value(int value);
        // Moves the top address by whole rows; positive scrolls down. The
        // anchor offset is preserved across the step.
        void scroll_rows(long long delta);

        void close_editor();
        void commit_edit();

        MemoryViewDocument& document_;

        std::uint64_t              first_byte_ {0};
        // The top addresses visited through navigate_to(), oldest first.
        std::vector<std::uint64_t> history_;
        std::size_t                bytes_per_row_ {16};
        std::size_t                visible_rows_ {1};
        int                        row_height_ {1};
        int                        header_height_ {1};
        int                        address_width_ {0};
        int                        cell_width_ {1};
        int                        text_width_ {0};
        bool                       text_column_visible_ {true};
        bool                       layout_ready_ {false};

        QAction*      goto_action_ {};
        QLineEdit*    editor_ {};
        bool          editing_ {false};
        std::uint64_t edit_address_ {0};
    };

} // namespace slopkit::ui::components
