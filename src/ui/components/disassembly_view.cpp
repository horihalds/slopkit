#include "ui/components/disassembly_view.hpp"

#include <algorithm>
#include <limits>

#include "scan/types.hpp"
#include "ui/address_format.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QWheelEvent>

namespace slopkit::ui::components
{

    namespace
    {
        constexpr int kMargin      = 4;
        constexpr int kCellPadding = 10;
        constexpr int kColumnGap   = 10;

        // The instruction column never shrinks below this many monospace glyphs;
        // the bytes column gives up its width first and wraps instead.
        constexpr int kMinInstructionChars = 16;

        // The scrollbar is a relative band: the value is parked at the middle
        // and each move is read as a row delta, then re-centred, so an
        // unbounded instruction extent is never squeezed into the scrollbar's
        // int range.
        constexpr int kScrollNeutral = 1'000'000;

        // A `.byte` row is an undecodable byte and paints muted, like `??`.
        bool is_muted(const DisassemblyDocument::Row& row)
        {
            return !row.readable || row.text.startsWith(QLatin1String(".byte"));
        }
    } // namespace

    DisassemblyView::DisassemblyView(DisassemblyDocument& document, QWidget* parent)
        : QAbstractScrollArea(parent), document_(document)
    {
        setObjectName(QStringLiteral("disassembly_view"));
        setFocusPolicy(Qt::StrongFocus);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        viewport()->setFont(mono_font());

        recompute_layout();
        setup_scrollbar();

        connect(&document_, &DisassemblyDocument::rowsChanged, this, &DisassemblyView::on_rows_changed);

        // The listing's own menu entry. It carries no shortcut: the byte view's
        // Ctrl+G stays the window's only one and the dialog routes it by focus.
        goto_action_ = new QAction(tr("Go To..."), this);
        connect(goto_action_, &QAction::triggered, this, &DisassemblyView::gotoRequested);
    }

    std::uint64_t DisassemblyView::first_address() const noexcept
    {
        return first_address_;
    }

    std::size_t DisassemblyView::visible_rows() const noexcept
    {
        return visible_rows_;
    }

    QAction* DisassemblyView::goto_action() const noexcept
    {
        return goto_action_;
    }

    QString DisassemblyView::row_text(std::size_t index) const
    {
        return document_.row(first_row_ + index).text;
    }

    std::size_t DisassemblyView::bytes_per_line() const noexcept
    {
        return bytes_per_line_;
    }

    std::size_t DisassemblyView::row_lines(std::size_t index) const
    {
        return document_.line_count(first_row_ + index, bytes_per_line_);
    }

    void DisassemblyView::recompute_layout()
    {
        const QFontMetrics metrics(mono_font());
        line_height_   = std::max(1, metrics.height() + 4);
        header_height_ = line_height_;

        // The bytes column width and the row fit both follow from the decoded
        // rows, so decode the cursor's neighbourhood first.
        ensure_cursor_decoded();
        update_columns();
        update_visible_rows();
    }

    void DisassemblyView::update_columns()
    {
        const QFontMetrics metrics(mono_font());

        // The address column follows the widest address on screen, but never
        // narrower than the 16-digit absolute form so it does not jitter.
        QString       address_sample = QStringLiteral("0x0000000000000000");
        const QString top_text       = document_.address_text(first_address_);
        if (top_text.size() > address_sample.size())
        {
            address_sample = top_text;
        }
        address_width_ = metrics.horizontalAdvance(address_sample) + kCellPadding;

        // The bytes column wants the widest instruction's raw bytes, but gives
        // up its width first: it shrinks down to one byte token and wraps, and
        // the instruction column keeps a guaranteed glyph budget.
        const int glyph = std::max(1, metrics.horizontalAdvance(QLatin1Char('0')));
        const int natural_bytes =
            metrics.horizontalAdvance(QString(static_cast<qsizetype>(document_.bytes_width()), QLatin1Char('0')))
            + kCellPadding;
        const int min_bytes = metrics.horizontalAdvance(QStringLiteral("00")) + kCellPadding;
        const int room      = viewport()->width() - 2 * kMargin - address_width_ - 2 * kColumnGap;
        bytes_width_ = std::clamp(natural_bytes, min_bytes, std::max(min_bytes, room - kMinInstructionChars * glyph));

        // `n` byte tokens need `3n - 1` characters; take the largest count whose
        // rendered width still fits the column, and never fewer than one.
        const int         content_width = std::max(0, bytes_width_ - kCellPadding);
        const std::size_t max_tokens    = std::max<std::size_t>(1, (document_.bytes_width() + 1) / 3);
        bytes_per_line_                 = 1;
        for (std::size_t tokens = 1; tokens <= max_tokens; ++tokens)
        {
            const int width =
                metrics.horizontalAdvance(QString(static_cast<qsizetype>(3 * tokens - 1), QLatin1Char('0')));
            if (width <= content_width)
            {
                bytes_per_line_ = tokens;
            }
        }
    }

