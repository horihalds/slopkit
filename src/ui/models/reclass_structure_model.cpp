#include "ui/models/reclass_structure_model.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "reclass/edit.hpp"
#include "reclass/node.hpp"
#include "reclass/nodes/container.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
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
        rebuild();
    }

    void ReclassStructureModel::set_class(std::size_t index)
    {
        if (index != selected_class_)
        {
            expanded_.clear();
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
        beginResetModel();
        build_rows();
        endResetModel();
        emit blocksChanged();
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

    bool ReclassStructureModel::setData(const QModelIndex& index, const QVariant& value, int role)
    {
        const ReclassRow* row = row_for(index);
        if (role != Qt::EditRole || index.column() != ReclassStructureModel::name || row == nullptr)
        {
            return false;
        }
        return set_node_name(*row, value.toString());
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
        if (is_marked(row) || project_ == nullptr)
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

        const bool  shared = node.kind() == reclass::NodeKind::union_value;
        std::size_t offset = 0;
        for (std::size_t index = 0; index < container->children_mutable().size(); ++index)
        {
            const reclass::Node& child = *container->children_mutable()[index];

            auto row         = std::make_unique<ReclassRow>();
            row->node        = &child;
            row->node_id     = child.id;
            row->class_index = class_index;
            row->offset      = base_offset + offset;
            row->block       = block;

            ReclassRow* created = append_row(parent, std::move(row));

            if (child.is_container() && is_expanded(child.id))
            {
                build_container(child, class_index, block, created->offset, created);
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

            ReclassRow* created = append_row(parent, std::move(row));

            if (reclass::is_class_kind(node.kind()))
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

                    ReclassBlock child;
                    child.index        = blocks_.size();
                    child.class_index  = target_index;
                    child.parent_block = block;
                    child.base_offset  = created->offset;
                    child.size         = child_layout.has_value() ? child_layout->size : 0;
                    child.via_pointer  = node.kind() == reclass::NodeKind::class_pointer;
                    blocks_.push_back(child);

                    ancestors.push_back(target_index);
                    build_class(*target, target_index, child.index, created->offset, ancestors, created);
                    ancestors.pop_back();
                }
            }
            else if (node.is_container() && is_expanded(node.id))
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

    QString ReclassStructureModel::type_text(const ReclassRow& row, const reclass::Node& node) const
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
                return type_text(*row, node);
            case name:
                return to_qstring(node.name);
            case comment:
                return to_qstring(node.comment);
            default:
                // The address and value columns are answered by the value model.
                return {};
            }
        case Qt::EditRole:
            // The name cell is typed into, so the editor starts from the same
            // text the cell shows; no other cell takes an edit.
            return index.column() == name ? to_qstring(node.name) : QVariant {};
        case Qt::FontRole:
            if (index.column() == address || index.column() == offset || index.column() == value)
            {
                return QVariant::fromValue(mono_font());
            }
            return {};
        case Qt::ForegroundRole:
            if (is_marked(*row) || index.column() == comment || index.column() == offset)
            {
                return active_theme().text_muted;
            }
            if (index.column() == type)
            {
                return active_theme().syntax_keyword;
            }
            return {};
        case widgets::kFullTextRole:
            switch (index.column())
            {
            case type:
                return type_text(*row, node);
            case name:
                return to_qstring(node.name);
            case comment:
                return to_qstring(node.comment);
            default:
                return {};
            }
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
        case type:
            return tr("Type");
        case name:
            return tr("Name");
        case value:
            return tr("Value");
        case comment:
            return tr("Comment");
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
        Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (index.column() == name)
        {
            // A node's name is the user's own label and costs no target access,
            // so it is typed straight into the cell.
            result |= Qt::ItemIsEditable;
        }
        return result;
    }

}
