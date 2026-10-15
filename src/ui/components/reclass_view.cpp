#include "ui/components/reclass_view.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

#include <QContextMenuEvent>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPolygon>
#include <QResizeEvent>
#include <QScrollBar>
#include <QToolTip>
#include <QVariant>
#include <QWheelEvent>

#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/fonts.hpp"
#include "ui/models/reclass_proxy_model.hpp"
#include "ui/models/reclass_value_model.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui::components
{
    namespace
    {
        constexpr int kMargin                = 4;
        constexpr int kTextPadding           = 4;
        // The room the widest text a column was measured for leaves before the
        // next column's text: a grid whose columns are sized once should not
        // spend more than a few pixels between them.
        constexpr int kColumnGap             = 12;
        // The value and the notes and readings that annotate it sit closer
        // together than the tabular fields before them, so a row reads as one
        // value with its annotations.
        constexpr int kAnnotationGap         = 6;
        constexpr int kIndentStep            = 12;
        constexpr int kTriangleWidth         = 8;
        constexpr int kWheelRows             = 3;
        // The note field of a node with no note is only as wide as its marker,
        // so its editor opens at least this wide to be typed into.
        constexpr int kCommentEditorMinWidth = 160;
        // The cursor and hover tints are drawn over the stripes rather than
        // beside them, so both kinds of row keep the same tint.
        constexpr int kTintAlpha             = 36;

        // The gutter a column leaves before the next one: the value and the
        // notes and readings that annotate it take the tighter one.
        constexpr int gap_after(int column)
        {
            using models::ReclassStructureModel;
            return column == ReclassStructureModel::value || column == ReclassStructureModel::comment
                        || column == ReclassStructureModel::hints
                     ? kAnnotationGap
                     : kColumnGap;
        }

        // A flowed field's run is exactly as wide as the text it was measured
        // for, and Qt's own elision counts a text as too wide when it is exactly
        // as wide as the room it is given, so it would drop the run's last
        // characters. A text that fits is therefore drawn whole - the same
        // "does it fit" the hover tooltip answers with in hover_full_text().
        QString fitted_run(const QFontMetrics& metrics, const QString& text, int available)
        {
            return metrics.horizontalAdvance(text) <= available ? text
                                                                : metrics.elidedText(text, Qt::ElideRight, available);
        }

        // The rect the `[N]` a Type cell brackets draws in, when the cell draws
        // its whole text: nothing when the run is absent or the cell elides the
        // text it sits in, which leaves the run no fixed place. An array draws
        // its index control behind the run, so the run is the one before the
        // arrows and not the last bracket the text spells: a class the cell
        // names may hold a bracket of its own.
        std::optional<QRect> run_rect(const QRect&        cell,
                                      const QFontMetrics& metrics,
                                      const QString&      text,
                                      const QString&      run,
                                      int                 before_arrows = -1)
        {
            if (run.isEmpty() || cell.isEmpty())
            {
                return std::nullopt;
            }
            const int offset = before_arrows < 0 ? text.indexOf(run) : text.lastIndexOf(run, before_arrows);
            if (offset < 0)
            {
                return std::nullopt;
            }
            const int x         = cell.x() + metrics.horizontalAdvance(text.left(offset));
            const int run_width = metrics.horizontalAdvance(run);
            if (run_width <= 0 || x + run_width > cell.x() + cell.width())
            {
                return std::nullopt;
            }
            return QRect(x, cell.y(), run_width, cell.height());
        }

        // The rects the three parts of an array's index control draw in, inside
        // a Type cell that draws its whole text: nothing when the cell elides
        // the text the control sits in, which leaves it no fixed place.
        std::optional<ReClassView::ElementRects> control_rects(const QRect&               cell,
                                                               const QFontMetrics&        metrics,
                                                               const QString&             text,
                                                               const models::ElementRuns& runs)
        {
            if (text.isEmpty() || runs.previous < 0 || runs.index < 0 || runs.next < 0)
            {
                return std::nullopt;
            }
            const auto run = [&cell, &metrics, &text](int offset, int length)
            {
                return QRect(cell.x() + metrics.horizontalAdvance(text.left(offset)),
                             cell.y(),
                             metrics.horizontalAdvance(text.mid(offset, length)),
                             cell.height());
            };

            ReClassView::ElementRects rects;
            rects.previous = run(runs.previous, 1);
            rects.index    = run(runs.index, runs.index_length);
            rects.next     = run(runs.next, 1);
            if (rects.previous.x() < cell.x() || rects.next.x() + rects.next.width() > cell.x() + cell.width())
            {
                return std::nullopt;
            }
            return rects;
        }

        // The value's own font and colour when the model states them, and the
        // grid's when the choice is left to us.
        QFont cell_font(const QModelIndex& index, const QAbstractItemModel& model)
        {
            const QVariant value = model.data(index, Qt::FontRole);
            return value.canConvert<QFont>() ? value.value<QFont>() : ui_font();
        }

        std::optional<QColor> cell_colour(const QModelIndex& index, const QAbstractItemModel& model)
        {
            const QVariant value = model.data(index, Qt::ForegroundRole);
            return value.canConvert<QColor>() ? std::optional<QColor>(value.value<QColor>()) : std::nullopt;
        }

        // The colour a value run paints: the warning colour for a byte of its own
        // that moved, and the grid's text otherwise. A run with no byte of its own
        // - a kind that draws no byte pair - warns whole when any of the node's
        // bytes moved, except the cell's own `= ` marker, which names the value
        // rather than reading it and so keeps the grid's text.
        QColor part_colour(const reclass::ValuePart&     part,
                           std::span<const std::uint8_t> moved,
                           bool                          any_moved,
                           const QColor&                 primary)
        {
            if (part.label)
            {
                return primary;
            }
            if (part.byte.has_value())
            {
                const std::size_t index = *part.byte;
                return index < moved.size() && moved[index] != 0 ? active_theme().warning : primary;
            }
            return any_moved ? active_theme().warning : primary;
        }

        // The widest readings a hex cell can draw: the longest float three
        // decimals can spell, the signed value's longest text and the raw bits
        // at full width. A reading only arrives on a live pass, when the columns
        // are not measured again, so the hints field is sized for this and never
        // for what the node currently reads.
        QString widest_readings(reclass::NodeKind kind)
        {
            const bool wide = reclass::kind_size(kind) == 8;
            return QStringLiteral("(-999998.999  %1  %2)")
                .arg(wide ? QStringLiteral("-9223372036854775808") : QStringLiteral("-2147483648"),
                     wide ? QStringLiteral("0xFFFFFFFFFFFFFFFF") : QStringLiteral("0xFFFFFFFF"));
        }

        // The widest pointer field a row can draw: the value model's longest
        // region tag beside the address itself at full width.
        QString widest_pointer()
        {
            return QStringLiteral("-> <STACK>FFFFFFFFFFFFFFFF");
        }

        // The widest value text a row can draw, taken from the row's kind rather
        // than from what the class reads right now: a live pass never measures
        // the columns again, so a value that arrives later must already fit. A
        // hex cell's dump is fixed by its width, and every other kind is read
        // through the same formatter the cell uses, on a sample of its own size.
        QString widest_value_text(const reclass::Node& node)
        {
            const reclass::NodeKind kind = node.kind();
            const std::size_t       size = reclass::kind_size(kind);
            if (size == 0)
            {
                return {};
            }

            const std::vector<std::byte> sample(size, std::byte {0x80});
            std::string                  text;
            for (const reclass::ValuePart& part : reclass::format_node_value_parts(kind, sample, node.hex))
            {
                if (!part.reading)
                {
                    text += part.text;
                }
            }
            if (text.empty() || reclass::is_pointer_kind(kind))
            {
                return {};
            }
            return reclass::is_hex_kind(kind) ? to_qstring(text) : QStringLiteral("= ") + to_qstring(text);
        }
    }

    ReClassView::ReClassView(models::ReclassProxyModel&     model,
                             models::ReclassStructureModel& structure,
                             models::ReclassValueModel&     values,
                             QWidget*                       parent)
        : QAbstractScrollArea(parent), model_(&model), structure_(&structure), values_(&values),
          scroller_(*this,
                    [this](long long delta)
                    {
                        scroll_rows(delta);
                    })
    {
        setFocusPolicy(Qt::StrongFocus);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        viewport()->setMouseTracking(true);

        update_layout();
        update_rows();

        // The bar's offset is part of the columns' on-screen x, so a scroll
        // re-places the row; the bar appearing or hiding resizes the viewport,
        // which changes how many rows fit. The editor and the tooltip were
        // pinned to the cells as they stood, so both close.
        const auto follow_scroll = [this]
        {
            QToolTip::hideText();
            close_editor();
            update_view();
        };
        connect(horizontalScrollBar(),
                &QScrollBar::valueChanged,
                this,
                [follow_scroll](int)
                {
                    follow_scroll();
                });
        connect(horizontalScrollBar(),
                &QScrollBar::rangeChanged,
                this,
                [follow_scroll](int, int)
                {
                    follow_scroll();
                });

        // The model resets itself when a pass rebuilds the tree, so the rows are
        // flattened again; a live pass only changes data, which repaints.
        connect(model_, &QAbstractItemModel::modelReset, this, &ReClassView::update_rows);
        connect(model_, &QAbstractItemModel::rowsInserted, this, &ReClassView::update_rows);
        connect(model_, &QAbstractItemModel::rowsRemoved, this, &ReClassView::update_rows);
        connect(model_,
                &QAbstractItemModel::dataChanged,
                this,
                [this](const QModelIndex& top_left, const QModelIndex& bottom_right, const QList<int>&)
                {
                    using models::ReclassStructureModel;
                    // A rename, a note and an address that resolves or retargets
                    // are the changes that can move a column boundary; a value
                    // pass never re-measures.
                    const bool retyped = top_left.column() == bottom_right.column()
                                      && (top_left.column() == ReclassStructureModel::name
                                          || top_left.column() == ReclassStructureModel::comment);
                    if (retyped)
                    {
                        update_content_widths();
                        update_columns();
                    }
                    else if (top_left.column() <= ReclassStructureModel::address
                             && bottom_right.column() >= ReclassStructureModel::address)
                    {
                        // The address column follows the addresses the class
                        // draws, so re-measure it and re-lay the columns only
                        // when the width actually moved.
                        const int before = column_width_[ReclassStructureModel::address];
                        update_content_widths();
                        if (column_width_[ReclassStructureModel::address] != before)
                        {
                            update_columns();
                        }
                    }
                    viewport()->update();
                });
    }

    models::ReclassProxyModel* ReClassView::model() const noexcept
    {
        return model_;
    }

    models::ReclassStructureModel* ReClassView::structure() const noexcept
    {
        return structure_;
    }

    std::size_t ReClassView::row_count() const noexcept
    {
        return rows_.size();
    }

    std::size_t ReClassView::visible_row_count() const noexcept
    {
        return rows_.size();
    }

    QLineEdit* ReClassView::editor() const noexcept
    {
        return editor_;
    }

    void ReClassView::relayout()
    {
        close_editor();
        update_layout();
        update_rows();
    }

    void ReClassView::update_layout()
    {
        const QFontMetrics mono(mono_font());
        const QFontMetrics prose(ui_font());
        row_height_ = std::max(1, std::max(mono.height(), prose.height()) + 4);

        // Every field's width or cap starts at nothing here and is measured from
        // the flattened rows the class draws (see update_content_widths).
        using models::ReclassStructureModel;
        for (int column = 0; column < ReclassStructureModel::column_count; ++column)
        {
            column_width_[column] = 0;
        }
        content_x_     = 0;
        content_width_ = 0;
    }

    void ReClassView::update_content_widths()
    {
        using models::ReclassStructureModel;

        const QFontMetrics mono(mono_font());
        const QFontMetrics prose(ui_font());
        const auto         display = [this](const VisualRow& entry, int column)
        {
            return model_->data(index_for(*entry.row, column), Qt::DisplayRole).toString();
        };

        // The three aligned fields are sized to the text this class really
        // draws, so a list of small nodes does not sit in a grid sized for the
        // widest kind in the language. Each flowing field keeps a cap - the
        // widest text the class draws, or the widest its kinds can draw - which
        // the run is clipped at and which the scrollbar's end is measured from.
        // This follows the flattened rows, which a live pass never rebuilds.
        int address_width = 0;
        int offset_width  = 0;
        int ascii_width   = 0;
        int type_width    = 0;
        int name_width    = 0;
        int value_width   = 0;
        int comment_width = 0;
        int hints_width   = 0;
        int pointer_width = 0;
        int content       = 0;
        for (const VisualRow& entry : rows_)
        {
            const reclass::Node* node = entry.row->node;
            if (node == nullptr)
            {
                continue;
            }

            // The address column is only as wide as the longest address this
            // class draws; a hidden row draws nothing there.
            if (!entry.row->hidden)
            {
                address_width = std::max(
                    address_width, mono.horizontalAdvance(display(entry, ReclassStructureModel::address)) + kColumnGap);
            }
            offset_width = std::max(offset_width,
                                    mono.horizontalAdvance(display(entry, ReclassStructureModel::offset)) + kColumnGap);
            // An Ascii cell draws at most kAsciiMaxChars characters, so the
            // column is measured at the characters it can draw; a wider node
            // then no longer stretches the row.
            const std::size_t ascii_chars =
                entry.row->hidden ? 0 : std::min(node->value_size(), models::kAsciiMaxChars);
            ascii_width = std::max(
                ascii_width,
                mono.horizontalAdvance(QString(static_cast<qsizetype>(ascii_chars), QLatin1Char('W'))) + kColumnGap);

            // A hidden row collapses to the `...` placeholder and draws nothing
            // else, so its placeholder width folds into the name cap.
            const QString type_text = entry.row->hidden ? QString() : display(entry, ReclassStructureModel::type);
            const QString name_text =
                entry.row->hidden ? QStringLiteral("...") : display(entry, ReclassStructureModel::name);
            const QString comment_text = entry.row->hidden ? QString() : display(entry, ReclassStructureModel::comment);
            type_width                 = std::max(type_width, prose.horizontalAdvance(type_text));
            name_width                 = std::max(name_width, prose.horizontalAdvance(name_text));
            comment_width              = std::max(comment_width, prose.horizontalAdvance(comment_text));

            // The value field is sized for the widest value the row's kind can
            // draw, so the values that only arrive with the first live pass are
            // not clipped by a cap measured while nothing had been read.
            const int value_run = std::max(mono.horizontalAdvance(display(entry, ReclassStructureModel::value)),
                                           mono.horizontalAdvance(widest_value_text(*node)));
            value_width         = std::max(value_width, value_run);

            const reclass::NodeKind kind      = node->kind();
            int                     hints_run = 0;
            if (!entry.row->hidden && reclass::is_hex_kind(kind) && reclass::kind_size(kind) >= 4)
            {
                hints_run   = mono.horizontalAdvance(widest_readings(kind));
                hints_width = std::max(hints_width, hints_run);
            }
            int pointer_run = 0;
            if (!entry.row->hidden && (reclass::is_hex_kind(kind) || reclass::is_pointer_kind(kind)))
            {
                pointer_run   = mono.horizontalAdvance(widest_pointer());
                pointer_width = std::max(pointer_width, pointer_run);
            }

            // The widest row's end, measured from the head of the content slot:
            // every field that row draws at the width it can reach. A live pass
            // is bounded by the same widths, so the range never has to grow.
            int        end = kTextPadding + static_cast<int>(entry.depth) * kIndentStep
                           + (expandable(*entry.row) ? kTriangleWidth : 0);
            const auto add = [&end](int width, int column)
            {
                if (width > 0)
                {
                    end += width + gap_after(column);
                }
            };
            if (!entry.row->hidden)
            {
                add(prose.horizontalAdvance(type_text), ReclassStructureModel::type);
                add(prose.horizontalAdvance(name_text), ReclassStructureModel::name);
                add(value_run, ReclassStructureModel::value);
                add(prose.horizontalAdvance(comment_text), ReclassStructureModel::comment);
                add(hints_run, ReclassStructureModel::hints);
                add(pointer_run, ReclassStructureModel::pointer);
            }
            else
            {
                add(prose.horizontalAdvance(name_text), ReclassStructureModel::name);
            }
            content = std::max(content, end + kMargin);
        }

        column_width_[ReclassStructureModel::address] = address_width;
        column_width_[ReclassStructureModel::offset]  = offset_width;
        column_width_[ReclassStructureModel::ascii]   = ascii_width;
        column_width_[ReclassStructureModel::type]    = type_width;
        column_width_[ReclassStructureModel::name]    = name_width;
        column_width_[ReclassStructureModel::value]   = value_width;
        column_width_[ReclassStructureModel::comment] = comment_width;
        column_width_[ReclassStructureModel::hints]   = hints_width;
        column_width_[ReclassStructureModel::pointer] = pointer_width;
        content_width_                                = content;
    }

    void ReClassView::update_columns()
    {
        using models::ReclassStructureModel;

        // The three aligned fields sit on their measured columns and every row's
        // own content starts right behind them; the bar spans exactly the width
        // the widest row overflows the pane, and its offset is baked into the
        // fields' x, so every paint, hit-test and editor path keeps reading view
        // coordinates.
        const int content = kMargin + column_width_[ReclassStructureModel::offset]
                          + column_width_[ReclassStructureModel::address] + column_width_[ReclassStructureModel::ascii]
                          + content_width_;
        QScrollBar* bar   = horizontalScrollBar();
        bar->setRange(0, std::max(0, content - viewport()->width()));
        bar->setPageStep(viewport()->width());
        bar->setSingleStep(std::max(1, row_height_));

        int x                                    = kMargin - bar->value();
        column_x_[ReclassStructureModel::offset] = x;
        x += column_width_[ReclassStructureModel::offset];
        column_x_[ReclassStructureModel::address] = x;
        x += column_width_[ReclassStructureModel::address];
        column_x_[ReclassStructureModel::ascii] = x;
        x += column_width_[ReclassStructureModel::ascii];
        content_x_ = x;
    }

    void ReClassView::update_rows()
    {
        rows_.clear();
        flatten({}, 0);
        resolve_identity();
        update_content_widths();
        update_view();
    }

    void ReClassView::flatten(const QModelIndex& parent, std::size_t depth)
    {
        const int count = model_->rowCount(parent);
        for (int position = 0; position < count; ++position)
        {
            // Column 0 only: a Qt parent index exposes children in column 0
            // alone, whatever column the cell being drawn sits in.
            const QModelIndex         index = model_->index(position, 0, parent);
            const models::ReclassRow* row   = row_of(index);
            if (row == nullptr)
            {
                continue;
            }
            rows_.push_back(VisualRow {.row = row, .depth = depth});
            flatten(index, depth + 1);
        }
    }

    void ReClassView::resolve_identity()
    {
        // The cursor and the selection live as node ids, so a rebuild of the
        // row objects - the only thing a class or an expansion change does -
        // leaves them where they were.
        std::erase_if(selected_,
                      [this](std::size_t node_id)
                      {
                          return visual_index_of(node_id) < 0;
                      });

        if (has_current_ && visual_index_of(current_) < 0)
        {
            has_current_ = false;
            current_     = 0;
        }
    }

    void ReClassView::update_view()
    {
        update_columns();
        visible_rows_  = std::max(1, std::max(0, viewport()->height() - kMargin) / std::max(1, row_height_));
        const int rows = static_cast<int>(rows_.size());
        first_row_     = std::clamp(first_row_, 0, std::max(0, rows - visible_rows_));
        scroller_.note_visible_rows(static_cast<std::size_t>(visible_rows_));
        viewport()->update();
    }

    const models::ReclassRow* ReClassView::row_of(const QModelIndex& index) const
    {
        if (!index.isValid())
        {
            return nullptr;
        }
        return structure_->row_for(model_->mapToSource(index));
    }

    const models::ReclassRow* ReClassView::row_for_node(std::size_t node_id) const
    {
        const int index = visual_index_of(node_id);
        return index < 0 ? nullptr : rows_[static_cast<std::size_t>(index)].row;
    }

    int ReClassView::visual_index_of(std::size_t node_id) const
    {
        for (std::size_t index = 0; index < rows_.size(); ++index)
        {
            if (rows_[index].row->node_id == node_id)
            {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    QModelIndex ReClassView::index_for(const models::ReclassRow& row, int column) const
    {
        // The proxy is an identity over the structure tree, so an index built
        // through the ancestors is the same row in both models - including for
        // the rows an expanded reference inlined, whose position in their parent
        // identifies them.
        std::vector<const models::ReclassRow*> ancestors;
        for (const models::ReclassRow* parent = row.parent; parent != nullptr; parent = parent->parent)
        {
            ancestors.push_back(parent);
        }

        QModelIndex parent;
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
        {
            parent = model_->index(static_cast<int>((*it)->position), 0, parent);
        }
        return model_->index(static_cast<int>(row.position), column, parent);
    }

    QModelIndex ReClassView::currentIndex() const
    {
        const models::ReclassRow* row = has_current_ ? row_for_node(current_) : nullptr;
        return row == nullptr ? QModelIndex() : index_for(*row, models::ReclassStructureModel::address);
    }

    void ReClassView::setCurrentIndex(const QModelIndex& index)
    {
        const models::ReclassRow* row = row_of(index);
        if (row == nullptr)
        {
            has_current_ = false;
            current_     = 0;
        }
        else
        {
            has_current_ = true;
            current_     = row->node_id;
        }
        selected_.clear();
        if (has_current_)
        {
            selected_.push_back(current_);
        }
        emit selectionChanged();
        viewport()->update();
    }

    void ReClassView::scrollTo(const QModelIndex& index)
    {
        const models::ReclassRow* row = row_of(index);
        if (row != nullptr)
        {
            ensure_visible(visual_index_of(row->node_id));
        }
    }

    bool ReClassView::isExpanded(const QModelIndex& index) const
    {
        const models::ReclassRow* row = row_of(index);
        return row != nullptr && structure_->is_expanded(row->node_id);
    }

    void ReClassView::expand(const QModelIndex& index)
    {
        const models::ReclassRow* row = row_of(index);
        if (row != nullptr)
        {
            structure_->set_expanded(row->node_id, true);
        }
    }

    void ReClassView::collapse(const QModelIndex& index)
    {
        const models::ReclassRow* row = row_of(index);
        if (row != nullptr)
        {
            structure_->set_expanded(row->node_id, false);
        }
    }

    std::vector<QModelIndex> ReClassView::selected_rows() const
    {
        std::vector<QModelIndex> indexes;
        for (const std::size_t node_id : selected_)
        {
            if (const models::ReclassRow* row = row_for_node(node_id); row != nullptr)
            {
                indexes.push_back(index_for(*row, models::ReclassStructureModel::address));
            }
        }
        return indexes;
    }

    std::optional<QRect> ReClassView::cell_rect(const QModelIndex& index) const
    {
        const models::ReclassRow* row = row_of(index);
        if (row == nullptr)
        {
            return std::nullopt;
        }
        const int visual = visual_index_of(row->node_id);
        if (visual < 0)
        {
            return std::nullopt;
        }
        return cell_rect_at(visual, index.column());
    }

    std::optional<QRect> ReClassView::byte_rect(const QModelIndex& index, std::size_t byte_index) const
    {
        const models::ReclassRow* row = row_of(index);
        if (row == nullptr)
        {
            return std::nullopt;
        }
        const int visual = visual_index_of(row->node_id);
        if (visual < 0)
        {
            return std::nullopt;
        }
        return byte_rect_in(rows_[static_cast<std::size_t>(visual)], visual, index.column(), byte_index);
    }

    std::optional<QRect> ReClassView::extent_rect(const QModelIndex& index) const
    {
        using models::ReclassStructureModel;
        const models::ReclassRow* row = row_of(index);
        if (row == nullptr || index.column() != ReclassStructureModel::type)
        {
            return std::nullopt;
        }
        const int visual = visual_index_of(row->node_id);
        if (visual < 0)
        {
            return std::nullopt;
        }
        const QModelIndex  type = index_for(*row, ReclassStructureModel::type);
        const QRect        cell = cell_rect_at(visual, ReclassStructureModel::type);
        const QFontMetrics metrics(cell_font(type, *model_));
        if (cell.isEmpty())
        {
            return std::nullopt;
        }
        return run_rect(cell,
                        metrics,
                        model_->data(type, Qt::DisplayRole).toString(),
                        model_->data(type, models::kExtentTextRole).toString(),
                        structure_->element_runs(row->node_id).previous);
    }

    std::optional<ReClassView::ElementRects> ReClassView::element_rects(const QModelIndex& index) const
    {
        using models::ReclassStructureModel;
        const models::ReclassRow* row = row_of(index);
        if (row == nullptr || index.column() != ReclassStructureModel::type)
        {
            return std::nullopt;
        }
        const int visual = visual_index_of(row->node_id);
        if (visual < 0)
        {
            return std::nullopt;
        }
        return element_rects_in(rows_[static_cast<std::size_t>(visual)], visual);
    }

    std::optional<QRect>
    ReClassView::byte_rect_in(const VisualRow& entry, int row_index, int column, std::size_t byte_index) const
    {
        const RowLayout layout = layout_row(entry, row_index);
        const QRect     cell   = layout.rects[column];
        if (cell.isEmpty())
        {
            return std::nullopt;
        }
        const QFontMetrics metrics(cell_font(index_for(*entry.row, column), *model_));

        if (column == models::ReclassStructureModel::ascii)
        {
            const QString text = values_->ascii_text(*entry.row);
            const int     x    = aligned_x(entry, column);
            // An elided Ascii cell draws a text that is no longer one character
            // per byte, so no character sits at a fixed place in it.
            if (metrics.horizontalAdvance(text) > cell.x() + cell.width() - x
                || byte_index >= static_cast<std::size_t>(text.size()))
            {
                return std::nullopt;
            }
            const int advance = metrics.horizontalAdvance(QLatin1Char('0'));
            return QRect(x + static_cast<int>(byte_index) * advance, cell.y(), advance, cell.height());
        }

        if (column != models::ReclassStructureModel::value)
        {
            return std::nullopt;
        }

        // The value cell draws a hex dump one run per byte, each run carrying
        // the space before its pair, so the pair itself starts behind it.
        const std::vector<reclass::ValuePart> parts = values_->value_parts(*entry.row);
        int                                   total = 0;
        for (const reclass::ValuePart& part : parts)
        {
            total += metrics.horizontalAdvance(to_qstring(part.text));
        }
        if (parts.empty() || total > cell.width())
        {
            return std::nullopt;
        }

        int x = cell.x();
        for (const reclass::ValuePart& part : parts)
        {
            const int width = metrics.horizontalAdvance(to_qstring(part.text));
            if (part.byte == byte_index)
            {
                const int pair = metrics.horizontalAdvance(QStringLiteral("00"));
                return QRect(x + width - pair, cell.y(), pair, cell.height());
            }
            x += width;
        }
        return std::nullopt;
    }

    std::optional<QRect>
    ReClassView::extent_rect_in(const VisualRow& entry, int row_index, const QPoint& position) const
    {
        using models::ReclassStructureModel;
        const QModelIndex  type = index_for(*entry.row, ReclassStructureModel::type);
        const QRect        cell = cell_rect_at(row_index, ReclassStructureModel::type);
        const QFontMetrics metrics(cell_font(type, *model_));
        if (cell.isEmpty())
        {
            return std::nullopt;
        }
        const std::optional<QRect> run = run_rect(cell,
                                                  metrics,
                                                  model_->data(type, Qt::DisplayRole).toString(),
                                                  model_->data(type, models::kExtentTextRole).toString(),
                                                  structure_->element_runs(entry.row->node_id).previous);
        // A point right of the run's start is on the run: the cell is exactly as
        // wide as the text it draws, so nothing else stands beside it. An
        // array's index control stands right behind the run, and the arrows own
        // the points from their own glyph on, so the two zones stay apart.
        if (!run.has_value() || position.x() < run->x())
        {
            return std::nullopt;
        }
        const std::optional<ElementRects> control = element_rects_in(entry, row_index);
        if (control.has_value() && position.x() >= control->previous.x())
        {
            return std::nullopt;
        }
        return run;
    }

    std::optional<ReClassView::ElementRects> ReClassView::element_rects_in(const VisualRow& entry, int row_index) const
    {
        using models::ReclassStructureModel;
        const QModelIndex  type = index_for(*entry.row, ReclassStructureModel::type);
        const QRect        cell = cell_rect_at(row_index, ReclassStructureModel::type);
        const QFontMetrics metrics(cell_font(type, *model_));
        if (cell.isEmpty())
        {
            return std::nullopt;
        }
        return control_rects(cell,
                             metrics,
                             model_->data(type, Qt::DisplayRole).toString(),
                             structure_->element_runs(entry.row->node_id));
    }

    QString ReClassView::element_tooltip(const models::ReclassRow& row, int delta) const
    {
        if (row.node == nullptr)
        {
            return {};
        }
        const std::size_t count = structure_->element_count(*row.node);
        if (count == 0)
        {
            return {};
        }
        const auto      steps   = static_cast<long long>(count);
        const auto      current = static_cast<long long>(structure_->element_index(row.node_id));
        const long long shown   = ((current + delta) % steps + steps) % steps;
        return tr("Show element %1 of %2").arg(shown + 1).arg(count);
    }

    int ReClassView::byte_at(const VisualRow& entry, int row_index, int column, const QPoint& position) const
    {
        int last = -1;
        for (int index = 0;; ++index)
        {
            const std::optional<QRect> rect = byte_rect_in(entry, row_index, column, static_cast<std::size_t>(index));
            if (!rect.has_value())
            {
                break;
            }
            last = index;
            if (position.x() < rect->x() + rect->width())
            {
                return index;
            }
        }
        // A click past the cell's last byte belongs to that byte.
        return last;
    }

    QString ReClassView::cell_text(const QModelIndex& index) const
    {
        return model_->data(index, Qt::DisplayRole).toString();
    }

    QString ReClassView::hover_full_text(const QPoint& position) const
    {
        const std::optional<Hit> hit = hit_test(position);
        if (!hit.has_value())
        {
            return {};
        }
        const QModelIndex index = index_for(*hit->row, hit->column);
        const QString     full  = model_->data(index, widgets::kFullTextRole).toString();
        if (full.isEmpty())
        {
            return {};
        }

        const VisualRow&   entry     = rows_[static_cast<std::size_t>(hit->row_index)];
        const RowLayout    layout    = layout_row(entry, hit->row_index);
        const QRect        rect      = layout.rects[hit->column];
        // An aligned field's text sits on its content box; a flowed field's run
        // is its own rect, so the width the tooltip may use is the run's.
        const bool         aligned   = hit->column == models::ReclassStructureModel::offset
                                    || hit->column == models::ReclassStructureModel::address
                                    || hit->column == models::ReclassStructureModel::ascii;
        const int          origin    = aligned ? aligned_x(entry, hit->column) : rect.x();
        const int          available = std::max(0, rect.x() + rect.width() - origin);
        const QFontMetrics metrics(cell_font(index, *model_));
        // The same "does it fit" the paint decides with: a text exactly as wide
        // as its run is drawn whole, so it needs no tooltip.
        return metrics.horizontalAdvance(full) > available ? full : QString();
    }

    bool ReClassView::expandable(const models::ReclassRow& row) const
    {
        return structure_->expandable(row);
    }

    bool ReClassView::is_selected(const models::ReclassRow& row) const
    {
        return std::ranges::contains(selected_, row.node_id);
    }

    bool ReClassView::is_current(const models::ReclassRow& row) const
    {
        return has_current_ && row.node_id == current_;
    }

    int ReClassView::aligned_x(const VisualRow& entry, int column) const noexcept
    {
        // The offset, address and ascii fields print text of a varying length,
        // so they sit on the column's content box: a shorter row moves right,
        // which leaves the same gap to the next field on every row while the
        // widest row still starts where it always did.
        const int         base  = column_x_[column] + kTextPadding;
        const QModelIndex index = index_for(*entry.row, column);
        const int         advance =
            QFontMetrics(cell_font(index, *model_)).horizontalAdvance(model_->data(index, Qt::DisplayRole).toString());
        return base + std::max(0, column_width_[column] - gap_after(column) - advance);
    }

    ReClassView::RowLayout ReClassView::layout_row(const VisualRow& entry, int row_index) const
    {
        using models::ReclassStructureModel;

        const QFontMetrics mono(mono_font());
        const QFontMetrics prose(ui_font());
        // The width a field's text takes in the face the paint uses for it: the
        // type, name and comment fields are the ones the model leaves to the
        // grid's UI font.
        const auto         advance = [&mono, &prose](int column, const QString& text)
        {
            const bool prose_column = column == ReclassStructureModel::type || column == ReclassStructureModel::name
                                   || column == ReclassStructureModel::comment;
            return (prose_column ? prose : mono).horizontalAdvance(text);
        };
        const auto text_for = [this, &entry](int column)
        {
            if (entry.row->hidden)
            {
                return column == models::ReclassStructureModel::name ? QStringLiteral("...") : QString();
            }
            return model_->data(index_for(*entry.row, column), Qt::DisplayRole).toString();
        };

        const int y = kMargin + (row_index - first_row_) * row_height_;
        RowLayout layout;
        layout.rects[ReclassStructureModel::offset] = QRect(
            column_x_[ReclassStructureModel::offset], y, column_width_[ReclassStructureModel::offset], row_height_);
        layout.rects[ReclassStructureModel::address] = QRect(
            column_x_[ReclassStructureModel::address], y, column_width_[ReclassStructureModel::address], row_height_);
        layout.rects[ReclassStructureModel::ascii] =
            QRect(column_x_[ReclassStructureModel::ascii], y, column_width_[ReclassStructureModel::ascii], row_height_);

        // The row's own content starts at one slot: its indent and, for an
        // expandable row, the disclosure triangle come first, then the fields
        // flow one behind the other, an empty field leaving no room and no gap.
        int x = content_x_ + kTextPadding + static_cast<int>(entry.depth) * kIndentStep
              + (expandable(*entry.row) ? kTriangleWidth : 0);
        for (const int column : {ReclassStructureModel::type,
                                 ReclassStructureModel::name,
                                 ReclassStructureModel::value,
                                 ReclassStructureModel::comment,
                                 ReclassStructureModel::hints,
                                 ReclassStructureModel::pointer})
        {
            const QString text = text_for(column);
            if (text.isEmpty())
            {
                layout.rects[column] = QRect(x, y, 0, row_height_);
                continue;
            }
            const int width      = std::min(advance(column, text), column_width_[column]);
            layout.rects[column] = QRect(x, y, width, row_height_);
            x += width + gap_after(column);
        }
        return layout;
    }

    int ReClassView::field_at(const RowLayout& layout, int x) const noexcept
    {
        using models::ReclassStructureModel;
        int last = ReclassStructureModel::offset;
        for (int column = 0; column < ReclassStructureModel::column_count; ++column)
        {
            const QRect& rect = layout.rects[column];
            if (rect.width() <= 0)
            {
                continue;
            }
            if (x < rect.left())
            {
                return last;
            }
            if (x < rect.left() + rect.width())
            {
                return column;
            }
            last = column;
        }
        return last;
    }

    QRect ReClassView::cell_rect_at(int row_index, int column) const
    {
        if (row_index < 0 || row_index >= static_cast<int>(rows_.size()) || column < 0
            || column >= models::ReclassStructureModel::column_count)
        {
            return {};
        }
        return layout_row(rows_[static_cast<std::size_t>(row_index)], row_index).rects[column];
    }

    QRect ReClassView::triangle_rect(const VisualRow& entry, int row_index) const
    {
        const int x = content_x_ + kTextPadding + static_cast<int>(entry.depth) * kIndentStep;
        const int y = kMargin + (row_index - first_row_) * row_height_ + (row_height_ - kTriangleWidth) / 2;
        return QRect(x, y, kTriangleWidth, kTriangleWidth);
    }

    std::optional<ReClassView::Hit> ReClassView::hit_test(const QPoint& position) const
    {
        if (position.y() < kMargin)
        {
            return std::nullopt;
        }
        const int row_index = first_row_ + (position.y() - kMargin) / std::max(1, row_height_);
        if (row_index < first_row_ || row_index >= first_row_ + visible_rows_
            || row_index >= static_cast<int>(rows_.size()))
        {
            return std::nullopt;
        }

        const VisualRow& entry       = rows_[static_cast<std::size_t>(row_index)];
        const RowLayout  layout      = layout_row(entry, row_index);
        int              column      = field_at(layout, position.x());
        // The disclosure triangle stands at the head of the run, left of the
        // type text, so a click there acts on the row's expander rather than on
        // the field the layout would otherwise name.
        const bool       on_triangle = expandable(*entry.row) && triangle_rect(entry, row_index).contains(position);
        if (on_triangle)
        {
            column = models::ReclassStructureModel::type;
        }

        Hit hit;
        hit.row         = entry.row;
        hit.row_index   = row_index;
        hit.column      = column;
        hit.rect        = layout.rects[column];
        hit.on_triangle = on_triangle;

        // An array's arrows step the element on their own, so a click on one
        // never reaches the field the layout would otherwise name.
        if (!on_triangle && column == models::ReclassStructureModel::type)
        {
            const std::optional<ElementRects> control = element_rects_in(entry, row_index);
            if (control.has_value())
            {
                if (control->previous.contains(position))
                {
                    hit.element_step = -1;
                }
                else if (control->next.contains(position))
                {
                    hit.element_step = 1;
                }
            }
        }
        return hit;
    }

    void ReClassView::select_row(const Hit& hit, Qt::KeyboardModifiers modifiers)
    {
        const std::size_t node_id = hit.row->node_id;
        if (modifiers.testFlag(Qt::ControlModifier))
        {
            if (is_selected(*hit.row))
            {
                std::erase(selected_, node_id);
            }
            else
            {
                selected_.push_back(node_id);
            }
        }
        else if (modifiers.testFlag(Qt::ShiftModifier) && has_current_)
        {
            const int anchor = visual_index_of(current_);
            const int from   = std::min(anchor, hit.row_index);
            const int to     = std::max(anchor, hit.row_index);
            selected_.clear();
            for (int index = std::max(0, from); index <= to && index < static_cast<int>(rows_.size()); ++index)
            {
                selected_.push_back(rows_[static_cast<std::size_t>(index)].row->node_id);
            }
        }
        else
        {
            selected_.assign(1, node_id);
        }

        current_     = node_id;
        has_current_ = true;
        emit selectionChanged();
        viewport()->update();
    }

    void ReClassView::move_cursor(int row_index)
    {
        const int target = std::clamp(row_index, 0, static_cast<int>(rows_.size()) - 1);
        if (target < 0)
        {
            return;
        }
        current_     = rows_[static_cast<std::size_t>(target)].row->node_id;
        has_current_ = true;
        selected_.assign(1, current_);
        close_editor();
        ensure_visible(target);
        emit selectionChanged();
        viewport()->update();
    }

    void ReClassView::toggle_row(int row_index)
    {
        if (row_index < 0 || row_index >= static_cast<int>(rows_.size()))
        {
            return;
        }
        // The structure model owns what is inlined, so the toggle goes through
        // it and the rows come back rebuilt.
        const std::size_t node_id  = rows_[static_cast<std::size_t>(row_index)].row->node_id;
        const bool        expanded = structure_->is_expanded(node_id);
        structure_->set_expanded(node_id, !expanded);
        ensure_visible(visual_index_of(node_id));
        emit rowExpansionChanged();
    }

    void ReClassView::open_editor_at(int row_index, int column)
    {
        if (row_index < 0 || row_index >= static_cast<int>(rows_.size()))
        {
            return;
        }
        ensure_visible(row_index);
        const VisualRow& entry = rows_[static_cast<std::size_t>(row_index)];
        Hit              hit;
        hit.row       = entry.row;
        hit.row_index = row_index;
        hit.column    = column;
        hit.rect      = cell_rect_at(row_index, column);
        open_editor(hit);
    }

    void ReClassView::commit_edit()
    {
        if (!editing_ || editor_ == nullptr)
        {
            return;
        }
        const models::ReclassRow* row = row_for_node(target_.node_id);
        if (row == nullptr)
        {
            close_editor();
            return;
        }
        // The run writes through the structure model, which owns the node's own
        // number; one byte writes through the value model, the only path that
        // can address a part of a cell; a whole cell writes through the model.
        bool applied = false;
        if (target_.element)
        {
            applied = structure_->set_element_index(*row, editor_->text());
        }
        else if (target_.extent)
        {
            applied = structure_->set_node_extent(*row, editor_->text());
        }
        else if (target_.byte_index >= 0)
        {
            applied = values_->write_byte(*row, static_cast<std::size_t>(target_.byte_index), editor_->text());
        }
        else
        {
            applied = model_->setData(index_for(*row, target_.column), editor_->text(), Qt::EditRole);
        }
        if (applied)
        {
            close_editor();
            return;
        }
        // The write was refused with its reason on the status line; the cell
        // keeps the typed text so it can be corrected.
        editor_->selectAll();
    }

    void ReClassView::close_editor()
    {
        if (!editing_)
        {
            return;
        }
        editing_ = false;
        target_  = EditTarget {};
        if (editor_ != nullptr)
        {
            editor_->hide();
        }
    }

    void ReClassView::scroll_rows(long long delta)
    {
        if (delta == 0 || rows_.empty())
        {
            return;
        }
        const long long limit = static_cast<long long>(rows_.size()) - visible_rows_;
        const long long moved = std::clamp(static_cast<long long>(first_row_) + delta, 0LL, std::max(0LL, limit));
        if (moved == first_row_)
        {
            return;
        }
        first_row_ = static_cast<int>(moved);
        QToolTip::hideText();
        close_editor();
        viewport()->update();
    }

    void ReClassView::ensure_visible(int row_index)
    {
        if (row_index < 0)
        {
            return;
        }
        if (row_index < first_row_)
        {
            first_row_ = row_index;
        }
        else if (row_index >= first_row_ + visible_rows_)
        {
            first_row_ = row_index - visible_rows_ + 1;
        }
        first_row_ = std::clamp(first_row_, 0, std::max(0, static_cast<int>(rows_.size()) - visible_rows_));
        viewport()->update();
    }

    void ReClassView::resizeEvent(QResizeEvent* event)
    {
        QAbstractScrollArea::resizeEvent(event);
        update_view();
    }

    void ReClassView::changeEvent(QEvent* event)
    {
        switch (event->type())
        {
        case QEvent::FontChange:
        case QEvent::ApplicationFontChange:
        case QEvent::StyleChange:
            relayout();
            break;
        default:
            break;
        }
        QAbstractScrollArea::changeEvent(event);
    }

    void ReClassView::paintEvent(QPaintEvent* event)
    {
        Q_UNUSED(event);
        QPainter painter(viewport());
        paint_rows(painter);
    }

    void ReClassView::paint_rows(QPainter& painter) const
    {
        const Theme& theme = active_theme();
        painter.fillRect(viewport()->rect(), theme.surface);

        const int last = std::min(static_cast<int>(rows_.size()), first_row_ + visible_rows_);
        for (int row_index = first_row_; row_index < last; ++row_index)
        {
            const VisualRow& entry    = rows_[static_cast<std::size_t>(row_index)];
            const bool       selected = is_selected(*entry.row);
            const QRect band(0, kMargin + (row_index - first_row_) * row_height_, viewport()->width(), row_height_);

            if (row_index % 2 != 0)
            {
                painter.fillRect(band, theme.surface_hover);
            }
            if (selected)
            {
                painter.fillRect(band, theme.accent);
            }
            else if (is_current(*entry.row) || row_index == hovered_)
            {
                QColor tint = theme.accent;
                tint.setAlpha(kTintAlpha);
                painter.fillRect(band, tint);
            }

            const RowLayout layout = layout_row(entry, row_index);
            for (int column = 0; column < models::ReclassStructureModel::column_count; ++column)
            {
                paint_cell(painter, entry, column, layout.rects[column], selected);
            }
            if (expandable(*entry.row))
            {
                paint_triangle(painter, entry, row_index, selected);
            }
        }
    }

    void ReClassView::paint_cell(
        QPainter& painter, const VisualRow& entry, int column, const QRect& rect, bool selected) const
    {
        const QModelIndex index = index_for(*entry.row, column);

        const QFont        font(cell_font(index, *model_));
        const QFontMetrics metrics(font);
        painter.setFont(font);

        // Whatever colour the row set on the cell stands; without one the grid
        // paints its own text colour, or the on-accent one when selected.
        const std::optional<QColor> stated = cell_colour(index, *model_);
        const QColor primary               = selected ? active_theme().on_accent : stated.value_or(active_theme().text);
        painter.setPen(primary);

        // A hidden node collapses to `...` at the head of its content slot and
        // shows nothing in the other fields, the reference's collapsed
        // placeholder.
        QString text;
        if (entry.row->hidden)
        {
            text = column == models::ReclassStructureModel::name ? QStringLiteral("...") : QString();
        }
        else
        {
            text = model_->data(index, Qt::DisplayRole).toString();
        }
        if (text.isEmpty())
        {
            return;
        }

        // An aligned field's text sits on its content box; a flowed field's run
        // is its own rect, so the text starts were the layout placed it.
        const bool  aligned   = column == models::ReclassStructureModel::offset
                             || column == models::ReclassStructureModel::address
                             || column == models::ReclassStructureModel::ascii;
        const int   x         = aligned ? aligned_x(entry, column) : rect.x();
        const int   available = std::max(0, rect.x() + rect.width() - x);
        const QRect text_rect(x, rect.y(), available, rect.height());

        // The value cell paints in runs: the node's own bytes in the grid's text
        // colour except the pairs whose byte moved since the last pass, which
        // light warning, and the readings beside them in the readout colour, so a
        // hex cell's float and integer stay visibly apart from its bytes. A kind
        // that draws no byte pair warns whole when any of its bytes moved. A
        // selected row, a stated colour (an unreadable block) and a value too
        // wide for its column all paint whole instead.
        if (column == models::ReclassStructureModel::value && !selected && !stated.has_value())
        {
            const std::vector<reclass::ValuePart> parts = values_->value_parts(*entry.row);
            const std::span<const std::uint8_t>   moved = values_->node_changed_bytes(*entry.row);
            const bool                            any   = std::ranges::any_of(moved,
                                                                              [](std::uint8_t flag)
                                                                              {
                                                     return flag != 0;
                                                                              });
            int                                   total = 0;
            for (const reclass::ValuePart& part : parts)
            {
                total += metrics.horizontalAdvance(to_qstring(part.text));
            }
            if (!parts.empty() && total <= available)
            {
                int run_x = x;
                for (const reclass::ValuePart& part : parts)
                {
                    const QString run   = to_qstring(part.text);
                    const int     width = metrics.horizontalAdvance(run);
                    painter.setPen(part_colour(part, moved, any, primary));
                    painter.drawText(
                        QRect(run_x, rect.y(), width, rect.height()), Qt::AlignLeft | Qt::AlignVCenter, run);
                    run_x += width;
                }
                return;
            }

            // The value does not fit the column; it elides whole, so its moved
            // bytes cannot be singled out.
            painter.setPen(any ? active_theme().warning : primary);
        }

        painter.drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter, fitted_run(metrics, text, available));

        // An array's arrows and its bracketed index read as a control of their
        // own rather than as the kind the cell names, so the three runs are
        // painted over the text that was just drawn. A selected row keeps one
        // colour over the whole cell, and an elided text leaves no fixed place
        // for the runs.
        if (column != models::ReclassStructureModel::type || selected || entry.row->hidden)
        {
            return;
        }
        const models::ElementRuns         runs    = structure_->element_runs(entry.row->node_id);
        const std::optional<ElementRects> control = control_rects(rect, metrics, text, runs);
        if (!control.has_value())
        {
            return;
        }
        painter.setPen(active_theme().grid_index);
        painter.drawText(control->previous, Qt::AlignLeft | Qt::AlignVCenter, text.mid(runs.previous, 1));
        painter.drawText(control->index, Qt::AlignLeft | Qt::AlignVCenter, text.mid(runs.index, runs.index_length));
        painter.drawText(control->next, Qt::AlignLeft | Qt::AlignVCenter, text.mid(runs.next, 1));
        painter.setPen(primary);
    }

    void ReClassView::paint_triangle(QPainter& painter, const VisualRow& entry, int row_index, bool selected) const
    {
        const QRect rect     = triangle_rect(entry, row_index);
        const bool  expanded = structure_->is_expanded(entry.row->node_id);

        QPolygon arrow;
        if (expanded)
        {
            arrow << QPoint(rect.x() + 1, rect.y() + 2) << QPoint(rect.x() + 7, rect.y() + 2)
                  << QPoint(rect.x() + 4, rect.y() + 6);
        }
        else
        {
            arrow << QPoint(rect.x() + 2, rect.y() + 1) << QPoint(rect.x() + 2, rect.y() + 7)
                  << QPoint(rect.x() + 6, rect.y() + 4);
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(selected ? active_theme().on_accent : active_theme().text_muted);
        painter.drawPolygon(arrow);
    }

    void ReClassView::mousePressEvent(QMouseEvent* event)
    {
        if (event->button() != Qt::LeftButton)
        {
            QAbstractScrollArea::mousePressEvent(event);
            return;
        }

        const std::optional<Hit> hit = hit_test(event->position().toPoint());
        if (!hit.has_value())
        {
            if (editor_ != nullptr && editing_)
            {
                close_editor();
            }
            QAbstractScrollArea::mousePressEvent(event);
            return;
        }

        setFocus(Qt::MouseFocusReason);
        if (hit->on_triangle)
        {
            select_row(*hit, Qt::NoModifier);
            toggle_row(hit->row_index);
        }
        else
        {
            select_row(*hit, event->modifiers());
            if (hit->element_step != 0 && hit->row != nullptr)
            {
                // An arrow walks the array's elements, so the row shows another
                // one; the selection keeps its place.
                structure_->step_element(hit->row->node_id, hit->element_step);
            }
            // Clicking a hidden node's `...` row unhides it, the reference's
            // click-to-reveal.
            else if (hit->row != nullptr && hit->row->hidden)
            {
                emit hiddenRowActivated(hit->row->node_id);
            }
        }
        event->accept();
    }

    void ReClassView::mouseMoveEvent(QMouseEvent* event)
    {
        const std::optional<Hit> hit      = hit_test(event->position().toPoint());
        const bool               on_arrow = hit.has_value() && hit->element_step != 0;
        const int                hovered  = hit.has_value() && !on_arrow ? hit->row_index : -1;
        if (hovered != hovered_)
        {
            hovered_ = hovered;
            viewport()->update();
        }

        if (on_arrow)
        {
            // An arrow takes the pointing hand and explains the element it shows.
            viewport()->setCursor(Qt::PointingHandCursor);
            QToolTip::showText(
                event->globalPosition().toPoint(), element_tooltip(*hit->row, hit->element_step), viewport());
            event->accept();
            return;
        }
        viewport()->unsetCursor();

        const QString full = hover_full_text(event->position().toPoint());
        if (full.isEmpty())
        {
            QToolTip::hideText();
        }
        else
        {
            QToolTip::showText(event->globalPosition().toPoint(), full, viewport());
        }
        event->accept();
    }

    void ReClassView::mouseDoubleClickEvent(QMouseEvent* event)
    {
        if (event->button() != Qt::LeftButton)
        {
            QAbstractScrollArea::mouseDoubleClickEvent(event);
            return;
        }

        const std::optional<Hit> hit = hit_test(event->position().toPoint());
        if (!hit.has_value())
        {
            QAbstractScrollArea::mouseDoubleClickEvent(event);
            return;
        }

        // A byte of a hex dump or of the Ascii column is typed into on its own,
        // when the row's value is writable and the point resolves to a byte the
        // row covers; every other cell keeps the rule below.
        const bool byte_column =
            hit->column == models::ReclassStructureModel::value || hit->column == models::ReclassStructureModel::ascii;
        if (byte_column && values_->byte_editable(*hit->row))
        {
            const VisualRow& entry      = rows_[static_cast<std::size_t>(hit->row_index)];
            const int        byte_index = byte_at(entry, hit->row_index, hit->column, event->position().toPoint());
            if (byte_index >= 0)
            {
                open_byte_editor(hit->row_index, hit->column, byte_index);
                event->accept();
                return;
            }
        }

        // The Type cell's index control is typed into on its own: the two arrows
        // walk the elements on a single click, while the bracketed index between
        // them opens the editor over it.
        if (hit->column == models::ReclassStructureModel::type)
        {
            const VisualRow&                  entry   = rows_[static_cast<std::size_t>(hit->row_index)];
            const std::optional<ElementRects> control = element_rects_in(entry, hit->row_index);
            if (control.has_value() && control->index.contains(event->position().toPoint()))
            {
                open_element_editor(hit->row_index, control->index);
                event->accept();
                return;
            }
        }

        // The Type cell's bracketed count is typed into on its own: a
        // double-click on the run opens the editor over it, while a click
        // elsewhere in the cell keeps the rules below.
        if (hit->column == models::ReclassStructureModel::type)
        {
            const VisualRow&           entry = rows_[static_cast<std::size_t>(hit->row_index)];
            const std::optional<QRect> run   = extent_rect_in(entry, hit->row_index, event->position().toPoint());
            if (run.has_value())
            {
                open_extent_editor(hit->row_index, *run);
                event->accept();
                return;
            }
        }

        if (model_->flags(index_for(*hit->row, hit->column)).testFlag(Qt::ItemIsEditable))
        {
            // Either a value the write path accepts or the node's own name.
            open_editor(*hit);
            event->accept();
            return;
        }

        if (expandable(*hit->row))
        {
            const int row_index = hit->row_index;
            toggle_row(row_index);
            event->accept();
            return;
        }
        QAbstractScrollArea::mouseDoubleClickEvent(event);
    }

    void ReClassView::open_editor(const Hit& hit)
    {
        if (hit.row == nullptr || hit.column < 0)
        {
            return;
        }

        const QModelIndex index = index_for(*hit.row, hit.column);
        if (editor_ == nullptr)
        {
            editor_ = new QLineEdit(viewport());
            editor_->setObjectName(QStringLiteral("reclass_view_editor"));
            editor_->setFont(mono_font());
            editor_->installEventFilter(this);
            connect(editor_,
                    &QLineEdit::editingFinished,
                    this,
                    [this]
                    {
                        // Return commits in the event filter, so a focus loss is
                        // the only way here: it discards the typed text.
                        if (editing_)
                        {
                            close_editor();
                        }
                    });
        }

        editing_ = true;
        target_  = EditTarget {.node_id    = hit.row->node_id,
                               .column     = hit.column,
                               .byte_index = hit.byte_index,
                               .extent     = hit.extent,
                               .element    = hit.element,
                               .rect       = hit.rect};
        editor_->setFont(cell_font(index, *model_));
        // The editor shows the plain text the model answers for the cell - the
        // byte the row draws, bare and upper case, for a per-byte edit - and not
        // the readings the cell draws beside a value.
        editor_->setText(edit_text(*hit.row));
        // A node with no note draws only its marker, so the editor is opened
        // wide enough to type a note into without re-laying the row out.
        QRect rect = hit.rect;
        if (hit.column == models::ReclassStructureModel::comment)
        {
            rect.setWidth(std::max(rect.width(), kCommentEditorMinWidth));
        }
        else if (hit.byte_index >= 0)
        {
            // A pair-wide box would clip the two digits it holds.
            const QFontMetrics metrics(cell_font(index, *model_));
            rect.setWidth(std::max(rect.width(), metrics.horizontalAdvance(QStringLiteral("0000"))));
        }
        else if (hit.extent || hit.element)
        {
            // The run's own box is as wide as its brackets, and the index's as
            // wide as its digits; a number needs the room a hex pair is given,
            // so typing past a single digit is never clipped.
            const QFontMetrics metrics(cell_font(index, *model_));
            rect.setWidth(std::max(rect.width(), metrics.horizontalAdvance(QStringLiteral("0000"))));
        }
        editor_->setGeometry(rect);
        editor_->show();
        editor_->setFocus(Qt::MouseFocusReason);
        editor_->selectAll();
    }

    QString ReClassView::edit_text(const models::ReclassRow& row) const
    {
        if (target_.element)
        {
            return QString::number(structure_->element_index(row.node_id));
        }
        if (target_.byte_index >= 0)
        {
            const std::span<const std::byte> bytes  = values_->node_bytes(row);
            const auto                       offset = static_cast<std::size_t>(target_.byte_index);
            if (offset >= bytes.size())
            {
                return {};
            }
            const unsigned value = std::to_integer<unsigned>(bytes[offset]);
            return QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper();
        }
        return model_->data(index_for(row, target_.column), Qt::EditRole).toString();
    }

    void ReClassView::open_byte_editor(int row_index, int column, int byte_index)
    {
        if (row_index < 0 || row_index >= static_cast<int>(rows_.size()) || byte_index < 0)
        {
            return;
        }
        ensure_visible(row_index);
        const VisualRow&           entry = rows_[static_cast<std::size_t>(row_index)];
        const std::optional<QRect> rect  = byte_rect_in(entry, row_index, column, static_cast<std::size_t>(byte_index));
        if (!rect.has_value())
        {
            return;
        }

        Hit hit;
        hit.row        = entry.row;
        hit.row_index  = row_index;
        hit.column     = column;
        hit.rect       = *rect;
        hit.byte_index = byte_index;
        open_editor(hit);
    }

    void ReClassView::open_extent_editor(int row_index, const QRect& run)
    {
        if (row_index < 0 || row_index >= static_cast<int>(rows_.size()))
        {
            return;
        }
        ensure_visible(row_index);
        const VisualRow& entry = rows_[static_cast<std::size_t>(row_index)];

        Hit hit;
        hit.row       = entry.row;
        hit.row_index = row_index;
        hit.column    = models::ReclassStructureModel::type;
        hit.rect      = run;
        hit.extent    = true;
        open_editor(hit);
    }

    void ReClassView::open_element_editor(int row_index, const QRect& run)
    {
        if (row_index < 0 || row_index >= static_cast<int>(rows_.size()))
        {
            return;
        }
        ensure_visible(row_index);
        const VisualRow& entry = rows_[static_cast<std::size_t>(row_index)];

        Hit hit;
        hit.row       = entry.row;
        hit.row_index = row_index;
        hit.column    = models::ReclassStructureModel::type;
        hit.rect      = run;
        hit.element   = true;
        open_editor(hit);
    }

    void ReClassView::contextMenuEvent(QContextMenuEvent* event)
    {
        // The reference selects the node under the cursor before it opens the
        // menu, as long as at most one node was selected, so the commands act on
        // the row the menu was opened over rather than on a stale selection.
        const std::optional<Hit> hit = hit_test(viewport()->mapFromGlobal(event->globalPos()));
        if (hit.has_value() && selected_.size() <= 1)
        {
            select_row(*hit, Qt::NoModifier);
        }
        emit rowMenuRequested(event->globalPos());
    }

    void ReClassView::wheelEvent(QWheelEvent* event)
    {
        const int steps = event->angleDelta().y() / 120;
        if (steps == 0)
        {
            QAbstractScrollArea::wheelEvent(event);
            return;
        }
        scroll_rows(-static_cast<long long>(steps) * kWheelRows);
        event->accept();
    }

    void ReClassView::leaveEvent(QEvent* event)
    {
        hovered_ = -1;
        QToolTip::hideText();
        viewport()->unsetCursor();
        viewport()->update();
        QAbstractScrollArea::leaveEvent(event);
    }

    void ReClassView::keyPressEvent(QKeyEvent* event)
    {
        const int current = has_current_ ? visual_index_of(current_) : -1;
        const int rows    = static_cast<int>(rows_.size());

        switch (event->key())
        {
        case Qt::Key_Up:
            move_cursor(current < 0 ? 0 : current - 1);
            break;
        case Qt::Key_Down:
            move_cursor(current < 0 ? 0 : current + 1);
            break;
        case Qt::Key_Home:
            move_cursor(0);
            break;
        case Qt::Key_End:
            move_cursor(rows - 1);
            break;
        case Qt::Key_PageUp:
            move_cursor((current < 0 ? 0 : current) - visible_rows_);
            break;
        case Qt::Key_PageDown:
            move_cursor((current < 0 ? 0 : current) + visible_rows_);
            break;
        case Qt::Key_Left:
            if (current >= 0 && expandable(*rows_[static_cast<std::size_t>(current)].row)
                && structure_->is_expanded(rows_[static_cast<std::size_t>(current)].row->node_id))
            {
                toggle_row(current);
            }
            break;
        case Qt::Key_Right:
            if (current >= 0 && expandable(*rows_[static_cast<std::size_t>(current)].row)
                && !structure_->is_expanded(rows_[static_cast<std::size_t>(current)].row->node_id))
            {
                toggle_row(current);
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (current >= 0
                && model_
                       ->flags(index_for(*rows_[static_cast<std::size_t>(current)].row,
                                         models::ReclassStructureModel::value))
                       .testFlag(Qt::ItemIsEditable))
            {
                open_editor_at(current, models::ReclassStructureModel::value);
            }
            break;
        case Qt::Key_F3:
            // A note is typed into without a target, like the name beside it.
            if (current >= 0
                && model_
                       ->flags(index_for(*rows_[static_cast<std::size_t>(current)].row,
                                         models::ReclassStructureModel::comment))
                       .testFlag(Qt::ItemIsEditable))
            {
                open_editor_at(current, models::ReclassStructureModel::comment);
            }
            break;
        case Qt::Key_F2:
            // A name is typed into without a target, so it opens even when the
            // value cell of the same row is read-only; an array's element takes
            // no name, so its read-only cell opens nothing.
            if (current >= 0
                && model_
                       ->flags(index_for(*rows_[static_cast<std::size_t>(current)].row,
                                         models::ReclassStructureModel::name))
                       .testFlag(Qt::ItemIsEditable))
            {
                open_editor_at(current, models::ReclassStructureModel::name);
            }
            break;
        default:
            QAbstractScrollArea::keyPressEvent(event);
            return;
        }
        event->accept();
    }

    bool ReClassView::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == editor_ && event->type() == QEvent::KeyPress)
        {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape)
            {
                close_editor();
                return true;
            }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
            {
                // A line editor hands an unhandled Return up to its parent, where
                // a dialog would press its own default button; the edit belongs to
                // the cell, so the key stops here.
                commit_edit();
                return true;
            }
        }
        return QAbstractScrollArea::eventFilter(watched, event);
    }

}
