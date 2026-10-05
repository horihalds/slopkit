#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "ui/components/memory_view_document.hpp"

#include <QAbstractScrollArea>
#include <QRect>
#include <QString>

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

        [[nodiscard]] std::uint64_t first_byte() const noexcept;
        void                        set_first_byte(std::uint64_t address); // Go / open at
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
        // The value text the cell at `address` renders (empty when unreadable).
        [[nodiscard]] QString              cell_text(std::uint64_t address) const;

        // Fills `menu` with the value formats, the text encodings and the
        // text-column toggle, checked against the document's state, and wires
        // each entry to apply itself. Public so tests can inspect it without
        // opening a menu.
        void populate_options_menu(QMenu& menu);

        // The inline editor (created on first use, hidden afterwards).
        [[nodiscard]] QLineEdit* editor() const noexcept;

    signals:
        void firstByteChanged(std::uint64_t address); // the address box follows
        void bytesEdited();                           // a write was submitted

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
        struct Hit
        {
            std::uint64_t address {};
            int           column {-1};
            QRect         rect;
        };

        // The value cell under `position`, or nullopt over the address/text areas.
        [[nodiscard]] std::optional<Hit> hit_test(const QPoint& position) const;

        // Re-derives the row height, the address/cell/text widths, the bytes per
        // row and the visible rows, then re-windows the document.
        void recompute_layout();

        void setup_scrollbar();
        void recenter_scrollbar();
        void on_scroll_value(int value);
        // Moves the top address by whole rows; positive scrolls down.
        void scroll_rows(long long delta);
        void align_first_byte();

        void close_editor();
        void commit_edit();

        MemoryViewDocument& document_;

        std::uint64_t first_byte_ {0};
        std::size_t   bytes_per_row_ {16};
        std::size_t   visible_rows_ {1};
        int           row_height_ {1};
        int           address_width_ {0};
        int           cell_width_ {1};
        int           text_width_ {0};
        bool          text_column_visible_ {true};
        bool          layout_ready_ {false};

        QLineEdit*    editor_ {};
        bool          editing_ {false};
        std::uint64_t edit_address_ {0};
    };

} // namespace slopkit::ui::components