    void DisassemblyView::update_visible_rows()
    {
        const int available = std::max(0, viewport()->height() - kMargin - header_height_);

        // Sum full post-wrap row heights from the top row; the first row always
        // counts even when it is taller than the viewport. Rows past the decoded
        // tail are counted as one line each so the decode budget grows with the
        // viewport instead of with the last decode pass.
        int         used = 0;
        std::size_t rows = 0;
        for (std::size_t index = first_row_;; ++index)
        {
            const std::size_t lines  = index < document_.row_count()
                                         ? std::max<std::size_t>(1, document_.line_count(index, bytes_per_line_))
                                         : 1;
            const int         height = static_cast<int>(lines) * line_height_;
            if (rows > 0 && used + height > available)
            {
                break;
            }
            used += height;
            ++rows;
        }
        visible_rows_ = std::max<std::size_t>(1, rows);

        if (QScrollBar* bar = verticalScrollBar(); bar != nullptr)
        {
            bar->setPageStep(static_cast<int>(visible_rows_));
        }
    }

    void DisassemblyView::ensure_cursor_decoded()
    {
        document_.set_view(first_address_, visible_rows_);
        const std::uint64_t offset = first_address_ - document_.window_base();
        document_.ensure_rows(static_cast<std::size_t>(offset) + visible_rows_ + DisassemblyDocument::kDecodeSlack);
        if (const auto index = document_.row_at(first_address_); index.has_value())
        {
            first_row_ = *index;
        }
    }

    void DisassemblyView::on_rows_changed()
    {
        ensure_cursor_decoded();

        // A step up lands on the previous window's last decoded row once its
        // payload is in; the sweep is bounded to one window.
        if (seat_last_row_)
        {
            seat_last_row_ = false;
            document_.ensure_rows(std::numeric_limits<std::size_t>::max());
            scroll_to_row(document_.row_count() == 0 ? 0 : document_.row_count() - 1);
            return;
        }

        update_columns();
        update_visible_rows();
        viewport()->update();
    }

    void DisassemblyView::relayout()
    {
        recompute_layout();
        viewport()->update();
    }

    void DisassemblyView::setup_scrollbar()
    {
        QScrollBar* bar = verticalScrollBar();
        bar->setRange(0, 2 * kScrollNeutral);
        bar->setSingleStep(1);
        bar->setPageStep(static_cast<int>(visible_rows_));
        bar->setTracking(true);
        {
            const QSignalBlocker block(bar);
            bar->setValue(kScrollNeutral);
        }
        connect(bar, &QScrollBar::valueChanged, this, &DisassemblyView::on_scroll_value);
    }

    void DisassemblyView::recenter_scrollbar()
    {
        QScrollBar* bar = verticalScrollBar();
        if (bar == nullptr)
        {
            return;
        }
        const QSignalBlocker block(bar);
        bar->setValue(kScrollNeutral);
    }

    void DisassemblyView::on_scroll_value(int value)
    {
        const int delta = value - kScrollNeutral;
        if (delta == 0)
        {
            return;
        }
        recenter_scrollbar();
        scroll_rows(delta);
    }

    void DisassemblyView::scroll_rows(long long delta)
    {
        if (delta == 0 || document_.row_count() == 0)
        {
            return;
        }

        const long long last   = static_cast<long long>(document_.row_count()) - 1;
        long long       target = static_cast<long long>(first_row_) + delta;
        if (target < 0)
        {
            // Before the first decoded row: cross to the previous window when
            // already at its start, else clamp inside this one.
            if (first_row_ == 0)
            {
                step_window(-1);
                return;
            }
            target = 0;
        }
        if (target > last)
        {
            // Decode ahead before clamping, so a page step can grow the rows.
            document_.ensure_rows(static_cast<std::size_t>(target) + visible_rows_ + DisassemblyDocument::kDecodeSlack);
            const long long grown = static_cast<long long>(document_.row_count()) - 1;
            if (target > grown)
            {
                // Past the last decoded row of an exhausted window: continue on
                // the next page rather than dead-ending.
                if (document_.window_exhausted())
                {
                    step_window(1);
                    return;
                }
                target = grown;
            }
        }
        scroll_to_row(static_cast<std::size_t>(std::max(0LL, target)));
    }

    void DisassemblyView::step_window(int direction)
    {
        const std::uint64_t step = DisassemblyDocument::kWindowSize;
        const std::uint64_t base = document_.window_base();
        if (direction < 0)
        {
            if (base < step)
            {
                return; // Bottom of the window range.
            }
            seat_last_row_ = true;
            first_address_ = base - step;
        }
        else
        {
            first_address_ = std::min(base + step, scan::kMaxUserAddress);
        }

        recenter_scrollbar();
        document_.set_view(first_address_, visible_rows_);
        viewport()->update(); // Keep the previous rows painted until the payload lands.
    }

    void DisassemblyView::scroll_to_row(std::size_t index)
    {
        if (document_.row_count() == 0)
        {
            return;
        }
        first_row_     = std::min(index, document_.row_count() - 1);
        first_address_ = document_.row(first_row_).address;
        recenter_scrollbar();
        ensure_cursor_decoded();
        update_columns();
        update_visible_rows();
        viewport()->update();
    }

