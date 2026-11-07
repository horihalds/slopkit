#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <QString>
#include <QTreeWidget>

#include "reclass/project.hpp"

class QContextMenuEvent;
class QTreeWidgetItem;
class QWidget;

namespace slopkit::ui::components
{
    // The reference's project pane: the classes and the enums as two top-level
    // groups. The class-hierarchy view additionally nests the classes a class
    // points at under it, the way `ClassTreeNode.RebuildClassHierarchy` does,
    // with a guard so a cycle still renders.
    class ProjectView : public QTreeWidget
    {
        Q_OBJECT

    public:
        // The payload each item carries: what it stands for and its index in the
        // project's classes or enums.
        enum ItemData
        {
            item_role = Qt::UserRole,
            item_index,
        };

        enum class ItemRole
        {
            group,
            klass,
            enum_kind,
        };

        explicit ProjectView(QWidget* parent = nullptr);

        void set_project(reclass::Project& project);

        // Rebuilds the tree from the project, restoring the selected class when
        // it still exists. Never emits classSelected, so the caller drives the
        // selection itself.
        void refresh();

        void               set_hierarchy_view(bool enabled);
        [[nodiscard]] bool hierarchy_view() const noexcept;

        void expand_all();
        void collapse_all();

        [[nodiscard]] std::size_t      class_count() const;
        [[nodiscard]] QString          class_name(std::size_t index) const;
        [[nodiscard]] QTreeWidgetItem* class_item(std::size_t index) const noexcept;
        [[nodiscard]] QTreeWidgetItem* enum_item(std::size_t index) const noexcept;
        void                           set_class_tooltip(std::size_t index, const QString& tooltip);

        [[nodiscard]] std::optional<std::size_t> selected_class() const;
        [[nodiscard]] std::optional<std::size_t> selected_enum() const;
        void                                     select_class(std::size_t index);

    signals:
        void classSelected(std::size_t index);
        void newClassRequested();
        void deleteClassRequested();
        void removeUnusedClassesRequested();
        void renameClassRequested();
        void setClassAddressRequested();
        void newEnumRequested();
        void editEnumRequested(std::size_t index);
        void deleteEnumRequested(std::size_t index);

    protected:
        void contextMenuEvent(QContextMenuEvent* event) override;

    private:
        void             on_current_item_changed(QTreeWidgetItem* current);
        QTreeWidgetItem* add_class_item(QTreeWidgetItem* parent, std::size_t index);
        void populate_hierarchy_children(QTreeWidgetItem* parent, std::size_t index, std::vector<std::size_t>& seen);
        [[nodiscard]] std::vector<std::size_t> referenced_classes(std::size_t index) const;

        reclass::Project*          project_ {};
        bool                       hierarchy_view_ {false};
        std::optional<std::size_t> selected_;

        // The first item of each class and each enum, so a tooltip can be set by
        // index even when the hierarchy view draws a class twice.
        std::vector<QTreeWidgetItem*> class_items_;
        std::vector<QTreeWidgetItem*> enum_items_;
    };
}
