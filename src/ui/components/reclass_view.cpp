#include "ui/components/reclass_view.hpp"

#include <algorithm>
#include <cstdint>
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
#include <QToolTip>
#include <QVariant>
#include <QWheelEvent>

#include "ui/address_format.hpp"
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
        constexpr int kMargin         = 4;
        constexpr int kTextPadding    = 4;
        // The room the widest text a column was measured for leaves before the
        // next column's text: a grid whose columns are sized once should not
        // spend more than a few pixels between them.
        constexpr int kColumnGap      = 12;
        constexpr int kIndentStep     = 12;
        constexpr int kTriangleWidth  = 8;
        constexpr int kMinColumnWidth = 24;
        constexpr int kWheelRows      = 3;
        // The cursor and hover tints are drawn over the stripes rather than
        // beside them, so both kinds of row keep the same tint.
        constexpr int kTintAlpha      = 36;

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

        // The indent a row's text starts at: the address and offset columns stay
        // flush so their digits line up, everything after them follows the tree.
        constexpr bool indents(int column)
        {
            using models::ReclassStructureModel;
            return column != ReclassStructureModel::address && column != ReclassStructureModel::offset
                && column != ReclassStructureModel::ascii;
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
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        viewport()->setMouseTracking(true);

        update_layout();
        update_rows();

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
                    // A rename is the one live change that can move a column
                    // boundary; a value pass never re-measures.
                    if (top_left.column() == models::ReclassStructureModel::name
                        && bottom_right.column() == models::ReclassStructureModel::name)
                    {
                        update_content_widths();
                        update_columns();
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

    int ReClassView::column_x(int column) const noexcept
    {
        if (column < 0 || column >= models::ReclassStructureModel::column_count)
        {
            return 0;
        }
        return column_x_[column];
    }

    int ReClassView::column_width(int column) const noexcept
    {
        if (column < 0 || column >= models::ReclassStructureModel::column_count)
        {
            return 0;
        }
        return column_width_[column];
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

        // The address is always the padded form, so it keeps a width measured
        // from the font once here and never again; the comment column starts at
        // its header word and is widened to the notes the class draws (see
        // update_content_widths); the columns between them are sized to the
        // text the class draws.
        const auto measured = [](const QFontMetrics& metrics, const QString& sample)
        {
            return metrics.horizontalAdvance(sample) + kColumnGap;
        };

        using models::ReclassStructureModel;
        column_width_[ReclassStructureModel::address] = measured(mono, format_padded_hex(0));
        column_width_[ReclassStructureModel::ascii]   = measured(mono, tr("Ascii"));
        comment_content_                              = measured(prose, tr("Comment"));
        column_width_[ReclassStructureModel::comment] = comment_content_;
        column_width_[ReclassStructureModel::value]   = 0;
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

        // A narrow column is only as wide as the text this class really draws,
        // so a list of small nodes does not sit in a grid sized for the widest
        // kind in the language. This follows the flattened rows, which a live
        // pass never rebuilds.
        int offset_width  = kMinColumnWidth;
        int ascii_width   = kMinColumnWidth;
        int type_width    = kMinColumnWidth;
        int name_width    = kMinColumnWidth;
        int comment_width = prose.horizontalAdvance(tr("Comment")) + kColumnGap;
        for (const VisualRow& entry : rows_)
        {
            const int indent = static_cast<int>(entry.depth) * kIndentStep;
            offset_width = std::max(offset_width,
                                    mono.horizontalAdvance(display(entry, ReclassStructureModel::offset)) + kColumnGap);
            ascii_width  = std::max(ascii_width,
                                    mono.horizontalAdvance(display(entry, ReclassStructureModel::ascii)) + kColumnGap);
            type_width   = std::max(type_width,
                                    prose.horizontalAdvance(display(entry, ReclassStructureModel::type)) + kColumnGap
                                        + indent + (expandable(*entry.row) ? kTriangleWidth : 0));
            name_width   = std::max(
                name_width, prose.horizontalAdvance(display(entry, ReclassStructureModel::name)) + kColumnGap + indent);
            comment_width = std::max(
                comment_width, prose.horizontalAdvance(display(entry, ReclassStructureModel::comment)) + kColumnGap);
        }

        column_width_[ReclassStructureModel::offset] = offset_width;
        column_width_[ReclassStructureModel::ascii]  = ascii_width;
        column_width_[ReclassStructureModel::type]   = type_width;
        column_width_[ReclassStructureModel::name]   = name_width;
        comment_content_                             = comment_width;
    }

    void ReClassView::update_columns()
    {
        using models::ReclassStructureModel;

        // The first four columns keep the widths they were measured for; the
        // value column takes whatever the pane has left, and the comment column
        // is pinned to the right edge, sized to the notes the class draws but
        // clamped so it can never starve the value column.
        int x = kMargin;
        for (int column = 0; column <= ReclassStructureModel::name; ++column)
        {
            column_x_[column] = x;
            x += column_width_[column];
        }

        const QFontMetrics prose(ui_font());
        const int          floor_width                = prose.horizontalAdvance(tr("Comment")) + kColumnGap;
        const int          cap                        = std::max(floor_width, viewport()->width() / 3);
        const int          comment_width              = std::clamp(comment_content_, floor_width, cap);
        column_width_[ReclassStructureModel::comment] = comment_width;

        column_x_[ReclassStructureModel::value] = x;
        const int comment = std::max(x + kMinColumnWidth, viewport()->width() - comment_width - kMargin);
        column_width_[ReclassStructureModel::value] = comment - x;
        column_x_[ReclassStructureModel::comment]   = comment;
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
        const QRect        rect      = cell_rect_at(hit->row_index, hit->column);
        const int          available = std::max(0, rect.x() + rect.width() - content_x(entry, hit->column));
        const QFontMetrics metrics(cell_font(index, *model_));
        return metrics.elidedText(full, Qt::ElideRight, available) == full ? QString() : full;
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

    int ReClassView::content_x(const VisualRow& entry, int column) const noexcept
    {
        int x = column_x_[column] + kTextPadding;
        if (!indents(column))
        {
            return x;
        }
        x += static_cast<int>(entry.depth) * kIndentStep;
        if (column == models::ReclassStructureModel::type && expandable(*entry.row))
        {
            x += kTriangleWidth;
        }
        return x;
    }

    QRect ReClassView::cell_rect_at(int row_index, int column) const
    {
        const int y = kMargin + (row_index - first_row_) * row_height_;
        return QRect(column_x_[column], y, column_width(column), row_height_);
    }

    QRect ReClassView::triangle_rect(const VisualRow& entry, int row_index) const
    {
        const int x =
            column_x_[models::ReclassStructureModel::type] + kTextPadding + static_cast<int>(entry.depth) * kIndentStep;
        const int y = kMargin + (row_index - first_row_) * row_height_ + (row_height_ - kTriangleWidth) / 2;
        return QRect(x, y, kTriangleWidth, kTriangleWidth);
    }

    int ReClassView::column_at(int x) const noexcept
    {
        if (x < column_x_[0])
        {
            return 0;
        }
        int column = models::ReclassStructureModel::column_count - 1;
        while (column > 0 && x < column_x_[column])
        {
            --column;
        }
        return column;
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

        const VisualRow& entry  = rows_[static_cast<std::size_t>(row_index)];
        const int        column = column_at(position.x());

        Hit hit;
        hit.row         = entry.row;
        hit.row_index   = row_index;
        hit.column      = column;
        hit.rect        = cell_rect_at(row_index, column);
        hit.on_triangle = expandable(*entry.row) && column == models::ReclassStructureModel::type
                       && triangle_rect(entry, row_index).contains(position);
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
        commit_ran_                   = true;
        const models::ReclassRow* row = row_for_node(edit_node_id_);
        if (row == nullptr)
        {
            close_editor();
            return;
        }
        const QModelIndex index = index_for(*row, edit_column_);
        if (model_->setData(index, editor_->text(), Qt::EditRole))
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
        editing_      = false;
        edit_node_id_ = 0;
        commit_ran_   = false;
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

            for (int column = 0; column < models::ReclassStructureModel::column_count; ++column)
            {
                paint_cell(painter, entry, column, cell_rect_at(row_index, column), selected);
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

        // A hidden node collapses to `...` in the name column and shows nothing
        // in the others, the reference's collapsed placeholder.
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

        const int   x         = content_x(entry, column);
        const int   available = std::max(0, rect.x() + rect.width() - x);
        const QRect text_rect(x, rect.y(), available, rect.height());

        // The value cell paints in runs: the node's own bytes in the grid's text
        // colour, then the readings beside them in the readout colour, so a hex
        // cell's float and integer stay visibly apart from its bytes. A selected
        // row, a stated colour (unreadable or changed) and a value too wide for
        // its column all paint whole instead.
        if (column == models::ReclassStructureModel::value && !selected && !stated.has_value())
        {
            const std::vector<reclass::ValuePart> parts = values_->value_parts(*entry.row);
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
                    painter.setPen(part.reading ? active_theme().datatype_readout : primary);
                    painter.drawText(
                        QRect(run_x, rect.y(), width, rect.height()), Qt::AlignLeft | Qt::AlignVCenter, run);
                    run_x += width;
                }
                return;
            }
            painter.setPen(primary);
        }

        painter.drawText(
            text_rect, Qt::AlignLeft | Qt::AlignVCenter, metrics.elidedText(text, Qt::ElideRight, available));
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
            // Clicking a hidden node's `...` row unhides it, the reference's
            // click-to-reveal.
            if (hit->row != nullptr && hit->row->hidden)
            {
                emit hiddenRowActivated(hit->row->node_id);
            }
        }
        event->accept();
    }

    void ReClassView::mouseMoveEvent(QMouseEvent* event)
    {
        const std::optional<Hit> hit     = hit_test(event->position().toPoint());
        const int                hovered = hit.has_value() ? hit->row_index : -1;
        if (hovered != hovered_)
        {
            hovered_ = hovered;
            viewport()->update();
        }

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
            connect(editor_, &QLineEdit::returnPressed, this, &ReClassView::commit_edit);
            connect(editor_,
                    &QLineEdit::editingFinished,
                    this,
                    [this]
                    {
                        // Return emits this right after returnPressed, so a
                        // commit that was refused keeps the editor open for the
                        // correction; a focus loss discards the text instead.
                        if (commit_ran_)
                        {
                            commit_ran_ = false;
                            return;
                        }
                        if (editing_)
                        {
                            close_editor();
                        }
                    });
        }

        editing_      = true;
        edit_node_id_ = hit.row->node_id;
        edit_column_  = hit.column;
        // The editor shows the plain text the model answers for the cell, not the
        // readings a value cell draws beside it.
        editor_->setFont(cell_font(index, *model_));
        editor_->setText(model_->data(index, Qt::EditRole).toString());
        editor_->setGeometry(hit.rect);
        editor_->show();
        editor_->setFocus(Qt::MouseFocusReason);
        editor_->selectAll();
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
        case Qt::Key_F2:
            // A name is typed into without a target, so it opens even when the
            // value cell of the same row is read-only.
            if (current >= 0)
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
            if (auto* key = static_cast<QKeyEvent*>(event); key->key() == Qt::Key_Escape)
            {
                close_editor();
                return true;
            }
        }
        return QAbstractScrollArea::eventFilter(watched, event);
    }

}
