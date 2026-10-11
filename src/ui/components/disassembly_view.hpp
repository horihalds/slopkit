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
    struct PaintedSegment
    {
        QString text;
        QColor  colour;
        QString full {};
    };

    [[nodiscard]] QColor syntax_colour(SegmentKind kind, const Theme& theme, const QColor& base);

    class DisassemblyView : public QAbstractScrollArea
    {
        Q_OBJECT

    public:
        explicit DisassemblyView(DisassemblyDocument& document, QWidget* parent = nullptr);

        [[nodiscard]] std::uint64_t first_address() const noexcept;
        void                        set_first_address(std::uint64_t address);
        bool                        go_to(const QString& text);
        void                        navigate_to(std::uint64_t address);
        bool                        back();
        [[nodiscard]] bool          can_go_back() const noexcept;
        void                        clear_history() noexcept;
        [[nodiscard]] std::size_t   visible_rows() const noexcept;

        [[nodiscard]] std::optional<std::uint64_t> selected_address() const noexcept;
        void                                       set_selected_address(std::optional<std::uint64_t> address);

        [[nodiscard]] int address_width() const noexcept;
        [[nodiscard]] int bytes_width() const noexcept;

        void                                      relayout();
        [[nodiscard]] QString                     row_text(std::size_t index) const;
        [[nodiscard]] std::vector<PaintedSegment> row_segments(std::size_t index) const;
        [[nodiscard]] QString                     hover_full_text(const QPoint& position) const;
        [[nodiscard]] std::size_t                 bytes_per_line() const noexcept;
        [[nodiscard]] std::size_t                 row_lines(std::size_t index) const;

        void populate_menu(QMenu& menu, std::size_t row);

        [[nodiscard]] QAction* goto_action() const noexcept;

    signals:
        void gotoRequested();
        void selectionChanged(std::optional<std::uint64_t> address);
        void navigated();
        void followInMemoryViewRequested(std::uint64_t address);
        void instructionAccessesRequested(std::size_t row);
        void nopRequested(std::size_t row);
        void editRequested(std::size_t row);
        void hookRequested(std::size_t row);
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
        enum class HeaderDrag
        {
            none,
            address,
            bytes
        };

        void recompute_layout();
        void update_columns();
        void update_visible_rows();
        void ensure_cursor_decoded();
        void on_rows_changed();
        void scroll_to_row(std::size_t index);

        void scroll_rows(long long delta);
        void step_window(int direction);

        void                      copy_row(std::size_t row, CopyFormat format);
        [[nodiscard]] std::size_t row_at_position(const QPoint& position) const;
        void                      select_row_at(const QPoint& position);
        [[nodiscard]] HeaderDrag  boundary_at(const QPoint& position) const;
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

        int        address_width_user_ {0};
        int        bytes_width_user_ {0};
        HeaderDrag header_drag_ {HeaderDrag::none};
        HeaderDrag hover_boundary_ {HeaderDrag::none};
        int        drag_origin_x_ {0};
        int        drag_origin_width_ {0};

        std::optional<std::uint64_t> selected_address_;

        QAction* goto_action_ {};

        NeutralScroller scroller_;
    };

}
