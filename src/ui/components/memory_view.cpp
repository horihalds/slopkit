#include "ui/components/memory_view.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <QAction>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QWheelEvent>

#include "scan/types.hpp"
#include "ui/address_format.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui::components
{

    namespace
    {
        constexpr int kMargin          = 4;
        constexpr int kGapAfterAddress = 8;
        constexpr int kCellPadding     = 10;
        constexpr int kTextGap         = 10;
        constexpr int kTextPadding     = 8;

        // A worst-case cell text for the current format, so the auto-fitted
        // cells stay wide enough for every value the format can produce.
        QString cell_sample(ValueFormat format)
        {
            switch (format.type)
            {
            case scan::ValueType::byte:
                return format.hex ? QStringLiteral("00") : QStringLiteral("-128");
            case scan::ValueType::int16:
                return format.hex ? QStringLiteral("0000") : QStringLiteral("-32768");
            case scan::ValueType::int32:
                return format.hex ? QStringLiteral("00000000") : QStringLiteral("-2147483648");
            case scan::ValueType::int64:
                return format.hex ? QStringLiteral("0000000000000000") : QStringLiteral("-9223372036854775808");
            case scan::ValueType::float32:
                return QStringLiteral("-1.23457e+38");
            case scan::ValueType::float64:
                return QStringLiteral("-1.23457e+308");
            default:
                break;
            }
            return QStringLiteral("00");
        }
    } // namespace

    MemoryView::MemoryView(MemoryViewDocument& document, QWidget* parent)
        : QAbstractScrollArea(parent), document_(document), scroller_(*this,
                                                                      [this](long long delta)
                                                                      {
                                                                          scroll_rows(delta);
                                                                      })
    {
        setObjectName(QStringLiteral("memory_view"));
        setFocusPolicy(Qt::StrongFocus);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        viewport()->setFont(mono_font());

        recompute_layout();

        connect(&document_,
                &MemoryViewDocument::repaintRequested,
                this,
                [this]
                {
                    viewport()->update();
                });

        goto_action_ = new QAction(tr("Go To..."), this);
        goto_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+G")));
        goto_action_->setShortcutContext(Qt::WindowShortcut);
        addAction(goto_action_);
        connect(goto_action_, &QAction::triggered, this, &MemoryView::gotoRequested);
    }

    QAction* MemoryView::goto_action() const noexcept
    {
        return goto_action_;
    }

    std::uint64_t MemoryView::first_byte() const noexcept
    {
        return first_byte_;
    }

    bool MemoryView::text_column_visible() const noexcept
    {
        return text_column_visible_;
    }

    std::size_t MemoryView::bytes_per_row() const noexcept
    {
        return bytes_per_row_;
    }

    std::size_t MemoryView::visible_rows() const noexcept
    {
        return visible_rows_;
    }

    QLineEdit* MemoryView::editor() const noexcept
    {
        return editor_;
    }

    void MemoryView::relayout()
    {
        close_editor(); // A format or encoding change invalidates an open cell.
        recompute_layout();
        viewport()->update();
    }

    std::optional<QRect> MemoryView::cell_rect(std::uint64_t address) const
    {
        if (bytes_per_row_ == 0 || row_height_ <= 0 || address < first_byte_)
        {
            return std::nullopt;
        }
        const std::uint64_t delta = address - first_byte_;
        const std::uint64_t row   = delta / bytes_per_row_;
        if (row >= visible_rows_)
        {
            return std::nullopt;
        }

        const std::size_t size   = std::max<std::size_t>(1, document_.format().size());
        const std::size_t cells  = bytes_per_row_ / size;
        const std::size_t column = static_cast<std::size_t>(delta % bytes_per_row_) / size;
        if (column >= cells)
        {
            return std::nullopt;
        }

        return QRect(kMargin + address_width_ + kGapAfterAddress + static_cast<int>(column) * cell_width_,
                     kMargin + header_height_ + static_cast<int>(row) * row_height_,
                     cell_width_,
                     row_height_);
    }

    QString MemoryView::cell_text(std::uint64_t address) const
    {
        return document_.cell(address).text;
    }

    QString MemoryView::column_offset_text(std::size_t column) const
    {
        const std::size_t format_bytes = std::max<std::size_t>(1, document_.format().size());
        const std::size_t offset       = column * format_bytes;
        const std::size_t span         = bytes_per_row_ >= format_bytes ? bytes_per_row_ - format_bytes : 0;

        // Room for the largest offset on the row, but at least two digits.
        std::size_t digits = 1;
        for (std::size_t value = span; value >= 16; value /= 16)
        {
            ++digits;
        }
        digits = std::max<std::size_t>(2, digits);

        return QString::number(offset, 16).rightJustified(static_cast<qsizetype>(digits), QLatin1Char('0')).toUpper();
    }

    void MemoryView::recompute_layout()
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
        const QString top_text       = document_.address_text(first_byte_);
        if (top_text.size() > address_sample.size())
        {
            address_sample = top_text;
        }
        address_width_ = metrics.horizontalAdvance(address_sample) + kCellPadding;

        const ValueFormat format          = document_.format();
        const std::size_t format_bytes    = std::max<std::size_t>(1, format.size());
        const std::size_t bytes_per_glyph = document_.encoding() == TextEncoding::utf16 ? 2 : 1;

        cell_width_ = std::max(1, metrics.horizontalAdvance(cell_sample(format)) + kCellPadding);

        const int available   = std::max(0, viewport()->width() - address_width_ - kGapAfterAddress - 2 * kMargin);
        const int glyph_width = std::max(1, metrics.horizontalAdvance(QLatin1Char('0')));
        const int per_cell_text =
            static_cast<int>((format_bytes / bytes_per_glyph) * static_cast<std::size_t>(glyph_width));

        std::size_t cells = 1;
        if (text_column_visible_)
        {
            const int room  = available - kTextGap - kTextPadding;
            const int denom = cell_width_ + per_cell_text;
            cells           = room >= denom && denom > 0 ? static_cast<std::size_t>(room / denom) : 1;
            text_width_     = static_cast<int>(cells) * per_cell_text + kTextPadding;
        }
        else
        {
            cells       = static_cast<std::size_t>(std::max(1, available / cell_width_));
            text_width_ = 0;
        }

        const std::size_t new_bytes_per_row = std::max<std::size_t>(1, cells * format_bytes);
        if (!layout_ready_ || new_bytes_per_row != bytes_per_row_)
        {
            // Keep the top address where it is and lay the new row grid out
            // from it, so a re-fit never moves the view.
            bytes_per_row_ = new_bytes_per_row;
        }
        layout_ready_ = true;

        scroller_.note_visible_rows(visible_rows_);
        document_.set_view(first_byte_, bytes_per_row_, visible_rows_);
    }

    void MemoryView::scroll_rows(long long delta)
    {
        if (delta == 0 || bytes_per_row_ == 0)
        {
            return;
        }

        if (delta < 0)
        {
            const std::uint64_t step = static_cast<std::uint64_t>(-delta) * bytes_per_row_;
            first_byte_              = step >= first_byte_ ? 0 : first_byte_ - step;
        }
        else
        {
            const std::uint64_t step = static_cast<std::uint64_t>(delta) * bytes_per_row_;
            // Saturate at the top of the user address space so the add cannot wrap.
            first_byte_ = scan::kMaxUserAddress - first_byte_ < step ? scan::kMaxUserAddress : first_byte_ + step;
        }
        close_editor();
        document_.set_view(first_byte_, bytes_per_row_, visible_rows_);
        viewport()->update();
    }

    void MemoryView::set_first_byte(std::uint64_t address)
    {
        // The top address is kept exactly as requested, clamped to the ceiling.
        first_byte_ = std::min(address, scan::kMaxUserAddress);
        close_editor();
        scroller_.recenter();
        document_.set_view(first_byte_, bytes_per_row_, visible_rows_);
        viewport()->update();
    }

    bool MemoryView::go_to(const QString& text)
    {
        const auto parsed = ui::parse_address_text(text.toStdString(), document_.module_spans());
        if (!parsed.has_value())
        {
            return false;
        }
        navigate_to(*parsed);
        return true;
    }

    void MemoryView::navigate_to(std::uint64_t address)
    {
        const std::uint64_t target = std::min(address, scan::kMaxUserAddress);
        if (!history_.record(first_byte_, target))
        {
            return;
        }
        set_first_byte(address);
        emit navigated();
    }

    bool MemoryView::back()
    {
        const std::optional<std::uint64_t> previous = history_.back();
        if (!previous.has_value())
        {
            return false;
        }
        set_first_byte(*previous);
        emit navigated();
        return true;
    }

    bool MemoryView::can_go_back() const noexcept
    {
        return history_.can_go_back();
    }

    void MemoryView::clear_history() noexcept
    {
        history_.clear();
    }

    void MemoryView::set_text_column_visible(bool visible)
    {
        if (text_column_visible_ == visible)
        {
            return;
        }
        text_column_visible_ = visible;
        close_editor();
        recompute_layout();
        viewport()->update();
    }

    void MemoryView::resizeEvent(QResizeEvent* event)
    {
        QAbstractScrollArea::resizeEvent(event);
        recompute_layout();
        viewport()->update();
    }

    void MemoryView::showEvent(QShowEvent* event)
    {
        QAbstractScrollArea::showEvent(event);
        document_.set_visible(true);
        recompute_layout();
    }

    void MemoryView::hideEvent(QHideEvent* event)
    {
        QAbstractScrollArea::hideEvent(event);
        document_.set_visible(false);
        close_editor();
    }

    std::optional<MemoryView::Hit> MemoryView::hit_test(const QPoint& position) const
    {
        if (row_height_ <= 0 || bytes_per_row_ == 0 || cell_width_ <= 0 || position.y() < kMargin + header_height_)
        {
            return std::nullopt;
        }
        const int row = (position.y() - kMargin - header_height_) / row_height_;
        if (row < 0 || static_cast<std::size_t>(row) >= visible_rows_)
        {
            return std::nullopt;
        }

        const int         cells_left = kMargin + address_width_ + kGapAfterAddress;
        const int         relative   = position.x() - cells_left;
        const std::size_t size       = std::max<std::size_t>(1, document_.format().size());
        const std::size_t column =
            relative < 0 ? 0 : static_cast<std::size_t>(relative) / static_cast<std::size_t>(cell_width_);
        if (relative < 0 || column >= bytes_per_row_ / size)
        {
            return std::nullopt;
        }

        Hit hit;
        hit.column  = static_cast<int>(column);
        hit.address = first_byte_ + static_cast<std::uint64_t>(row) * bytes_per_row_ + column * size;
        hit.rect    = QRect(cells_left + static_cast<int>(column) * cell_width_,
                            kMargin + header_height_ + row * row_height_,
                            cell_width_,
                            row_height_);
        return hit;
    }

    void MemoryView::paintEvent(QPaintEvent* event)
    {
        Q_UNUSED(event);
        QPainter painter(viewport());
        painter.setFont(mono_font());

        const Theme& theme = active_theme();
        painter.fillRect(viewport()->rect(), theme.surface);

        const std::size_t size       = std::max<std::size_t>(1, document_.format().size());
        const std::size_t cells      = bytes_per_row_ / size;
        const int         cells_left = kMargin + address_width_ + kGapAfterAddress;

        // The column header: the hex offset of each value within the row.
        painter.setPen(theme.text_muted);
        for (std::size_t column = 0; column < cells; ++column)
        {
            painter.drawText(
                QRect(cells_left + static_cast<int>(column) * cell_width_, kMargin, cell_width_, header_height_),
                Qt::AlignLeft | Qt::AlignVCenter,
                column_offset_text(column));
        }

        int y = kMargin + header_height_;
        for (std::size_t row = 0; row < visible_rows_; ++row)
        {
            const std::uint64_t row_address = first_byte_ + static_cast<std::uint64_t>(row) * bytes_per_row_;

            painter.setPen(theme.text_muted);
            painter.drawText(QRect(kMargin, y, address_width_, row_height_),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             document_.address_text(row_address));

            int x = cells_left;
            for (std::size_t column = 0; column < cells; ++column)
            {
                const std::uint64_t            address = row_address + column * size;
                const MemoryViewDocument::Cell cell    = document_.cell(address);

                QColor colour = theme.text;
                if (!cell.readable)
                {
                    colour = theme.text_muted;
                }
                else if (cell.changed)
                {
                    colour = theme.warning;
                }
                painter.setPen(colour);
                painter.drawText(QRect(x, y, cell_width_, row_height_), Qt::AlignLeft | Qt::AlignVCenter, cell.text);
                x += cell_width_;
            }

            if (text_column_visible_)
            {
                painter.setPen(theme.border);
                painter.drawLine(x + kTextGap / 2, y, x + kTextGap / 2, y + row_height_);
                painter.setPen(theme.text);
                painter.drawText(QRect(x + kTextGap, y, text_width_, row_height_),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 document_.text_row(row_address));
            }

            y += row_height_;
        }
    }

    void MemoryView::wheelEvent(QWheelEvent* event)
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

    void MemoryView::keyPressEvent(QKeyEvent* event)
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
            set_first_byte(0);
            break;
        default:
            QAbstractScrollArea::keyPressEvent(event);
            return;
        }
        event->accept();
    }

    void MemoryView::mouseDoubleClickEvent(QMouseEvent* event)
    {
        if (event->button() == Qt::LeftButton)
        {
            if (const std::optional<Hit> hit = hit_test(event->position().toPoint()); hit.has_value())
            {
                if (editor_ == nullptr)
                {
                    editor_ = new QLineEdit(viewport());
                    editor_->setObjectName(QStringLiteral("memory_view_editor"));
                    editor_->setFont(mono_font());
                    editor_->installEventFilter(this);
                    connect(editor_, &QLineEdit::returnPressed, this, &MemoryView::commit_edit);
                    connect(editor_,
                            &QLineEdit::editingFinished,
                            this,
                            [this]
                            {
                                if (editing_)
                                {
                                    close_editor();
                                }
                            });
                }
                editing_                            = true;
                edit_address_                       = hit->address;
                const MemoryViewDocument::Cell cell = document_.cell(hit->address);
                // An unreadable cell opens empty rather than pre-filled with `?`.
                editor_->setText(cell.readable ? cell.text : QString());
                editor_->setGeometry(hit->rect);
                editor_->show();
                editor_->setFocus(Qt::MouseFocusReason);
                editor_->selectAll();
                event->accept();
                return;
            }
        }
        QAbstractScrollArea::mouseDoubleClickEvent(event);
    }

    void MemoryView::contextMenuEvent(QContextMenuEvent* event)
    {
        QMenu menu(this);
        populate_options_menu(menu);
        menu.exec(event->globalPos());
    }

    void MemoryView::populate_options_menu(QMenu& menu)
    {
        menu.addAction(goto_action_);

        // Always present so the capability is discoverable, disabled while the
        // history is empty.
        QAction* back_action = menu.addAction(tr("Back"));
        back_action->setEnabled(can_go_back());
        connect(back_action,
                &QAction::triggered,
                this,
                [this]
                {
                    back();
                });

        menu.addSeparator();

        // One exclusive group spans every value format, so exactly one is checked.
        auto* format_group = new QActionGroup(&menu);
        format_group->setExclusive(true);

        QMenu*     format_menu = menu.addMenu(tr("Value format"));
        const auto add_format  = [this, format_group](QMenu* parent, const QString& label, ValueFormat format)
        {
            QAction* action = parent->addAction(label);
            action->setCheckable(true);
            action->setChecked(document_.format() == format);
            format_group->addAction(action);
            connect(action,
                    &QAction::triggered,
                    this,
                    [this, format]
                    {
                        document_.set_format(format);
                        relayout();
                    });
        };

        const scan::ValueType integers[] = {
            scan::ValueType::byte,
            scan::ValueType::int16,
            scan::ValueType::int32,
            scan::ValueType::int64,
        };
        for (const scan::ValueType type : integers)
        {
            QMenu* type_menu =
                format_menu->addMenu(QString::fromLatin1(scan::kValueTypeNames[static_cast<std::size_t>(type)]));
            add_format(type_menu, tr("Hex"), ValueFormat {.type = type, .hex = true});
            add_format(type_menu, tr("Decimal"), ValueFormat {.type = type, .hex = false});
        }
        add_format(format_menu,
                   QString::fromLatin1(scan::kValueTypeNames[static_cast<std::size_t>(scan::ValueType::float32)]),
                   ValueFormat {.type = scan::ValueType::float32, .hex = false});
        add_format(format_menu,
                   QString::fromLatin1(scan::kValueTypeNames[static_cast<std::size_t>(scan::ValueType::float64)]),
                   ValueFormat {.type = scan::ValueType::float64, .hex = false});

        // Text encoding: the side column's decode unit.
        auto* encoding_group = new QActionGroup(&menu);
        encoding_group->setExclusive(true);
        QMenu*     encoding_menu = menu.addMenu(tr("Text encoding"));
        const auto add_encoding  = [this, encoding_group, encoding_menu](const QString& label, TextEncoding encoding)
        {
            QAction* action = encoding_menu->addAction(label);
            action->setCheckable(true);
            action->setChecked(document_.encoding() == encoding);
            encoding_group->addAction(action);
            connect(action,
                    &QAction::triggered,
                    this,
                    [this, encoding]
                    {
                        document_.set_encoding(encoding);
                        relayout();
                    });
        };
        add_encoding(tr("ASCII"), TextEncoding::ascii);
        add_encoding(tr("UTF-8"), TextEncoding::utf8);
        add_encoding(tr("UTF-16"), TextEncoding::utf16);

        QAction* show_text = menu.addAction(tr("Show text column"));
        show_text->setCheckable(true);
        show_text->setChecked(text_column_visible_);
        connect(show_text,
                &QAction::triggered,
                this,
                [this](bool checked)
                {
                    set_text_column_visible(checked);
                });
    }

    bool MemoryView::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == editor_ && event->type() == QEvent::KeyPress)
        {
            if (auto* key = static_cast<QKeyEvent*>(event); key->key() == Qt::Key_Escape)
            {
                close_editor();
                return true;
            }
        }
        return QAbstractScrollArea::eventFilter(watched, event);
    }

    void MemoryView::commit_edit()
    {
        if (!editing_ || editor_ == nullptr)
        {
            return;
        }

        if (document_.write_value(edit_address_, editor_->text()))
        {
            close_editor();
            emit bytesEdited();
            return;
        }
        // A rejected value keeps the editor open; the reason is in the status line.
        editor_->selectAll();
    }

    void MemoryView::close_editor()
    {
        if (!editing_)
        {
            return;
        }
        editing_      = false;
        edit_address_ = 0;
        if (editor_ != nullptr)
        {
            editor_->hide();
        }
    }

} // namespace slopkit::ui::components
