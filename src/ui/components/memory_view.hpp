#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "ui/components/memory_view_document.hpp"
#include "ui/components/navigation_history.hpp"
#include "ui/components/neutral_scroller.hpp"

#include <QAbstractScrollArea>
#include <QRect>
#include <QString>

class QAction;
class QLineEdit;
class QMenu;
class QMouseEvent;

namespace slopkit::ui::components
{
    class MemoryView : public QAbstractScrollArea
    {
        Q_OBJECT

    public:
        explicit MemoryView(MemoryViewDocument& document, QWidget* parent = nullptr);

        [[nodiscard]] std::uint64_t first_byte() const noexcept;
        void                        set_first_byte(std::uint64_t address);
        bool                        go_to(const QString& text);
        void                        navigate_to(std::uint64_t address);
        bool                        back();
        [[nodiscard]] bool          can_go_back() const noexcept;
        void                        clear_history() noexcept;
        void                        set_text_column_visible(bool visible);
        [[nodiscard]] bool          text_column_visible() const noexcept;
        [[nodiscard]] std::size_t   bytes_per_row() const noexcept;
        [[nodiscard]] std::size_t   visible_rows() const noexcept;

        void                               relayout();
        [[nodiscard]] std::optional<QRect> cell_rect(std::uint64_t address) const;
        [[nodiscard]] QString              cell_text(std::uint64_t address) const;
        [[nodiscard]] QString              column_offset_text(std::size_t column) const;
        [[nodiscard]] QString              hover_full_text(const QPoint& position) const;

        void populate_options_menu(QMenu& menu);

        [[nodiscard]] QAction* goto_action() const noexcept;

        [[nodiscard]] QLineEdit* editor() const noexcept;

    signals:
        void gotoRequested();
        void bytesEdited();
        void navigated();

    protected:
        void resizeEvent(QResizeEvent* event) override;
        void paintEvent(QPaintEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseDoubleClickEvent(QMouseEvent* event) override;
        void leaveEvent(QEvent* event) override;
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

        [[nodiscard]] std::optional<Hit> hit_test(const QPoint& position) const;

        void recompute_layout();

        void scroll_rows(long long delta);

        void close_editor();
        void commit_edit();

        MemoryViewDocument& document_;

        std::uint64_t     first_byte_ {0};
        NavigationHistory history_;
        std::size_t       bytes_per_row_ {16};
        std::size_t       visible_rows_ {1};
        int               row_height_ {1};
        int               header_height_ {1};
        int               address_width_ {0};
        int               cell_width_ {1};
        int               text_width_ {0};
        bool              text_column_visible_ {true};
        bool              layout_ready_ {false};

        QAction*      goto_action_ {};
        QLineEdit*    editor_ {};
        bool          editing_ {false};
        std::uint64_t edit_address_ {0};

        NeutralScroller scroller_;
    };

}
