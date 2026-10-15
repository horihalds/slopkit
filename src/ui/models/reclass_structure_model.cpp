#include "ui/models/reclass_structure_model.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "reclass/edit.hpp"
#include "reclass/node.hpp"
#include "reclass/nodes/container.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/datatype_palette.hpp"
#include "ui/fonts.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui::models
{
    namespace
    {
        const ReclassRow* find_row(const std::vector<std::unique_ptr<ReclassRow>>& rows, std::size_t node_id)
        {
            for (const std::unique_ptr<ReclassRow>& row : rows)
            {
                if (row->node_id == node_id)
                {
                    return row.get();
                }
                if (const ReclassRow* found = find_row(row->children, node_id); found != nullptr)
                {
                    return found;
                }
            }
            return nullptr;
        }

        bool is_marked(const ReclassRow& row)
        {
            return row.unknown_class || row.cycle || row.too_deep;
        }

        // The number the Type cell brackets, when the row offers one: a hidden
        // placeholder or a marked reference row answers nothing, exactly as its
        // cell text does.
        std::optional<std::size_t> run_extent(const ReclassRow& row, const reclass::Node& node)
        {
            if (row.hidden || is_marked(row))
            {
                return std::nullopt;
            }
            return node.extent();
        }

        // A note is drawn the way it would be written in source: prefixed, and
        // drawn as the bare marker when there is no note to draw.
        QString comment_text(const std::string& comment)
        {
            return comment.empty() ? QStringLiteral("//") : QStringLiteral("// ") + to_qstring(comment);
        }

        // The index control an array's Type cell draws - the two arrows around
        // the bracketed index - which the grid locates and paints on the cell's
        // own text.
        QString index_control(std::size_t index)
        {
            return QStringLiteral("◀ (%1) ▶").arg(index);
        }

        // The character offsets of that control inside the text ending with it.
        ElementRuns runs_of(const QString& label, std::size_t index)
        {
            const QString control = index_control(index);
            ElementRuns   runs;
            runs.previous     = static_cast<int>(label.size()) + 1;
            // Past the `◀`, the space and the opening bracket.
            runs.index        = runs.previous + 3;
            runs.index_length = static_cast<int>(QString::number(index).size());
            runs.next         = runs.previous + static_cast<int>(control.size()) - 1;
            return runs;
        }

        // The kind name of one element of a plain array, empty when the array
        // holds no element at that index.
        QString element_kind(const reclass::Node& node, std::size_t index)
        {
            const auto* container = dynamic_cast<const reclass::ContainerNode*>(&node);
            if (container == nullptr)
            {
                return {};
            }
            const std::vector<reclass::NodePtr>& children = container->children_mutable();
            return index < children.size() ? to_qstring(reclass::describe(children[index]->kind())) : QString {};
        }
    }

    ReclassStructureModel::ReclassStructureModel(reclass::Project& project, QObject* parent)
        : QAbstractItemModel(parent), project_(&project)
    {
        valid_class_ = project.class_at(0) != nullptr;
        rebuild();
    }

    void ReclassStructureModel::set_project(reclass::Project& project)
    {
        project_        = &project;
        selected_class_ = 0;
        valid_class_    = project.class_at(0) != nullptr;
        expanded_.clear();
        element_indices_.clear();
        rebuild();
    }

    void ReclassStructureModel::set_class(std::size_t index)
    {
        if (index != selected_class_)
        {
            expanded_.clear();
            element_indices_.clear();
        }
        selected_class_ = index;
        valid_class_    = project_ != nullptr && project_->class_at(index) != nullptr;
        rebuild();
    }

    void ReclassStructureModel::set_expanded(std::size_t node_id, bool expanded)
    {
        const bool changed = expanded ? expanded_.insert(node_id).second : expanded_.erase(node_id) > 0;
        if (!changed)
        {
            return;
        }
        rebuild();
    }

    void ReclassStructureModel::rebuild()
    {
        // The index rides on a node's own row, so an index whose node stopped
        // being an array - re-typed away, reduced to nothing - goes with it. The
        // project is asked, never the rows about to be dropped: their nodes may
        // already be freed.
        if (project_ == nullptr)
        {
            element_indices_.clear();
        }
        else
        {
            std::erase_if(element_indices_,
                          [this](const auto& entry)
                          {
                              const std::optional<reclass::NodeRef> ref = reclass::node_ref(*project_, entry.first);
                              const reclass::Node* node = ref.has_value() ? (*ref->owner)[ref->index].get() : nullptr;
                              return node == nullptr || element_count(*node) == 0;
                          });
        }

        beginResetModel();
        build_rows();
        endResetModel();
        emit blocksChanged();
    }

    bool ReclassStructureModel::set_node_comment(const ReclassRow& row, const QString& text)
    {
        // A hidden node is the collapsed `...` placeholder of a whole collapsed
        // run, so its row takes no note of its own.
        if (row.node == nullptr || row.hidden)
        {
            return false;
        }
        const auto ref = reclass::node_ref(*project_, row.node_id);
        if (!ref)
        {
            return false;
        }
        const QString trimmed              = text.trimmed();
        (*ref->owner)[ref->index]->comment = trimmed.toStdString();

        // A note moves no offset, so the row objects stay and only the one cell
        // is announced; the grid re-measures that column on it.
        const QModelIndex base = index_for_node(row.node_id);
        if (base.isValid())
        {
            const QModelIndex changed = index(base.row(), ReclassStructureModel::comment, base.parent());
            emit              dataChanged(changed, changed);
        }
        emit statusChanged(trimmed.isEmpty() ? tr("Comment cleared.") : tr("Comment saved."), false);
        return true;
    }

    bool ReclassStructureModel::set_node_name(const ReclassRow& row, const QString& name)
    {
        const QString trimmed = name.trimmed();
        if (trimmed.isEmpty())
        {
            emit statusChanged(tr("A node name is required."), true);
            return false;
        }

        if (row.node == nullptr)
        {
            return false;
        }

        // An array's element is identified by its place in the array rather than
        // by a name of its own, so it takes none; the cell the grid draws for it
        // is read-only for the same reason.
        if (is_element_row(row))
        {
            emit statusChanged(tr("An array's element takes no name."), true);
            return false;
        }

        const auto ref = reclass::node_ref(*project_, row.node_id);
        if (!ref)
        {
            return false;
        }
        (*ref->owner)[ref->index]->name = trimmed.toStdString();

        // A rename moves no offset, so the row objects stay and only the one
        // cell is announced; the grid re-measures that column on it.
        const QModelIndex base = index_for_node(row.node_id);
        if (base.isValid())
        {
            const QModelIndex changed = index(base.row(), ReclassStructureModel::name, base.parent());
            emit              dataChanged(changed, changed);
        }
        emit statusChanged(tr("Node renamed."), false);
        return true;
    }

    bool ReclassStructureModel::set_node_extent(const ReclassRow& row, const QString& text)
    {
        if (row.node == nullptr || row.hidden || is_marked(row))
        {
            return false;
        }
        const auto ref = reclass::node_ref(*project_, row.node_id);
        if (!ref)
        {
            return false;
        }

        reclass::Node& node = *(*ref->owner)[ref->index];
        if (!run_extent(row, node).has_value())
        {
            return false;
        }

        // The two wording choices are the only kind knowledge here; the number
        // itself is the node's own, so a kind without one never gets this far.
        const bool is_text = node.kind() == reclass::NodeKind::utf8_text || node.kind() == reclass::NodeKind::utf16_text
                          || node.kind() == reclass::NodeKind::utf32_text;
        const QString noun = is_text ? tr("Length") : tr("Count");

        bool                     ok    = false;
        const unsigned long long value = text.trimmed().toULongLong(&ok);
        if (!ok || value == 0 || !node.set_extent(*project_, static_cast<std::size_t>(value)))
        {
            emit statusChanged(tr("%1: expected a whole number of at least 1.").arg(noun), true);
            return false;
        }

        // The number sizes the node, so everything behind the node moves with
        // it and the class re-flattens the way a kind change makes it.
        rebuild();
        emit statusChanged(tr("%1 set to %2.").arg(noun).arg(value), false);
        return true;
    }

    bool ReclassStructureModel::setData(const QModelIndex& index, const QVariant& value, int role)
    {
        const ReclassRow* row = row_for(index);
        if (role != Qt::EditRole || row == nullptr)
        {
            return false;
        }
        if (index.column() == ReclassStructureModel::name)
        {
            return set_node_name(*row, value.toString());
        }
        if (index.column() == ReclassStructureModel::comment)
        {
            return set_node_comment(*row, value.toString());
        }
        return false;
    }

    reclass::Project& ReclassStructureModel::project() noexcept
    {
        return *project_;
    }

    std::size_t ReclassStructureModel::selected_class() const noexcept
    {
        return selected_class_;
    }

    bool ReclassStructureModel::valid_class() const noexcept
    {
        return valid_class_;
    }

    bool ReclassStructureModel::is_expanded(std::size_t node_id) const
    {
        return expanded_.contains(node_id);
    }

    bool ReclassStructureModel::expandable(const ReclassRow& row) const
    {
        // A hidden row is the collapsed `...` placeholder: it inlines nothing, so
        // it carries no expander.
        if (row.hidden || is_marked(row) || project_ == nullptr)
        {
            return false;
        }
        if (row.node == nullptr)
        {
            return false;
        }
        return reclass::is_class_kind(row.node->kind()) || row.node->is_container();
    }

    const std::set<std::size_t>& ReclassStructureModel::expanded_nodes() const noexcept
    {
        return expanded_;
    }

    bool ReclassStructureModel::is_element_row(const ReclassRow& row) const
    {
        // The row an expanded array built for the element its index names: an
        // array's own child, not a field a class array inlines.
        return row.parent != nullptr && row.parent->node != nullptr
            && row.parent->node->kind() == reclass::NodeKind::array;
    }

    std::size_t ReclassStructureModel::element_count(const reclass::Node& node) const
    {
        switch (node.kind())
        {
        case reclass::NodeKind::array:
        {
            const auto* container = dynamic_cast<const reclass::ContainerNode*>(&node);
            return container == nullptr ? 0 : container->children_mutable().size();
        }
        case reclass::NodeKind::class_instance_array:
        case reclass::NodeKind::class_wrapper_array:
            return node.extent().value_or(0);
        default:
            return 0;
        }
    }

    std::size_t ReclassStructureModel::element_count(std::size_t node_id) const
    {
        const ReclassRow* row = find_row(roots_, node_id);
        return row == nullptr || row->node == nullptr ? 0 : element_count(*row->node);
    }

    std::size_t ReclassStructureModel::stored_element_index(std::size_t node_id) const
    {
        const auto found = element_indices_.find(node_id);
        return found == element_indices_.end() ? 0 : found->second;
    }

    std::optional<std::size_t> ReclassStructureModel::shown_element(const ReclassRow&    row,
                                                                    const reclass::Node& node) const
    {
        // The collapsed `...` placeholder and a marked reference row draw no
        // control, exactly as they bracketed no number before.
        if (row.hidden || is_marked(row))
        {
            return std::nullopt;
        }
        const std::size_t count = element_count(node);
        if (count == 0)
        {
            return std::nullopt;
        }
        // An index past the end - a count the user shrank - shows the last
        // element rather than nothing.
        return std::min(stored_element_index(row.node_id), count - 1);
    }

    std::size_t ReclassStructureModel::element_index(std::size_t node_id) const
    {
        const ReclassRow* row = find_row(roots_, node_id);
        if (row == nullptr || row->node == nullptr)
        {
            return 0;
        }
        return shown_element(*row, *row->node).value_or(0);
    }

    bool ReclassStructureModel::set_element_index(std::size_t node_id, std::size_t index)
    {
        const ReclassRow* row = find_row(roots_, node_id);
        if (row == nullptr || row->node == nullptr)
        {
            return false;
        }
        const std::size_t count = element_count(*row->node);
        if (count == 0 || shown_element(*row, *row->node) == std::nullopt)
        {
            return false;
        }
        const std::size_t wrapped = index % count;
        if (stored_element_index(node_id) == wrapped)
        {
            return true;
        }
        element_indices_[node_id] = wrapped;

        // The row shows another element, so the class is flattened again; the
        // grid re-flattens, re-measures and repaints on the reset.
        rebuild();
        return true;
    }

    bool ReclassStructureModel::set_element_index(const ReclassRow& row, const QString& text)
    {
        if (row.node == nullptr)
        {
            return false;
        }
        const std::size_t count = element_count(*row.node);
        if (count == 0)
        {
            return false;
        }

        bool                     ok    = false;
        const unsigned long long typed = text.trimmed().toULongLong(&ok);
        if (!ok)
        {
            emit statusChanged(tr("Index: expected a whole number."), true);
            return false;
        }

        // An index past either end wraps, so the element the user names is the
        // one that shows: 0 ... count - 1.
        const std::size_t wrapped     = static_cast<std::size_t>(typed % count);
        const bool        moved       = stored_element_index(row.node_id) != wrapped;
        element_indices_[row.node_id] = wrapped;
        if (moved)
        {
            // The row shows another element, so the class is flattened again.
            rebuild();
        }
        emit statusChanged(tr("Element %1 of %2 shown.").arg(wrapped + 1).arg(count), false);
        return true;
    }

    bool ReclassStructureModel::step_element(std::size_t node_id, int delta)
    {
        const ReclassRow* row = find_row(roots_, node_id);
        if (row == nullptr || row->node == nullptr)
        {
            return false;
        }
        const std::size_t count = element_count(*row->node);
        if (count <= 1)
        {
            return false;
        }
        const auto      steps   = static_cast<long long>(count);
        const auto      current = static_cast<long long>(element_index(node_id));
        const long long moved   = ((current + delta) % steps + steps) % steps;
        return set_element_index(node_id, static_cast<std::size_t>(moved));
    }

    ElementRuns ReclassStructureModel::element_runs(std::size_t node_id) const
    {
        const ReclassRow* row = find_row(roots_, node_id);
        if (row == nullptr || row->node == nullptr)
        {
            return {};
        }
        const std::optional<std::size_t> shown = shown_element(*row, *row->node);
        return shown.has_value() ? runs_of(kind_text(*row, *row->node), *shown) : ElementRuns {};
    }

    const std::vector<ReclassBlock>& ReclassStructureModel::blocks() const noexcept
    {
        return blocks_;
    }

    const ReclassRow* ReclassStructureModel::row_for(const QModelIndex& index) const
    {
        return index.isValid() ? static_cast<const ReclassRow*>(index.internalPointer()) : nullptr;
    }

    const ReclassRow* ReclassStructureModel::row_at(const QModelIndex& parent, std::size_t row) const
    {
        if (!parent.isValid())
        {
            return row < roots_.size() ? roots_[row].get() : nullptr;
        }
        const ReclassRow* parent_row = row_for(parent);
        if (parent_row == nullptr)
        {
            return nullptr;
        }
        return row < parent_row->children.size() ? parent_row->children[row].get() : nullptr;
    }

    QModelIndex ReclassStructureModel::index_for_node(std::size_t node_id) const
    {
        const ReclassRow* row = find_row(roots_, node_id);
        if (row == nullptr)
        {
            return {};
        }
        return createIndex(static_cast<int>(row->position), 0, const_cast<ReclassRow*>(row));
    }

    void ReclassStructureModel::build_rows()
    {
        roots_.clear();
        blocks_.clear();

        const reclass::Class* type = valid_class_ ? project_->class_at(selected_class_) : nullptr;
        if (type == nullptr)
        {
            valid_class_ = false;
            return;
        }

        const auto layout = reclass::layout_class(*project_, *type);

        ReclassBlock root;
        root.index       = 0;
        root.class_index = selected_class_;
        root.size        = layout.has_value() ? layout->size : 0;
        blocks_.push_back(root);

        // The chain starts empty: a reference at the top level may inline its
        // class once, and only a reference that meets itself further down the
        // chain is a cycle.
        std::vector<std::size_t> ancestors;
        build_class(*type, selected_class_, 0, 0, ancestors, nullptr);
    }

    void ReclassStructureModel::build_container(const reclass::Node& node,
                                                std::size_t          class_index,
                                                std::size_t          block,
                                                std::size_t          base_offset,
                                                ReclassRow*          parent)
    {
        const auto* container = dynamic_cast<const reclass::ContainerNode*>(&node);
        if (container == nullptr)
        {
            return;
        }

        const bool                       shared = node.kind() == reclass::NodeKind::union_value;
        // An array shows one element at a time - the one the index names - so a
        // long buffer never draws a row per element; a union keeps every member.
        const std::optional<std::size_t> only =
            node.kind() == reclass::NodeKind::array && parent != nullptr ? shown_element(*parent, node) : std::nullopt;

        std::size_t offset = 0;
        for (std::size_t index = 0; index < container->children_mutable().size(); ++index)
        {
            const reclass::Node& child = *container->children_mutable()[index];

            if (!only.has_value() || *only == index)
            {
                auto row         = std::make_unique<ReclassRow>();
                row->node        = &child;
                row->node_id     = child.id;
                row->class_index = class_index;
                row->offset      = base_offset + offset;
                row->block       = block;
                row->hidden      = child.hidden;

                ReclassRow* created = append_row(parent, std::move(row));

                if (!child.hidden && child.is_container() && is_expanded(child.id))
                {
                    build_container(child, class_index, block, created->offset, created);
                }
            }

            if (!shared)
            {
                offset += reclass::node_size(*project_, child);
            }
        }
    }

    void ReclassStructureModel::build_class(const reclass::Class&     type,
                                            std::size_t               class_index,
                                            std::size_t               block,
                                            std::size_t               base_offset,
                                            std::vector<std::size_t>& ancestors,
                                            ReclassRow*               parent)
    {
        std::size_t offset = 0;
        for (std::size_t node_index = 0; node_index < type.nodes.size(); ++node_index)
        {
            const reclass::Node& node = *type.nodes[node_index];

            auto row         = std::make_unique<ReclassRow>();
            row->node        = &node;
            row->node_id     = node.id;
            row->class_index = class_index;
            row->node_index  = node_index;
            row->offset      = base_offset + offset;
            row->block       = block;
            row->hidden      = node.hidden;

            ReclassRow* created = append_row(parent, std::move(row));

            if (!node.hidden && reclass::is_class_kind(node.kind()))
            {
                const reclass::Class* target       = project_->find_class(node.class_name);
                const std::size_t     target_index = target == nullptr ? 0 : class_index_of(target);
                if (target == nullptr)
                {
                    created->unknown_class = true;
                }
                else if (std::ranges::find(ancestors, target_index) != ancestors.end())
                {
                    created->cycle = true;
                }
                else if (ancestors.size() >= kMaxReclassBlockDepth)
                {
                    created->too_deep = true;
                }
                else if (is_expanded(node.id))
                {
                    const auto child_layout = reclass::layout_class(*project_, *target);

                    const reclass::NodeKind kind   = node.kind();
                    const bool              array  = kind == reclass::NodeKind::class_wrapper_array
                                                  || kind == reclass::NodeKind::class_instance_array;
                    const std::size_t       stride = child_layout.has_value() ? child_layout->size : 0;

                    // Which element the class array shows, as a distance into
                    // the class's own bytes. A class with no size has no stride
                    // to shift by, so its element stays where the class starts.
                    std::size_t shift = 0;
                    if (array && stride != 0)
                    {
                        const std::size_t count = element_count(node);
                        if (count != 0)
                        {
                            shift = std::min(stored_element_index(node.id), count - 1) * stride;
                        }
                    }

                    ReclassBlock child;
                    child.index        = blocks_.size();
                    child.class_index  = target_index;
                    child.parent_block = block;
                    // A wrapper array's element lives inside the parent's own
                    // bytes, so the block itself starts at the element's stride;
                    // an instance array's element lives past the pointer's
                    // target, where the value model adds the same stride after
                    // the dereference.
                    child.base_offset =
                        kind == reclass::NodeKind::class_wrapper_array ? created->offset + shift : created->offset;
                    child.size           = stride;
                    child.element_offset = kind == reclass::NodeKind::class_instance_array ? shift : 0;
                    child.via_pointer =
                        kind == reclass::NodeKind::class_pointer || kind == reclass::NodeKind::class_instance_array;
                    blocks_.push_back(child);

                    ancestors.push_back(target_index);
                    build_class(*target, target_index, child.index, child.base_offset, ancestors, created);
                    ancestors.pop_back();
                }
            }
            else if (!node.hidden && node.is_container() && is_expanded(node.id))
            {
                build_container(node, class_index, block, created->offset, created);
            }

            offset += reclass::node_size(*project_, node);
        }
    }

    ReclassRow* ReclassStructureModel::append_row(ReclassRow* parent, std::unique_ptr<ReclassRow> row)
    {
        row->parent = parent;
        if (parent == nullptr)
        {
            row->position = roots_.size();
            roots_.push_back(std::move(row));
            return roots_.back().get();
        }

        row->position = parent->children.size();
        parent->children.push_back(std::move(row));
        return parent->children.back().get();
    }

    std::size_t ReclassStructureModel::class_index_of(const reclass::Class* type) const
    {
        const std::vector<reclass::Class>& classes = project_->classes();
        for (std::size_t index = 0; index < classes.size(); ++index)
        {
            if (&classes[index] == type)
            {
                return index;
            }
        }
        return classes.size();
    }

    QString ReclassStructureModel::kind_text(const ReclassRow& row, const reclass::Node& node) const
    {
        if (row.unknown_class)
        {
            return tr("unknown class");
        }
        if (row.cycle)
        {
            return tr("cycle");
        }
        if (row.too_deep)
        {
            return tr("too deep");
        }
        if (node.kind() == reclass::NodeKind::class_instance)
        {
            return tr("Instance <%1>").arg(to_qstring(node.class_name));
        }
        if (node.kind() == reclass::NodeKind::class_pointer)
        {
            return tr("Pointer <%1>").arg(to_qstring(node.class_name));
        }

        QString label = to_qstring(reclass::describe(node.kind()));
        if (node.kind() == reclass::NodeKind::array)
        {
            // The element the row shows names its own kind inside the angle
            // brackets, so a long buffer spells out what one of its rows holds.
            if (const std::optional<std::size_t> shown = shown_element(row, node))
            {
                const QString element = element_kind(node, *shown);
                if (!element.isEmpty())
                {
                    label += QStringLiteral("<%1>").arg(element);
                }
            }
        }
        if (!node.class_name.empty())
        {
            label += QStringLiteral(" <%1>").arg(to_qstring(node.class_name));
        }
        const std::string_view extra = node.extra_text();
        if (!extra.empty())
        {
            label += QStringLiteral(" ") + to_qstring(extra);
        }
        return label;
    }

    QString ReclassStructureModel::type_text(const ReclassRow& row, const reclass::Node& node) const
    {
        QString label = kind_text(row, node);
        if (const std::optional<std::size_t> shown = shown_element(row, node))
        {
            label += QStringLiteral(" ") + index_control(*shown);
        }
        return label;
    }

    QModelIndex ReclassStructureModel::index(int row, int column, const QModelIndex& parent) const
    {
        if (column < 0 || column >= column_count || row < 0)
        {
            return {};
        }
        const ReclassRow* created = row_at(parent, static_cast<std::size_t>(row));
        if (created == nullptr)
        {
            return {};
        }
        return createIndex(row, column, const_cast<ReclassRow*>(created));
    }

    QModelIndex ReclassStructureModel::parent(const QModelIndex& index) const
    {
        const ReclassRow* row = row_for(index);
        if (row == nullptr || row->parent == nullptr)
        {
            return {};
        }
        return createIndex(static_cast<int>(row->parent->position), 0, row->parent);
    }

    int ReclassStructureModel::rowCount(const QModelIndex& parent) const
    {
        if (parent.isValid() && parent.column() > 0)
        {
            return 0;
        }
        if (!parent.isValid())
        {
            return static_cast<int>(roots_.size());
        }
        const ReclassRow* row = row_for(parent);
        return row == nullptr ? 0 : static_cast<int>(row->children.size());
    }

    int ReclassStructureModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() && parent.column() > 0 ? 0 : column_count;
    }

    QVariant ReclassStructureModel::data(const QModelIndex& index, int role) const
    {
        const ReclassRow* row = row_for(index);
        if (row == nullptr || project_ == nullptr)
        {
            return {};
        }
        if (row->node == nullptr)
        {
            return {};
        }
        const reclass::Node& node = *row->node;

        switch (role)
        {
        case Qt::DisplayRole:
            switch (index.column())
            {
            case offset:
                return QString::fromStdString(std::format("{:X}", row->offset));
            case type:
                // A hex node's own width is what its byte count shows, so the
                // kind cell stays empty for it.
                return reclass::is_hex_kind(node.kind()) ? QVariant {} : type_text(*row, node);
            case name:
                // An array's element is identified by its place in the array,
                // so its row draws no name even when the node carries one.
                return is_element_row(*row) ? QVariant {} : to_qstring(node.name);
            case comment:
                return comment_text(node.comment);
            default:
                // The address, ascii, value, hints and pointer columns are
                // answered by the value model.
                return {};
            }
        case Qt::EditRole:
            // The name, the note and the Type cell's bracketed number are typed
            // into, so the editor starts from the same text the cell shows; the
            // run alone prefills its bare number, without the brackets.
            if (index.column() == name && !is_element_row(*row))
            {
                return to_qstring(node.name);
            }
            if (index.column() == comment)
            {
                return to_qstring(node.comment);
            }
            if (index.column() == type)
            {
                if (const std::optional<std::size_t> extent = run_extent(*row, node))
                {
                    return QString::number(*extent);
                }
            }
            return {};
        case Qt::FontRole:
            if (index.column() == address || index.column() == offset || index.column() == ascii
                || index.column() == value)
            {
                return QVariant::fromValue(mono_font());
            }
            return {};
        case Qt::ForegroundRole:
            // A marked reference row is muted whole; every other row spells its
            // kind, its offset and its note in the theme's own roles.
            if (is_marked(*row))
            {
                return active_theme().text_muted;
            }
            switch (index.column())
            {
            case offset:
                return active_theme().grid_offset;
            case comment:
                return active_theme().grid_comment;
            case type:
                return datatype_colour(node.kind());
            default:
                return {};
            }
        case widgets::kFullTextRole:
            switch (index.column())
            {
            case type:
                return type_text(*row, node);
            case name:
                return is_element_row(*row) ? QVariant {} : to_qstring(node.name);
            case comment:
                // The tooltip carries the note alone: the marker is the grid's
                // own decoration.
                return to_qstring(node.comment);
            default:
                return {};
            }
        case kExtentTextRole:
            // The run the cell already draws, so the grid can anchor its inline
            // editor on it without parsing the drawn text back apart.
            if (index.column() == type)
            {
                if (run_extent(*row, node).has_value())
                {
                    return to_qstring(node.extra_text());
                }
            }
            return {};
        default:
            return {};
        }
    }

    QVariant ReclassStructureModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        {
            return {};
        }
        switch (section)
        {
        case address:
            return tr("Address");
        case offset:
            return tr("Offset");
        case ascii:
            return tr("Ascii");
        case type:
            return tr("Type");
        case name:
            return tr("Name");
        case value:
            return tr("Value");
        case comment:
            return tr("Comment");
        case hints:
            return tr("Hints");
        case pointer:
            return tr("Pointer");
        default:
            return {};
        }
    }

    Qt::ItemFlags ReclassStructureModel::flags(const QModelIndex& index) const
    {
        if (!index.isValid())
        {
            return Qt::NoItemFlags;
        }
        Qt::ItemFlags     result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        const ReclassRow* row    = row_for(index);
        if (index.column() == name && (row == nullptr || !is_element_row(*row)))
        {
            // A node's name is the user's own label and costs no target access,
            // so it is typed straight into the cell; an array's element takes no
            // name, so its cell stays read-only.
            result |= Qt::ItemIsEditable;
        }
        if (index.column() == comment && row != nullptr && !row->hidden)
        {
            // A note costs no target access either, but the collapsed `...`
            // placeholder of a hidden node takes no edit.
            result |= Qt::ItemIsEditable;
        }
        return result;
    }

}
