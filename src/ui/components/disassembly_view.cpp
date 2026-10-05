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

    void DisassemblyView::recompute_layout()
    {
        const QFontMetrics metrics(mono_font());
        row_height_    = std::max(1, metrics.height() + 4);
        header_height_ = row_height_;

        visible_rows_ =
            std::max<std::size_t>(1,
                                  static_cast<std::size_t>(std::max(0, viewport()->height() - header_height_))
                                      / static_cast<std::size_t>(row_height_));

        // The address column follows the widest address on screen, but never
        // narrower than the 16-digit absolute form so it does not jitter.
        QString       address_sample = QStringLiteral("0x0000000000000000");
        const QString top_text       = document_.address_text(first_address_);
        if (top_text.size() > address_sample.size())
        {
            address_sample = top_text;
        }
        address_width_ = metrics.horizontalAdvance(address_sample) + kCellPadding;

        // The byte column holds the widest instruction's raw bytes.
        bytes_width_ =
            metrics.horizontalAdvance(QString(static_cast<qsizetype>(document_.bytes_width()), QLatin1Char('0')))
            + kCellPadding;

        ensure_cursor_decoded();
        if (verticalScrollBar() != nullptr)
        {
            verticalScrollBar()->setPageStep(static_cast<int>(visible_rows_));
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
            target = 0;
        }
        if (target > last)
        {
            // Decode ahead before clamping, so a page step can grow the rows.
            document_.ensure_rows(static_cast<std::size_t>(target) + visible_rows_ + DisassemblyDocument::kDecodeSlack);
            target = std::min(target, static_cast<long long>(document_.row_count()) - 1);
        }
        scroll_to_row(static_cast<std::size_t>(std::max(0LL, target)));
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
        document_.set_view(first_address_, visible_rows_);
        viewport()->update();
    }

    void DisassemblyView::set_first_address(std::uint64_t address)
    {
        first_address_ = std::min(address, scan::kMaxUserAddress);
        recenter_scrollbar();
        document_.set_view(first_address_, visible_rows_);
        ensure_cursor_decoded();
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

            painter.setPen(theme.text_muted);
            painter.drawText(QRect(address_left, y, address_width_, row_height_),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             document_.address_text(row.address));

            painter.setPen(is_muted(row) ? theme.text_muted : theme.text);
            painter.drawText(
                QRect(bytes_left, y, bytes_width_, row_height_), Qt::AlignLeft | Qt::AlignVCenter, row.bytes);
            painter.drawText(QRect(text_left, y, text_width, row_height_), Qt::AlignLeft | Qt::AlignVCenter, row.text);

            y += row_height_;
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