    void DisassemblyView::set_first_address(std::uint64_t address)
    {
        first_address_ = std::min(address, scan::kMaxUserAddress);
        recenter_scrollbar();
        ensure_cursor_decoded();
        update_columns();
        update_visible_rows();
        viewport()->update();
    }

    bool DisassemblyView::go_to(const QString& text)
    {
        const auto parsed = ui::parse_address_text(text.toStdString(), document_.module_spans());
        if (!parsed.has_value())
        {
            return false;
        }
        set_first_address(*parsed);
        return true;
    }

    void DisassemblyView::copy_address()
    {
        QApplication::clipboard()->setText(document_.address_text(first_address_));
    }

    void DisassemblyView::populate_menu(QMenu& menu)
    {
        menu.addAction(goto_action_);
        menu.addAction(tr("Copy address"), this, &DisassemblyView::copy_address);
    }

    void DisassemblyView::resizeEvent(QResizeEvent* event)
    {
        QAbstractScrollArea::resizeEvent(event);
        recompute_layout();
        viewport()->update();
    }

    void DisassemblyView::showEvent(QShowEvent* event)
    {
        QAbstractScrollArea::showEvent(event);
        document_.set_visible(true);
        recompute_layout();
    }

    void DisassemblyView::hideEvent(QHideEvent* event)
    {
        QAbstractScrollArea::hideEvent(event);
        document_.set_visible(false);
    }

    void DisassemblyView::paintEvent(QPaintEvent* event)
    {
        Q_UNUSED(event);
        QPainter painter(viewport());
        painter.setFont(mono_font());

        const Theme& theme = active_theme();
        painter.fillRect(viewport()->rect(), theme.surface);

        const int address_left = kMargin;
        const int bytes_left   = address_left + address_width_ + kColumnGap;
        const int text_left    = bytes_left + bytes_width_ + kColumnGap;
        const int text_width   = std::max(0, viewport()->width() - text_left - kMargin);

        painter.setPen(theme.text_muted);
        painter.drawText(QRect(address_left, kMargin, address_width_, header_height_),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         tr("Address"));
        painter.drawText(
            QRect(bytes_left, kMargin, bytes_width_, header_height_), Qt::AlignLeft | Qt::AlignVCenter, tr("Bytes"));
        painter.drawText(
            QRect(text_left, kMargin, text_width, header_height_), Qt::AlignLeft | Qt::AlignVCenter, tr("Instruction"));

        int y = kMargin + header_height_;
        for (std::size_t visible = 0; visible < visible_rows_; ++visible)
        {
            const std::size_t index = first_row_ + visible;
            if (index >= document_.row_count())
            {
                break;
            }
            const DisassemblyDocument::Row row = document_.row(index);
            const std::size_t lines            = std::max<std::size_t>(1, document_.line_count(index, bytes_per_line_));
            const int         row_height       = static_cast<int>(lines) * line_height_;

            painter.setPen(theme.text_muted);
            painter.drawText(QRect(address_left, y, address_width_, line_height_),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             document_.address_text(row.address));

            painter.setPen(is_muted(row) ? theme.text_muted : theme.text);
            painter.drawText(QRect(text_left, y, text_width, line_height_), Qt::AlignLeft | Qt::AlignVCenter, row.text);

            // The bytes wrap down their own column, one token group per line.
            for (std::size_t line = 0; line < lines; ++line)
            {
                const int line_y = y + static_cast<int>(line) * line_height_;
                painter.drawText(QRect(bytes_left, line_y, bytes_width_, line_height_),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 document_.byte_line(index, line, bytes_per_line_));
            }

            y += row_height;
        }
    }

    void DisassemblyView::wheelEvent(QWheelEvent* event)
    {
        const int steps = event->angleDelta().y() / 120;
        if (steps != 0)
        {
            scroll_rows(-static_cast<long long>(steps) * 3);
            event->accept();
            return;
        }
        QAbstractScrollArea::wheelEvent(event);
    }

    void DisassemblyView::keyPressEvent(QKeyEvent* event)
    {
        switch (event->key())
        {
        case Qt::Key_Up:
            scroll_rows(-1);
            break;
        case Qt::Key_Down:
            scroll_rows(1);
            break;
        case Qt::Key_PageUp:
            scroll_rows(-static_cast<long long>(visible_rows_));
            break;
        case Qt::Key_PageDown:
            scroll_rows(static_cast<long long>(visible_rows_));
            break;
        case Qt::Key_Home:
            if (document_.row_count() == 0)
            {
                document_.ensure_rows(1);
            }
            scroll_to_row(0);
            break;
        case Qt::Key_End:
            document_.ensure_rows(std::numeric_limits<std::size_t>::max());
            scroll_to_row(document_.row_count() == 0 ? 0 : document_.row_count() - 1);
            break;
        default:
            QAbstractScrollArea::keyPressEvent(event);
            return;
        }
        event->accept();
    }

    void DisassemblyView::contextMenuEvent(QContextMenuEvent* event)
    {
        QMenu menu(this);
        populate_menu(menu);
        menu.exec(event->globalPos());
    }

} // namespace slopkit::ui::components
