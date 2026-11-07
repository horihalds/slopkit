#include "ui/components/project_view.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <QContextMenuEvent>
#include <QMenu>
#include <QPoint>
#include <QSignalBlocker>
#include <QString>
#include <QTreeWidgetItem>

#include "reclass/nodes/node.hpp"
#include "ui/components/row_menu.hpp"

namespace slopkit::ui::components
{
    namespace
    {
        constexpr int kItemRole  = ProjectView::item_role;
        constexpr int kItemIndex = ProjectView::item_index;

        std::optional<std::size_t> index_of(const reclass::Project& project, const reclass::Class* type)
        {
            for (std::size_t index = 0; index < project.classes().size(); ++index)
            {
                if (&project.classes()[index] == type)
                {
                    return index;
                }
            }
            return std::nullopt;
        }
    }

    ProjectView::ProjectView(QWidget* parent) : QTreeWidget(parent)
    {
        setObjectName(QStringLiteral("reclass_project_view"));
        setColumnCount(1);
        setHeaderHidden(true);
        setContextMenuPolicy(Qt::DefaultContextMenu);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);

        connect(this,
                &QTreeWidget::currentItemChanged,
                this,
                [this](QTreeWidgetItem* current, QTreeWidgetItem*)
                {
                    on_current_item_changed(current);
                });
    }

    void ProjectView::set_project(reclass::Project& project)
    {
        project_ = &project;
        refresh();
    }

    std::size_t ProjectView::class_count() const
    {
        return project_ == nullptr ? 0 : project_->classes().size();
    }

    QString ProjectView::class_name(std::size_t index) const
    {
        const reclass::Class* type = project_ == nullptr ? nullptr : project_->class_at(index);
        return type == nullptr ? QString() : QString::fromStdString(type->name);
    }

    void ProjectView::set_class_tooltip(std::size_t index, const QString& tooltip)
    {
        if (index < class_items_.size() && class_items_[index] != nullptr)
        {
            class_items_[index]->setToolTip(0, tooltip);
        }
    }

    QTreeWidgetItem* ProjectView::class_item(std::size_t index) const noexcept
    {
        return index < class_items_.size() ? class_items_[index] : nullptr;
    }

    QTreeWidgetItem* ProjectView::enum_item(std::size_t index) const noexcept
    {
        return index < enum_items_.size() ? enum_items_[index] : nullptr;
    }

    void ProjectView::set_hierarchy_view(bool enabled)
    {
        if (hierarchy_view_ == enabled)
        {
            return;
        }
        hierarchy_view_ = enabled;
        refresh();
    }

    bool ProjectView::hierarchy_view() const noexcept
    {
        return hierarchy_view_;
    }

    void ProjectView::expand_all()
    {
        expandAll();
    }

    void ProjectView::collapse_all()
    {
        for (int position = 0; position < topLevelItemCount(); ++position)
        {
            QTreeWidgetItem* group = topLevelItem(position);
            group->setExpanded(true);
            for (int child = 0; child < group->childCount(); ++child)
            {
                group->child(child)->setExpanded(false);
            }
        }
    }

    void ProjectView::refresh()
    {
        const std::size_t count = class_count();
        if (count == 0)
        {
            selected_.reset();
        }
        else
        {
            selected_ = std::min(selected_.value_or(0), count - 1);
        }

        const QSignalBlocker blocker(this);
        clear();
        class_items_.assign(count, nullptr);
        enum_items_.assign(project_ == nullptr ? 0 : project_->enums().size(), nullptr);

        auto* classes_group = new QTreeWidgetItem(this);
        classes_group->setText(0, tr("Classes"));
        classes_group->setData(0, kItemRole, static_cast<int>(ItemRole::group));
        classes_group->setExpanded(true);

        for (std::size_t index = 0; index < count; ++index)
        {
            QTreeWidgetItem* item = add_class_item(classes_group, index);
            if (class_items_[index] == nullptr)
            {
                class_items_[index] = item;
            }
            if (hierarchy_view_)
            {
                std::vector<std::size_t> seen {index};
                populate_hierarchy_children(item, index, seen);
            }
        }

        auto* enums_group = new QTreeWidgetItem(this);
        enums_group->setText(0, tr("Enums"));
        enums_group->setData(0, kItemRole, static_cast<int>(ItemRole::group));
        enums_group->setExpanded(true);
        if (project_ != nullptr)
        {
            for (std::size_t index = 0; index < project_->enums().size(); ++index)
            {
                auto* item = new QTreeWidgetItem(enums_group);
                item->setText(0, QString::fromStdString(project_->enums()[index].name));
                item->setData(0, kItemRole, static_cast<int>(ItemRole::enum_kind));
                item->setData(0, kItemIndex, static_cast<qulonglong>(index));
                enum_items_[index] = item;
            }
        }

        if (selected_.has_value() && *selected_ < class_items_.size() && class_items_[*selected_] != nullptr)
        {
            setCurrentItem(class_items_[*selected_]);
        }
    }

    void ProjectView::select_class(std::size_t index)
    {
        // Selecting an item the tree already points at emits nothing, so a
        // rebuild that restores the same class cannot loop through the window's
        // classSelected handler.
        if (index < class_items_.size() && class_items_[index] != nullptr)
        {
            selected_ = index;
            setCurrentItem(class_items_[index]);
        }
    }

    std::optional<std::size_t> ProjectView::selected_class() const
    {
        return selected_;
    }

    std::optional<std::size_t> ProjectView::selected_enum() const
    {
        const QTreeWidgetItem* item = currentItem();
        if (item == nullptr || item->data(0, kItemRole).toInt() != static_cast<int>(ItemRole::enum_kind))
        {
            return std::nullopt;
        }
        return static_cast<std::size_t>(item->data(0, kItemIndex).toULongLong());
    }

    void ProjectView::on_current_item_changed(QTreeWidgetItem* current)
    {
        if (current == nullptr || current->data(0, kItemRole).toInt() != static_cast<int>(ItemRole::klass))
        {
            return;
        }
        selected_ = static_cast<std::size_t>(current->data(0, kItemIndex).toULongLong());
        emit classSelected(*selected_);
    }

    QTreeWidgetItem* ProjectView::add_class_item(QTreeWidgetItem* parent, std::size_t index)
    {
        auto* item = new QTreeWidgetItem(parent);
        item->setText(0, class_name(index));
        item->setData(0, kItemRole, static_cast<int>(ItemRole::klass));
        item->setData(0, kItemIndex, static_cast<qulonglong>(index));
        return item;
    }

    std::vector<std::size_t> ProjectView::referenced_classes(std::size_t index) const
    {
        std::vector<std::size_t> result;
        if (project_ == nullptr)
        {
            return result;
        }
        const reclass::Class* type = project_->class_at(index);
        if (type == nullptr)
        {
            return result;
        }
        for (const reclass::NodePtr& node : type->nodes)
        {
            if (node->class_name.empty())
            {
                continue;
            }
            const reclass::Class* target = project_->find_class(node->class_name);
            if (target == nullptr)
            {
                continue;
            }
            const std::optional<std::size_t> target_index = index_of(*project_, target);
            if (target_index.has_value() && std::ranges::find(result, *target_index) == result.end())
            {
                result.push_back(*target_index);
            }
        }
        return result;
    }

    void
    ProjectView::populate_hierarchy_children(QTreeWidgetItem* parent, std::size_t index, std::vector<std::size_t>& seen)
    {
        for (const std::size_t child : referenced_classes(index))
        {
            if (std::ranges::find(seen, child) != seen.end())
            {
                continue;
            }
            std::vector<std::size_t> child_seen = seen;
            child_seen.push_back(child);

            QTreeWidgetItem* item = add_class_item(parent, child);
            if (child < class_items_.size() && class_items_[child] == nullptr)
            {
                class_items_[child] = item;
            }
            item->setExpanded(true);
            populate_hierarchy_children(item, child, child_seen);
        }
    }

    void ProjectView::contextMenuEvent(QContextMenuEvent* event)
    {
        QTreeWidgetItem* item = itemAt(event->pos());
        const int        role = item == nullptr ? static_cast<int>(ItemRole::group) : item->data(0, kItemRole).toInt();

        QMenu menu(this);
        widgets::show_explanations(menu);
        constexpr int kEnum = static_cast<int>(ItemRole::enum_kind);
        if (role == kEnum)
        {
            QAction* edit = widgets::described_action(menu, tr("Edit Enum…"), tr("Change the enum's values."));
            connect(edit,
                    &QAction::triggered,
                    this,
                    [this]
                    {
                        if (const auto index = selected_enum(); index.has_value())
                        {
                            emit editEnumRequested(*index);
                        }
                    });

            QAction* remove =
                widgets::described_action(menu, tr("Delete Enum"), tr("Remove the enum from the project."));
            connect(remove,
                    &QAction::triggered,
                    this,
                    [this]
                    {
                        if (const auto index = selected_enum(); index.has_value())
                        {
                            emit deleteEnumRequested(*index);
                        }
                    });

            menu.addSeparator();
            QAction* add = widgets::described_action(menu, tr("Add Enum…"), tr("Add another enum."));
            connect(add,
                    &QAction::triggered,
                    this,
                    [this]
                    {
                        emit newEnumRequested();
                    });
        }
        else
        {
            QAction* add = widgets::described_action(menu, tr("Add Class…"), tr("Add a class to the project."));
            connect(add,
                    &QAction::triggered,
                    this,
                    [this]
                    {
                        emit newClassRequested();
                    });

            QAction* rename =
                widgets::described_action(menu, tr("Rename Class…"), tr("Change the selected class's name."));
            rename->setEnabled(selected_class().has_value());
            connect(rename,
                    &QAction::triggered,
                    this,
                    [this]
                    {
                        emit renameClassRequested();
                    });

            QAction* address =
                widgets::described_action(menu, tr("Set Class Address…"), tr("Change the class's expression."));
            address->setEnabled(selected_class().has_value());
            connect(address,
                    &QAction::triggered,
                    this,
                    [this]
                    {
                        emit setClassAddressRequested();
                    });

            QAction* remove = widgets::described_action(menu, tr("Delete Class"), tr("Remove the selected class."));
            remove->setEnabled(selected_class().has_value());
            connect(remove,
                    &QAction::triggered,
                    this,
                    [this]
                    {
                        emit deleteClassRequested();
                    });

            QAction* unused = widgets::described_action(
                menu, tr("Remove Unused Classes"), tr("Remove every class nothing refers to."));
            connect(unused,
                    &QAction::triggered,
                    this,
                    [this]
                    {
                        emit removeUnusedClassesRequested();
                    });

            menu.addSeparator();
            QAction* hierarchy = widgets::described_action(
                menu, tr("Class Hierarchy View"), tr("Nest each class's references under it."));
            hierarchy->setCheckable(true);
            hierarchy->setChecked(hierarchy_view_);
            connect(hierarchy, &QAction::toggled, this, &ProjectView::set_hierarchy_view);
        }

        menu.exec(event->globalPos());
    }
}
