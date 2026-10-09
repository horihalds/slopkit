#include "ui/components/disassembly_view.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

#include "scan/types.hpp"
#include "ui/address_format.hpp"
#include "ui/components/row_menu.hpp"
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
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QToolTip>
#include <QWheelEvent>

namespace slopkit::ui::components
{

    namespace
    {
        constexpr int kMargin      = 4;
        constexpr int kCellPadding = 10;

        // The gap between an instruction's text and its `NOPed:` annotation.
        constexpr int kMarkerGap = 8;

        // The gap between the address, bytes and instruction columns: one shared
        // value, so both boundaries tighten together and the header stays aligned
        // with the rows painted below it.
        constexpr int kColumnGap = 6;

        // The instruction column never shrinks below this many monospace glyphs;
        // the bytes column gives up its width first and wraps instead.
        constexpr int kMinInstructionChars = 16;

        // A `.byte` row is an undecodable byte and paints muted, like `??`.
        bool is_muted(const DisassemblyDocument::Row& row)
        {
            return !row.readable || row.text.startsWith(QLatin1String(".byte"));
        }
    } // namespace

    DisassemblyView::DisassemblyView(DisassemblyDocument& document, QWidget* parent)
        : QAbstractScrollArea(parent), document_(document), scroller_(*this,
                                                                      [this](long long delta)
                                                                      {
                                                                          scroll_rows(delta);
                                                                      })
    {
        setObjectName(QStringLiteral("disassembly_view"));
        setFocusPolicy(Qt::StrongFocus);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        viewport()->setFont(mono_font());
        viewport()->setMouseTracking(true);

        recompute_layout();

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

    std::optional<std::uint64_t> DisassemblyView::selected_address() const noexcept
    {
        return selected_address_;
    }

    void DisassemblyView::set_selected_address(std::optional<std::uint64_t> address)
    {
        if (selected_address_ == address)
        {
            return;
        }
        selected_address_ = address;
        viewport()->update();
        emit selectionChanged(selected_address_);
    }

    int DisassemblyView::address_width() const noexcept
    {
        return address_width_;
    }

    int DisassemblyView::bytes_width() const noexcept
    {
        return bytes_width_;
    }

    QString DisassemblyView::row_text(std::size_t index) const
    {
        return document_.row(first_row_ + index).text;
    }

    QColor syntax_colour(SegmentKind kind, const Theme& theme, const QColor& base)
    {
        switch (kind)
        {
        case SegmentKind::cpu_register:
            return theme.syntax_register;
        case SegmentKind::immediate:
            return theme.syntax_immediate;
        case SegmentKind::module:
            return theme.syntax_module;
        case SegmentKind::plain:
            return base;
        }
        return base;
    }

    std::vector<PaintedSegment> DisassemblyView::row_segments(std::size_t index) const
    {
        const DisassemblyDocument::Row row   = document_.row(first_row_ + index);
        const Theme&                   theme = active_theme();
        const QColor                   base  = document_.patch_at(first_row_ + index) != nullptr
                                                 ? theme.warning
                                                 : (is_muted(row) ? theme.text_muted : theme.text);

        std::vector<PaintedSegment> painted;
        painted.reserve(row.segments.size());
        for (const RowSegment& segment : row.segments)
        {
            painted.push_back({segment.text, syntax_colour(segment.kind, theme, base), segment.full});
        }
        return painted;
    }

    QString DisassemblyView::hover_full_text(const QPoint& position) const
    {
        if (position.y() < kMargin + header_height_)
        {
            return {}; // The header band holds no row.
        }

        // Mirror paintEvent's column geometry.
        const int address_left = kMargin;
        const int bytes_left   = address_left + address_width_ + kColumnGap;
        const int text_left    = bytes_left + bytes_width_ + kColumnGap;
        const int text_width   = std::max(0, viewport()->width() - text_left - kMargin);

        int y = kMargin + header_height_;
        for (std::size_t visible = 0; visible < visible_rows_; ++visible)
        {
            const std::size_t index = first_row_ + visible;
            if (index >= document_.row_count())
            {
                break;
            }
            const std::size_t lines      = std::max<std::size_t>(1, document_.line_count(index, bytes_per_line_));
            const int         row_height = static_cast<int>(lines) * line_height_;
            if (position.y() >= y + row_height)
            {
                y += row_height;
                continue;
            }

            const DisassemblyDocument::Row row = document_.row(index);
            // The address column: the full text only when it shortened.
            if (position.x() >= address_left && position.x() < address_left + address_width_)
            {
                const QString full = document_.full_address_text(row.address);
                return full != document_.address_text(row.address) ? full : QString {};
            }
            // The instruction column: the run printed under the cursor.
            if (position.x() >= text_left && position.x() < text_left + text_width)
            {
                const QFontMetrics metrics(mono_font());
                int                x = text_left;
                for (const RowSegment& run : row.segments)
                {
                    if (x >= text_left + text_width)
                    {
                        break;
                    }
                    if (position.x() < x + metrics.horizontalAdvance(run.text))
                    {
                        return run.full;
                    }
                    x += metrics.horizontalAdvance(run.text);
                }
            }
            return {};
        }
        return {}; // Below every painted row.
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
        const int          glyph = std::max(1, metrics.horizontalAdvance(QLatin1Char('0')));

        // The address column follows the widest address on screen, but never
        // narrower than the 16-digit absolute form so it does not jitter. A
        // dragged width replaces the automatic one and is clamped so that the
        // bytes column keeps one token and the instruction column its glyphs.
        QString       address_sample = QStringLiteral("0000000000000000");
        const QString top_text       = document_.address_text(first_address_);
        if (top_text.size() > address_sample.size())
        {
            address_sample = top_text;
        }
        const int min_address = metrics.horizontalAdvance(QStringLiteral("0000000000000000")) + kCellPadding;
        const int min_bytes   = metrics.horizontalAdvance(QStringLiteral("00")) + kCellPadding;
        if (address_width_user_ > 0)
        {
            const int address_max =
                viewport()->width() - 2 * kMargin - 2 * kColumnGap - min_bytes - kMinInstructionChars * glyph;
            address_width_ = std::clamp(address_width_user_, min_address, std::max(min_address, address_max));
        }
        else
        {
            address_width_ = metrics.horizontalAdvance(address_sample) + kCellPadding;
        }

        // The bytes column wants the widest instruction's raw bytes, but gives
        // up its width first: it shrinks down to one byte token and wraps, and
        // the instruction column keeps a guaranteed glyph budget. A dragged
        // width replaces the natural one and is clamped to the same bounds.
        const int natural_bytes =
            metrics.horizontalAdvance(QString(static_cast<qsizetype>(document_.bytes_width()), QLatin1Char('0')))
            + kCellPadding;
        const int room      = viewport()->width() - 2 * kMargin - address_width_ - 2 * kColumnGap;
        const int bytes_max = std::max(min_bytes, room - kMinInstructionChars * glyph);
        bytes_width_ = std::clamp(bytes_width_user_ > 0 ? bytes_width_user_ : natural_bytes, min_bytes, bytes_max);

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

        scroller_.note_visible_rows(visible_rows_);
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

    void DisassemblyView::scroll_rows(long long delta)
    {
        if (delta == 0 || document_.row_count() == 0)
        {
            return;
        }
        QToolTip::hideText(); // The row under the cursor is about to move.

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

        scroller_.recenter();
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
        QToolTip::hideText();
        scroller_.recenter();
        ensure_cursor_decoded();
        update_columns();
        update_visible_rows();
        viewport()->update();
    }

    void DisassemblyView::set_first_address(std::uint64_t address)
    {
        set_selected_address(std::nullopt);
        first_address_ = std::min(address, scan::kMaxUserAddress);
        scroller_.recenter();
        ensure_cursor_decoded();
        update_columns();
        update_visible_rows();
        viewport()->update();
    }

    bool DisassemblyView::go_to(const QString& text)
    {
        const auto parsed =
            ui::parse_address_text(text.toStdString(), document_.module_spans(), document_.symbols().snapshot());
        if (!parsed.has_value())
        {
            return false;
        }
        navigate_to(*parsed);
        return true;
    }

    void DisassemblyView::navigate_to(std::uint64_t address)
    {
        const std::uint64_t target = std::min(address, scan::kMaxUserAddress);
        if (!history_.record(first_address_, target))
        {
            return;
        }
        set_first_address(address);
        emit navigated();
    }

    bool DisassemblyView::back()
    {
        const std::optional<std::uint64_t> previous = history_.back();
        if (!previous.has_value())
        {
            return false;
        }
        set_first_address(*previous);
        emit navigated();
        return true;
    }

    bool DisassemblyView::can_go_back() const noexcept
    {
        return history_.can_go_back();
    }

    void DisassemblyView::clear_history() noexcept
    {
        history_.clear();
    }

    std::size_t DisassemblyView::row_at_position(const QPoint& position) const
    {
        if (position.y() < kMargin + header_height_)
        {
            return first_row_; // The header, or above the first row.
        }

        int y = kMargin + header_height_;
        for (std::size_t visible = 0; visible < visible_rows_; ++visible)
        {
            const std::size_t index = first_row_ + visible;
            if (index >= document_.row_count())
            {
                break;
            }
            const std::size_t lines      = std::max<std::size_t>(1, document_.line_count(index, bytes_per_line_));
            const int         row_height = static_cast<int>(lines) * line_height_;
            if (position.y() < y + row_height)
            {
                return index;
            }
            y += row_height;
        }
        return first_row_; // Empty space below the rows: the cursor row.
    }

    void DisassemblyView::select_row_at(const QPoint& position)
    {
        std::optional<std::uint64_t> address;
        if (position.y() >= kMargin + header_height_)
        {
            int y = kMargin + header_height_;
            for (std::size_t visible = 0; visible < visible_rows_; ++visible)
            {
                const std::size_t index = first_row_ + visible;
                if (index >= document_.row_count())
                {
                    break;
                }
                const std::size_t lines      = std::max<std::size_t>(1, document_.line_count(index, bytes_per_line_));
                const int         row_height = static_cast<int>(lines) * line_height_;
                if (position.y() < y + row_height)
                {
                    const DisassemblyDocument::Row row = document_.row(index);
                    if (!is_muted(row))
                    {
                        address = row.address;
                    }
                    break;
                }
                y += row_height;
            }
        }
        set_selected_address(address);
    }

    DisassemblyView::HeaderDrag DisassemblyView::boundary_at(const QPoint& position) const
    {
        if (position.y() >= kMargin + header_height_)
        {
            return HeaderDrag::none;
        }
        const int address_boundary = kMargin + address_width_;
        const int bytes_boundary   = address_boundary + kColumnGap + bytes_width_;
        if (std::abs(position.x() - address_boundary) <= kColumnGap)
        {
            return HeaderDrag::address;
        }
        if (std::abs(position.x() - bytes_boundary) <= kColumnGap)
        {
            return HeaderDrag::bytes;
        }
        return HeaderDrag::none;
    }

    void DisassemblyView::apply_drag(const QPoint& position)
    {
        if (header_drag_ == HeaderDrag::none)
        {
            return;
        }
        const int width = std::max(1, drag_origin_width_ + position.x() - drag_origin_x_);
        if (header_drag_ == HeaderDrag::address)
        {
            address_width_user_ = width;
        }
        else
        {
            bytes_width_user_ = width;
        }
        recompute_layout();
        viewport()->update();
    }

    void DisassemblyView::copy_row(std::size_t row, CopyFormat format)
    {
        const QString text = document_.copy_text(row, format);
        if (!text.isEmpty())
        {
            QApplication::clipboard()->setText(text);
        }
    }

    void DisassemblyView::populate_menu(QMenu& menu, std::size_t row)
    {
        widgets::show_explanations(menu);

        menu.addAction(goto_action_);

        // A row whose instruction references an address can be followed: to the
        // listing's own cursor, or to the byte view. The address is captured by
        // value - the span only lives until the window moves.
        if (const auto references = document_.row_addresses(row); !references.empty())
        {
            const std::uint64_t target = references.front().address;
            connect(menu.addAction(tr("Follow")),
                    &QAction::triggered,
                    this,
                    [this, target]
                    {
                        navigate_to(target);
                    });
            connect(menu.addAction(tr("Follow in Memory View")),
                    &QAction::triggered,
                    this,
                    [this, target]
                    {
                        emit followInMemoryViewRequested(target);
                    });
        }

        // Always present so Back is discoverable, disabled while the history is
        // empty.
        QAction* back_action = menu.addAction(tr("Back"));
        back_action->setEnabled(can_go_back());
        connect(back_action,
                &QAction::triggered,
                this,
                [this]
                {
                    back();
                });

        // Rewriting live code and putting it back: one click each, the log and
        // the warning-coloured rows are the feedback.
        menu.addSeparator();
        if (const CodePatch* replaced = document_.patch_at(row); replaced != nullptr)
        {
            QAction* restore = widgets::described_action(
                menu,
                tr("Restore Original Instruction at %1").arg(document_.address_text(replaced->begin)),
                tr("Write the instruction this session replaced (%1) back.").arg(replaced->original_text));
            connect(restore,
                    &QAction::triggered,
                    this,
                    [this, row]
                    {
                        emit restoreRequested(row);
                    });
        }
        else
        {
            const bool decodable = document_.editable(row);

            QAction* nop = decodable ? widgets::described_action(
                                           menu, tr("NOP Instruction"), tr("Replace this instruction with NOP bytes."))
                                     : widgets::disabled_action(menu,
                                                                tr("NOP Instruction"),
                                                                tr("Only a decoded instruction can be replaced."));
            connect(nop,
                    &QAction::triggered,
                    this,
                    [this, row]
                    {
                        emit nopRequested(row);
                    });

            QAction* edit =
                decodable ? widgets::described_action(
                                menu, tr("Edit Instruction..."), tr("Rewrite this instruction with assembler text."))
                          : widgets::disabled_action(
                                menu, tr("Edit Instruction..."), tr("Only a decoded instruction can be edited."));
            connect(edit,
                    &QAction::triggered,
                    this,
                    [this, row]
                    {
                        emit editRequested(row);
                    });
        }

        menu.addSeparator();

        QMenu*     copy     = menu.addMenu(tr("Copy"));
        const auto add_copy = [this, copy, row](const QString& label, CopyFormat format)
        {
            connect(copy->addAction(label),
                    &QAction::triggered,
                    this,
                    [this, row, format]
                    {
                        copy_row(row, format);
                    });
        };
        add_copy(tr("Address (module + RVA)"), CopyFormat::module_relative);
        add_copy(tr("Address (absolute)"), CopyFormat::absolute);
        add_copy(tr("Bytes"), CopyFormat::bytes);
        add_copy(tr("Instruction"), CopyFormat::instruction);
        add_copy(tr("Address + bytes"), CopyFormat::address_and_bytes);
        add_copy(tr("Address + instruction"), CopyFormat::address_and_instruction);
        add_copy(tr("Address + bytes + instruction"), CopyFormat::address_bytes_instruction);

        menu.addSeparator();
        const bool resolvable = !document_.row_memory(row).empty();
        QAction*   accesses =
            resolvable
                ? widgets::described_action(menu,
                                            tr("Find out what addresses this instruction accesses"),
                                            tr("Resolve this instruction's memory operands from live registers; the "
                                               "debugger is attached first (after a confirmation) when no session is "
                                               "running."))
                : widgets::disabled_action(menu,
                                           tr("Find out what addresses this instruction accesses"),
                                           tr("Only an instruction with a memory operand can be "
                                              "resolved."));
        connect(accesses,
                &QAction::triggered,
                this,
                [this, row]
                {
                    emit instructionAccessesRequested(row);
                });
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

            // The selected instruction carries a subtle band, so the target of
            // `Toggle Breakpoint` is obvious; the address, not a pixel,
            // remembers it across scrolls.
            if (selected_address_.has_value() && *selected_address_ == row.address)
            {
                painter.fillRect(QRect(0, y, viewport()->width(), row_height), theme.surface_hover);
            }

            painter.setPen(theme.text_muted);
            painter.drawText(QRect(address_left, y, address_width_, line_height_),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             document_.address_text(row.address));

            const bool   patched = document_.patch_at(index) != nullptr;
            const QColor base    = patched ? theme.warning : (is_muted(row) ? theme.text_muted : theme.text);
            if (row.segments.empty())
            {
                painter.setPen(base);
                painter.drawText(
                    QRect(text_left, y, text_width, line_height_), Qt::AlignLeft | Qt::AlignVCenter, row.text);
            }
            else
            {
                int x = text_left;
                for (const RowSegment& run : row.segments)
                {
                    const int remaining = std::max(0, text_left + text_width - x);
                    if (remaining == 0)
                    {
                        break;
                    }
                    painter.setPen(syntax_colour(run.kind, theme, base));
                    painter.drawText(QRect(x, y, remaining, line_height_), Qt::AlignLeft | Qt::AlignVCenter, run.text);
                    x += painter.fontMetrics().horizontalAdvance(run.text);
                }
            }
            // The run walk leaves the pen on its last colour; the annotation and
            // the bytes column are not syntax-highlighted, so restore the row's
            // own colour before drawing them.
            painter.setPen(base);
            if (const QString marker = document_.row_annotation(index); !marker.isEmpty())
            {
                const int used = painter.fontMetrics().horizontalAdvance(row.text) + kMarkerGap;
                painter.drawText(QRect(text_left + used, y, std::max(0, text_width - used), line_height_),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 marker);
            }

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

    void DisassemblyView::mousePressEvent(QMouseEvent* event)
    {
        const Qt::MouseButton button = event->button();
        if (button == Qt::LeftButton || button == Qt::RightButton)
        {
            const QPoint position = event->position().toPoint();
            if (button == Qt::LeftButton)
            {
                const HeaderDrag boundary = boundary_at(position);
                if (boundary != HeaderDrag::none)
                {
                    header_drag_       = boundary;
                    drag_origin_x_     = position.x();
                    drag_origin_width_ = boundary == HeaderDrag::address ? address_width_ : bytes_width_;
                    event->accept();
                    return;
                }
            }
            // A left or right press targets the row underneath; a right press
            // selects it so the menu and the toggle agree even before the menu
            // opens.
            select_row_at(position);
            event->accept();
            return;
        }
        QAbstractScrollArea::mousePressEvent(event);
    }

    void DisassemblyView::mouseMoveEvent(QMouseEvent* event)
    {
        const QPoint position = event->position().toPoint();
        if (header_drag_ != HeaderDrag::none)
        {
            apply_drag(position);
            event->accept();
            return;
        }

        const HeaderDrag boundary = boundary_at(position);
        if (boundary != hover_boundary_)
        {
            hover_boundary_ = boundary;
            if (boundary == HeaderDrag::none)
            {
                viewport()->unsetCursor();
            }
            else
            {
                viewport()->setCursor(Qt::SplitHCursor);
            }
        }

        // Reveal the untruncated address a shortened run or address hides;
        // hide the tooltip everywhere else, so it cannot outlive the row.
        if (const QString full = hover_full_text(position); !full.isEmpty())
        {
            QToolTip::showText(event->globalPosition().toPoint(), full, viewport());
        }
        else
        {
            QToolTip::hideText();
        }
        QAbstractScrollArea::mouseMoveEvent(event);
    }

    void DisassemblyView::mouseReleaseEvent(QMouseEvent* event)
    {
        if (header_drag_ != HeaderDrag::none && event->button() == Qt::LeftButton)
        {
            header_drag_ = HeaderDrag::none;
            event->accept();
            return;
        }
        QAbstractScrollArea::mouseReleaseEvent(event);
    }

    void DisassemblyView::mouseDoubleClickEvent(QMouseEvent* event)
    {
        if (event->button() == Qt::LeftButton)
        {
            const std::size_t row = row_at_position(event->position().toPoint());
            if (document_.editable(row))
            {
                emit editRequested(row);
                event->accept();
                return;
            }
        }
        QAbstractScrollArea::mouseDoubleClickEvent(event);
    }

    void DisassemblyView::leaveEvent(QEvent* event)
    {
        hover_boundary_ = HeaderDrag::none;
        viewport()->unsetCursor();
        QToolTip::hideText();
        QAbstractScrollArea::leaveEvent(event);
    }

    void DisassemblyView::contextMenuEvent(QContextMenuEvent* event)
    {
        const QPoint position = viewport()->mapFromGlobal(event->globalPos());
        // The menu acts on the row it targets, so selecting it first keeps the
        // menu and the control bar's toggle in agreement.
        select_row_at(position);
        QMenu menu(this);
        populate_menu(menu, row_at_position(position));
        menu.exec(event->globalPos());
    }

} // namespace slopkit::ui::components
