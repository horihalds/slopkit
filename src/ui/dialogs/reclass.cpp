#include "ui/dialogs/reclass.hpp"
#include "ui/dialogs/enums.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "expr/expression.hpp"
#include "reclass/dissect.hpp"
#include "reclass/edit.hpp"
#include "reclass/node_menu.hpp"
#include "reclass/nodes/container.hpp"
#include "ui/components/input_box.hpp"
#include "ui/components/project_view.hpp"
#include "ui/components/reclass_view.hpp"
#include "ui/components/row_menu.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"
#include "ui/models/reclass_proxy_model.hpp"
#include "ui/models/reclass_structure_model.hpp"
#include "ui/models/reclass_value_model.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::dialogs
{
    namespace
    {
        QString kind_label(reclass::NodeKind kind)
        {
            return to_qstring(reclass::describe(kind));
        }
    }

    ReClassDialog::ReClassDialog(reclass::Project&        project,
                                 process::AccessWorker&   worker,
                                 process::AttachedTarget& target,
                                 QWidget*                 parent)
        : QDialog(parent), project_(project), worker_(&worker), target_(&target)
    {
        setWindowTitle(tr("ReClass"));
        setWindowFlag(Qt::Window, true);
        resize(880, 560);
        // A companion window: it must not own the process lifetime, or closing
        // the shell would leave slopkit running.
        setAttribute(Qt::WA_QuitOnClose, false);

        structure_ = new models::ReclassStructureModel(project_, this);
        values_    = new models::ReclassValueModel(*worker_, *target_, this);
        values_->set_structure(*structure_);
        proxy_ = new models::ReclassProxyModel(*structure_, *values_, this);

        build_layout();
        connect_signals();
        refresh_all();
    }

    void ReClassDialog::build_layout()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        menu_bar_ = new QMenuBar(this);
        menu_bar_->setObjectName(QStringLiteral("reclass_menu_bar"));
        build_menu_bar();
        layout->addWidget(menu_bar_);

        tool_bar_ = new QToolBar(this);
        tool_bar_->setObjectName(QStringLiteral("reclass_toolbar"));
        tool_bar_->setToolButtonStyle(Qt::ToolButtonTextOnly);
        build_tool_bar();
        layout->addWidget(tool_bar_);

        auto* splitter = new QSplitter(Qt::Horizontal, this);
        project_view_  = new components::ProjectView(splitter);
        project_view_->set_project(project_);
        splitter->addWidget(project_view_);
        splitter->addWidget(build_grid_pane());
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes({210, 660});
        layout->addWidget(splitter, 1);

        status_bar_ = new QStatusBar(this);
        status_bar_->setObjectName(QStringLiteral("reclass_status_bar"));
        build_status_bar();
        layout->addWidget(status_bar_);
    }

    void ReClassDialog::build_menu_bar()
    {
        QMenu* view = menu_bar_->addMenu(tr("View"));
        widgets::show_explanations(*view);
        hierarchy_ =
            widgets::described_action(*view, tr("Class Hierarchy View"), tr("Nest each class's references under it."));
        hierarchy_->setCheckable(true);
        connect(hierarchy_, &QAction::toggled, this, &ReClassDialog::set_class_hierarchy_view);
        view->addSeparator();
        QAction* expand = widgets::described_action(*view, tr("Expand All"), tr("Expand every class in the tree."));
        connect(expand,
                &QAction::triggered,
                this,
                [this]
                {
                    project_view_->expand_all();
                });
        QAction* collapse =
            widgets::described_action(*view, tr("Collapse All"), tr("Collapse every class in the tree."));
        connect(collapse,
                &QAction::triggered,
                this,
                [this]
                {
                    project_view_->collapse_all();
                });

        QMenu* node = menu_bar_->addMenu(tr("Node"));
        widgets::show_explanations(*node);
        const auto command =
            [this, node](const QString& text, const QString& description, void (ReClassDialog::*slot)())
        {
            QAction* action = widgets::described_action(*node, text, description);
            connect(action, &QAction::triggered, this, slot);
            return action;
        };
        command(tr("Create Class from Nodes"),
                tr("Move the selected nodes into a class of their own."),
                &ReClassDialog::create_class_from_nodes);
        command(
            tr("Shrink Class"), tr("Drop the padding nodes at the end of the class."), &ReClassDialog::shrink_class);
        command(tr("Copy Address"), tr("Copy the selected node's address."), &ReClassDialog::copy_address);
        node->addSeparator();
        command(tr("Copy Nodes"), tr("Copy the selected nodes."), &ReClassDialog::copy_selected_nodes);
        command(tr("Paste Nodes"), tr("Paste the copied nodes into the selected class."), &ReClassDialog::paste_nodes);
        node->addSeparator();
        command(tr("Hide Nodes"),
                tr("Collapse the selected nodes to a placeholder row."),
                &ReClassDialog::hide_selected_nodes);
        command(tr("Unhide Child Nodes"),
                tr("Unhide the hidden children of the selection."),
                &ReClassDialog::unhide_child_nodes);
        command(tr("Unhide Nodes Above"),
                tr("Unhide the hidden nodes above the selection."),
                &ReClassDialog::unhide_above_nodes);
        command(tr("Unhide Nodes Below"),
                tr("Unhide the hidden nodes below the selection."),
                &ReClassDialog::unhide_below_nodes);
        command(tr("Unhide Nodes"), tr("Unhide the selected nodes."), &ReClassDialog::unhide_selected_nodes);
        node->addSeparator();
        command(tr("Search for Equal Values"),
                tr("Search the target for the selected node's value."),
                &ReClassDialog::search_equal_values);
        QAction* writes = widgets::described_action(
            *node, tr("Find Out What Writes This Address"), tr("Arm the Access Watch on writes."));
        connect(writes,
                &QAction::triggered,
                this,
                [this]
                {
                    watch_address(true);
                });
        QAction* accesses = widgets::described_action(
            *node, tr("Find Out What Accesses This Address"), tr("Arm the Access Watch on reads and writes."));
        connect(accesses,
                &QAction::triggered,
                this,
                [this]
                {
                    watch_address(false);
                });

        QMenu* project = menu_bar_->addMenu(tr("Project"));
        widgets::show_explanations(*project);
        QAction* add_class = widgets::described_action(*project, tr("Add Class…"), tr("Add a class to the project."));
        connect(add_class, &QAction::triggered, this, &ReClassDialog::add_class);
        QAction* delete_class =
            widgets::described_action(*project, tr("Delete Class"), tr("Remove the selected class."));
        connect(delete_class, &QAction::triggered, this, &ReClassDialog::delete_class);
        QAction* unused = widgets::described_action(
            *project, tr("Remove Unused Classes"), tr("Remove every class nothing refers to."));
        connect(unused, &QAction::triggered, this, &ReClassDialog::remove_unused_classes);
        project->addSeparator();
        QAction* add_enum = widgets::described_action(*project, tr("Add Enum…"), tr("Add an enum to the project."));
        connect(add_enum, &QAction::triggered, this, &ReClassDialog::add_enum);
    }

    void ReClassDialog::build_tool_bar()
    {
        new_class_ = tool_bar_->addAction(tr("New Class"));
        new_class_->setObjectName(QStringLiteral("reclass_new_class"));
        connect(new_class_, &QAction::triggered, this, &ReClassDialog::add_class);

        auto add_bytes_button = new QToolButton(tool_bar_);
        add_bytes_button->setObjectName(QStringLiteral("reclass_add_bytes"));
        add_bytes_button->setText(tr("Add Bytes"));
        add_bytes_button->setPopupMode(QToolButton::InstantPopup);
        add_bytes_button->setMenu(build_add_bytes_menu(false));
        tool_bar_->addWidget(add_bytes_button);

        auto insert_bytes_button = new QToolButton(tool_bar_);
        insert_bytes_button->setObjectName(QStringLiteral("reclass_insert_bytes"));
        insert_bytes_button->setText(tr("Insert Bytes"));
        insert_bytes_button->setPopupMode(QToolButton::InstantPopup);
        insert_bytes_button->setMenu(build_add_bytes_menu(true));
        tool_bar_->addWidget(insert_bytes_button);

        tool_bar_->addSeparator();

        // The reference gives every node kind its own button here. slopkit's
        // table holds the reference's whole set, so they stay behind one button
        // carrying the same grouped menu instead of widening the strip past the
        // window.
        add_node_ = new QToolButton(tool_bar_);
        add_node_->setObjectName(QStringLiteral("reclass_add_node"));
        add_node_->setText(tr("Add Node"));
        add_node_->setPopupMode(QToolButton::InstantPopup);
        add_node_->setIcon(widgets::datatype_icon(reclass::NodeKind::array));
        // The strip is text-only; this one button carries its glyph beside its
        // label, so it overrides the toolbar's own style.
        add_node_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        QMenu* add_node_menu = build_add_node_menu();
        // The menu is built once, so its tints would freeze on a theme switch;
        // rebuilding it when it is about to show keeps the glyph colours live.
        connect(add_node_menu,
                &QMenu::aboutToShow,
                this,
                [this, add_node_menu]
                {
                    add_node_menu->clear();
                    populate_node_kind_menu(*add_node_menu, false);
                });
        add_node_->setMenu(add_node_menu);
        tool_bar_->addWidget(add_node_);

        tool_bar_->addSeparator();

        delete_node_ = tool_bar_->addAction(tr("Delete Nodes"));
        delete_node_->setObjectName(QStringLiteral("reclass_delete_node"));
        connect(delete_node_, &QAction::triggered, this, &ReClassDialog::remove_selected_nodes);

        dissect_ = tool_bar_->addAction(tr("Dissect"));
        dissect_->setObjectName(QStringLiteral("reclass_dissect"));
        connect(dissect_, &QAction::triggered, this, &ReClassDialog::dissect);

        follow_ = tool_bar_->addAction(tr("Follow Pointer"));
        follow_->setObjectName(QStringLiteral("reclass_follow"));
        connect(follow_, &QAction::triggered, this, &ReClassDialog::follow_pointer);

        tool_bar_->addSeparator();

        delete_class_ = tool_bar_->addAction(tr("Delete Class"));
        delete_class_->setObjectName(QStringLiteral("reclass_delete_class"));
        connect(delete_class_, &QAction::triggered, this, &ReClassDialog::delete_class);
    }

    void ReClassDialog::build_status_bar()
    {
        status_ = new widgets::StatusLabel(status_bar_);
        status_->setObjectName(QStringLiteral("reclass_status"));
        status_bar_->addWidget(status_, 1);

        process_label_ = new QLabel(status_bar_);
        process_label_->setObjectName(QStringLiteral("reclass_process"));
        process_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        status_bar_->addPermanentWidget(process_label_);

        close_ = widgets::secondary_button(tr("Close"), status_bar_);
        close_->setObjectName(QStringLiteral("reclass_close"));
        status_bar_->addPermanentWidget(close_);
    }

    QWidget* ReClassDialog::build_grid_pane()
    {
        auto* pane   = new QWidget(this);
        auto* layout = new QVBoxLayout(pane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        header_ = new QLabel(pane);
        header_->setObjectName(QStringLiteral("reclass_header"));
        header_->setFont(mono_font());
        header_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(header_);

        grid_ = new components::ReClassView(*proxy_, *structure_, *values_, pane);
        grid_->setObjectName(QStringLiteral("reclass_grid"));
        layout->addWidget(grid_, 1);

        return pane;
    }

    QMenu* ReClassDialog::build_add_node_menu()
    {
        auto* menu = new QMenu(this);
        populate_node_kind_menu(*menu, false);
        return menu;
    }

    QMenu* ReClassDialog::build_add_bytes_menu(bool insert)
    {
        auto* menu = new QMenu(this);
        populate_add_bytes_menu(*menu, insert);
        return menu;
    }

    void ReClassDialog::populate_add_bytes_menu(QMenu& menu, bool insert)
    {
        widgets::show_explanations(menu);

        // The reference's presets, 4 to 4096 bytes, then its "… Bytes" prompt.
        for (const std::size_t count : {4, 8, 64, 256, 1024, 2048, 4096})
        {
            const QString label  = insert ? tr("Insert %1 Bytes").arg(count) : tr("Add %1 Bytes").arg(count);
            const QString detail = insert ? tr("Insert %1 padding bytes in front of the selection.").arg(count)
                                          : tr("Append %1 padding bytes at the end of the container.").arg(count);
            QAction*      action = widgets::described_action(menu, label, detail);
            connect(action,
                    &QAction::triggered,
                    this,
                    [this, insert, count]
                    {
                        if (insert)
                        {
                            static_cast<void>(insert_bytes(count));
                        }
                        else
                        {
                            static_cast<void>(add_bytes(count));
                        }
                    });
        }

        menu.addSeparator();
        QAction* custom = widgets::described_action(
            menu, insert ? tr("Insert … Bytes") : tr("Add … Bytes"), tr("Choose the number of bytes yourself."));
        connect(custom,
                &QAction::triggered,
                this,
                [this, insert]
                {
                    if (insert)
                    {
                        insert_bytes();
                    }
                    else
                    {
                        add_bytes();
                    }
                });
    }

    void ReClassDialog::populate_node_kind_menu(QMenu& menu, bool change)
    {
        widgets::show_explanations(menu);

        // The reference's groups, in its order, with a separator between them.
        bool first = true;
        for (const reclass::NodeMenuGroup& group : reclass::node_menu_groups())
        {
            if (!first)
            {
                menu.addSeparator();
            }
            first = false;

            for (const reclass::NodeMenuEntry& entry : group.entries)
            {
                const QString label  = to_qstring(entry.label);
                const QString detail = change ? tr("Replace the selected node's type with %1.").arg(label)
                                              : tr("Insert a %1 node before the selection.").arg(label);
                QAction*      action = widgets::described_action(menu, label, detail);
                const QIcon   icon   = widgets::datatype_icon(entry.kind);
                if (!icon.isNull())
                {
                    action->setIcon(icon);
                }
                connect(action,
                        &QAction::triggered,
                        this,
                        [this, kind = entry.kind, change]
                        {
                            if (change)
                            {
                                change_node_kind(kind);
                            }
                            else
                            {
                                insert_node(kind);
                            }
                        });
            }
        }
    }

    void ReClassDialog::connect_signals()
    {
        connect(close_, &QPushButton::clicked, this, &QDialog::close);

        connect(project_view_, &components::ProjectView::classSelected, this, &ReClassDialog::on_class_selected);
        connect(project_view_, &components::ProjectView::newClassRequested, this, &ReClassDialog::add_class);
        connect(project_view_, &components::ProjectView::deleteClassRequested, this, &ReClassDialog::delete_class);
        connect(project_view_,
                &components::ProjectView::removeUnusedClassesRequested,
                this,
                &ReClassDialog::remove_unused_classes);
        connect(project_view_, &components::ProjectView::renameClassRequested, this, &ReClassDialog::rename_class);
        connect(
            project_view_, &components::ProjectView::setClassAddressRequested, this, &ReClassDialog::set_class_address);
        connect(project_view_, &components::ProjectView::newEnumRequested, this, &ReClassDialog::add_enum);
        connect(project_view_, &components::ProjectView::editEnumRequested, this, &ReClassDialog::edit_enum);
        connect(project_view_, &components::ProjectView::deleteEnumRequested, this, &ReClassDialog::delete_enum);

        connect(grid_,
                &components::ReClassView::rowMenuRequested,
                this,
                [this](const QPoint& position)
                {
                    show_row_menu(position);
                });

        connect(grid_,
                &components::ReClassView::hiddenRowActivated,
                this,
                [this](std::size_t node_id)
                {
                    const auto ref = reclass::node_ref(project_, node_id);
                    if (!ref.has_value())
                    {
                        return;
                    }
                    (*ref->owner)[ref->index]->hidden = false;
                    refresh_all();
                    report(tr("Node revealed."), false);
                });

        connect(grid_, &components::ReClassView::selectionChanged, this, &ReClassDialog::update_actions);

        connect(values_,
                &models::ReclassValueModel::valuesChanged,
                this,
                [this]
                {
                    proxy_->refresh_values();
                });
        connect(values_,
                &models::ReclassValueModel::addressesChanged,
                this,
                [this]
                {
                    refresh_header();
                    refresh_class_tooltips();
                });

        connect(values_,
                &models::ReclassValueModel::statusChanged,
                this,
                [this](const QString& message, bool error)
                {
                    report(message, error);
                });

        // A name is renamed by the structure model, so the cell and the menu
        // report through the same signal.
        connect(structure_,
                &models::ReclassStructureModel::statusChanged,
                this,
                [this](const QString& message, bool error)
                {
                    report(message, error);
                });

        delete_action_ = new QAction(tr("Delete Node"), this);
        delete_action_->setShortcut(QKeySequence(Qt::Key_Delete));
        delete_action_->setShortcutContext(Qt::WindowShortcut);
        addAction(delete_action_);
        connect(delete_action_, &QAction::triggered, this, &ReClassDialog::remove_selected_nodes);

        widgets::chain_tab_order({project_view_, grid_, close_});
    }

    models::ReclassStructureModel* ReClassDialog::structure() const noexcept
    {
        return structure_;
    }

    models::ReclassProxyModel* ReClassDialog::proxy() const noexcept
    {
        return proxy_;
    }

    components::ProjectView* ReClassDialog::project_view() const noexcept
    {
        return project_view_;
    }

    components::ReClassView* ReClassDialog::grid() const noexcept
    {
        return grid_;
    }

    QLabel* ReClassDialog::header_label() const noexcept
    {
        return header_;
    }

    widgets::StatusLabel* ReClassDialog::status_label() const noexcept
    {
        return status_;
    }

    QMenuBar* ReClassDialog::menu_bar() const noexcept
    {
        return menu_bar_;
    }

    QToolBar* ReClassDialog::tool_bar() const noexcept
    {
        return tool_bar_;
    }

    QStatusBar* ReClassDialog::status_bar() const noexcept
    {
        return status_bar_;
    }

    QAction* ReClassDialog::new_class_action() const noexcept
    {
        return new_class_;
    }

    QAction* ReClassDialog::delete_class_action() const noexcept
    {
        return delete_class_;
    }

    QToolButton* ReClassDialog::add_node_button() const noexcept
    {
        return add_node_;
    }

    QAction* ReClassDialog::delete_node_action() const noexcept
    {
        return delete_node_;
    }

    QAction* ReClassDialog::dissect_action() const noexcept
    {
        return dissect_;
    }

    QAction* ReClassDialog::follow_action() const noexcept
    {
        return follow_;
    }

    QAction* ReClassDialog::hierarchy_action() const noexcept
    {
        return hierarchy_;
    }

    std::vector<LiveRequest> ReClassDialog::next_live_request()
    {
        // Hidden, detached or with nothing to show the window submits nothing.
        if (!isVisible() || structure_ == nullptr || values_ == nullptr || !values_->attached())
        {
            return {};
        }
        values_->maybe_resolve_addresses();
        return values_->requests();
    }

    void ReClassDialog::apply_live_readings(std::span<const LiveReading> readings)
    {
        if (values_ == nullptr)
        {
            return;
        }
        // The applied readings announce themselves; repainting is the value
        // model's signal's job, and doing it here as well repainted every cell
        // twice per pass.
        values_->apply_readings(readings);
    }

    void ReClassDialog::refresh_project()
    {
        structure_->set_project(project_);
        project_view_->set_project(project_);
        refresh_all();
    }

    void ReClassDialog::set_modules(std::vector<process::ModuleInfo> modules, std::vector<process::RegionInfo> regions)
    {
        values_->set_modules(std::move(modules), std::move(regions));
    }

    void ReClassDialog::closeEvent(QCloseEvent* event)
    {
        status_->clear_status();
        QDialog::closeEvent(event);
    }

    std::optional<std::size_t> ReClassDialog::current_node_id() const
    {
        const models::ReclassRow* row = structure_->row_for(grid_->currentIndex());
        return row == nullptr ? std::nullopt : std::optional<std::size_t> {row->node_id};
    }

    std::vector<std::size_t> ReClassDialog::selected_node_ids() const
    {
        std::vector<std::size_t> ids;
        for (const models::ReclassRow* row : selected_rows())
        {
            ids.push_back(row->node_id);
        }
        return ids;
    }

    std::vector<const models::ReclassRow*> ReClassDialog::selected_rows() const
    {
        std::vector<const models::ReclassRow*> rows;
        for (const QModelIndex& index : grid_->selected_rows())
        {
            if (const models::ReclassRow* row = structure_->row_for(index); row != nullptr)
            {
                rows.push_back(row);
            }
        }
        return rows;
    }

    bool ReClassDialog::selected_class_exists() const
    {
        return structure_->valid_class() && project_.class_at(structure_->selected_class()) != nullptr;
    }

    QString ReClassDialog::validate_expression(const QString& text) const
    {
        if (text.trimmed().isEmpty())
        {
            return tr("An address expression is required.");
        }
        const auto parsed = expr::parse(text.toStdString());
        return parsed ? QString() : QString::fromStdString(parsed.error().message);
    }

    QString ReClassDialog::class_name_error(const QString& name, const reclass::Class* current) const
    {
        const QString trimmed = name.trimmed();
        if (trimmed.isEmpty())
        {
            return tr("A class name is required.");
        }
        const reclass::Class* existing = project_.find_class(trimmed.toStdString());
        if (existing != nullptr && existing != current)
        {
            return tr("A class named '%1' already exists.").arg(trimmed);
        }
        return QString();
    }

    void ReClassDialog::report(const QString& message, bool error)
    {
        status_->set_status(error ? widgets::StatusKind::error : widgets::StatusKind::info, message);
    }

    void ReClassDialog::refresh_all()
    {
        const std::optional<std::size_t> current = current_node_id();

        refresh_project_tree();
        values_->invalidate();

        if (current.has_value())
        {
            const QModelIndex index = structure_->index_for_node(*current);
            if (index.isValid())
            {
                grid_->setCurrentIndex(index);
                grid_->scrollTo(index);
            }
        }
        update_actions();
    }

    void ReClassDialog::refresh_project_tree()
    {
        const std::size_t size     = project_.classes().size();
        const std::size_t selected = size == 0 ? 0 : std::min(structure_->selected_class(), size - 1);

        structure_->set_class(selected);
        project_view_->refresh();
        project_view_->select_class(selected);
        refresh_header();
        refresh_class_tooltips();
    }

    void ReClassDialog::refresh_header()
    {
        const reclass::Class* type =
            selected_class_exists() ? project_.class_at(structure_->selected_class()) : nullptr;
        if (type == nullptr)
        {
            header_->setText(tr("No class"));
            return;
        }
        const QString                      expression = to_qstring(type->address_expression);
        const std::optional<std::uint64_t> address    = values_->class_address(structure_->selected_class());
        header_->setText(
            address.has_value()
                ? tr("%1 · %2 → %3").arg(to_qstring(type->name), expression, values_->short_address(*address))
                : tr("%1 · %2 → not resolved yet").arg(to_qstring(type->name), expression));
    }

    void ReClassDialog::refresh_class_tooltips()
    {
        for (std::size_t index = 0; index < project_.classes().size(); ++index)
        {
            const reclass::Class&              type       = project_.classes()[index];
            const QString                      expression = to_qstring(type.address_expression);
            const std::optional<std::uint64_t> address    = values_->class_address(index);
            project_view_->set_class_tooltip(index,
                                             address.has_value()
                                                 ? tr("%1 → %2").arg(expression, values_->short_address(*address))
                                                 : tr("%1 → not resolved yet").arg(expression));
        }
    }

    void ReClassDialog::update_actions()
    {
        const bool has_class = selected_class_exists();
        const bool has_node  = grid_->currentIndex().isValid();

        delete_class_->setEnabled(has_class);
        add_node_->setEnabled(has_class);
        delete_node_->setEnabled(has_node);
        delete_action_->setEnabled(has_node);
        dissect_->setEnabled(has_node && values_->attached());
        follow_->setEnabled(has_node);

        process_label_->setText(to_qstring(target_->label()));
    }

    void ReClassDialog::on_class_selected(std::size_t index)
    {
        structure_->set_class(index);
        refresh_all();
    }

    void ReClassDialog::add_class()
    {
        widgets::InputBoxOptions name_options;
        name_options.title       = tr("Add Class");
        name_options.label       = tr("Name");
        name_options.placeholder = tr("Player");
        name_options.validate    = [this](const QString& text)
        {
            return class_name_error(text, nullptr);
        };

        const std::optional<QString> name = widgets::get_text(name_options, this);
        if (!name.has_value())
        {
            return;
        }

        widgets::InputBoxOptions address_options;
        address_options.title       = tr("Class Address");
        address_options.label       = tr("Address expression");
        address_options.placeholder = tr("game.exe+0x1234");
        address_options.monospace   = true;
        address_options.validate    = [this](const QString& text)
        {
            return validate_expression(text);
        };

        const std::optional<QString> address = widgets::get_text(address_options, this);
        if (!address.has_value())
        {
            return;
        }

        static_cast<void>(create_class(*name, *address));
    }

    bool ReClassDialog::create_class(const QString& name, const QString& expression)
    {
        const QString name_error = class_name_error(name, nullptr);
        if (!name_error.isEmpty())
        {
            report(name_error, true);
            return false;
        }
        const QString address_error = validate_expression(expression);
        if (!address_error.isEmpty())
        {
            report(address_error, true);
            return false;
        }

        const QString trimmed = name.trimmed();
        if (project_.add_class(trimmed.toStdString(), expression.trimmed().toStdString()) == 0)
        {
            report(tr("The class was not added."), true);
            return false;
        }

        // A new class starts covering a page of memory, so the user retypes the
        // fields they can see instead of adding bytes until the offsets reach.
        reclass::append_bytes(project_, project_.classes().back().nodes, reclass::kDefaultClassBytes);

        structure_->set_class(project_.classes().size() - 1);
        refresh_all();
        report(tr("Class '%1' added with %2 bytes of padding.").arg(trimmed).arg(reclass::kDefaultClassBytes), false);
        return true;
    }

    void ReClassDialog::rename_class()
    {
        if (!selected_class_exists())
        {
            report(tr("Select a class first."), true);
            return;
        }

        const std::size_t     index    = structure_->selected_class();
        const reclass::Class* selected = project_.class_at(index);

        widgets::InputBoxOptions options;
        options.title    = tr("Rename Class");
        options.label    = tr("Name");
        options.initial  = to_qstring(selected->name);
        options.validate = [this, selected](const QString& text)
        {
            return class_name_error(text, selected);
        };

        const std::optional<QString> name = widgets::get_text(options, this);
        if (!name.has_value())
        {
            return;
        }

        static_cast<void>(rename_selected_class(*name));
    }

    bool ReClassDialog::rename_selected_class(const QString& name)
    {
        if (!selected_class_exists())
        {
            report(tr("Select a class first."), true);
            return false;
        }

        const std::size_t index  = structure_->selected_class();
        const QString     reason = class_name_error(name, project_.class_at(index));
        if (!reason.isEmpty())
        {
            report(reason, true);
            return false;
        }

        project_.classes()[index].name = name.trimmed().toStdString();
        refresh_all();
        report(tr("Class renamed."), false);
        return true;
    }

    void ReClassDialog::set_class_address()
    {
        if (!selected_class_exists())
        {
            report(tr("Select a class first."), true);
            return;
        }

        const reclass::Class* selected = project_.class_at(structure_->selected_class());

        widgets::InputBoxOptions options;
        options.title       = tr("Class Address");
        options.label       = tr("Address expression");
        options.initial     = to_qstring(selected->address_expression);
        options.placeholder = tr("game.exe+0x1234");
        options.monospace   = true;
        options.validate    = [this](const QString& text)
        {
            return validate_expression(text);
        };

        const std::optional<QString> address = widgets::get_text(options, this);
        if (!address.has_value())
        {
            return;
        }

        static_cast<void>(set_selected_class_address(*address));
    }

    bool ReClassDialog::set_selected_class_address(const QString& address)
    {
        if (!selected_class_exists())
        {
            report(tr("Select a class first."), true);
            return false;
        }

        const QString reason = validate_expression(address);
        if (!reason.isEmpty())
        {
            report(reason, true);
            return false;
        }

        project_.classes()[structure_->selected_class()].address_expression = address.trimmed().toStdString();
        refresh_all();
        report(tr("Class address set."), false);
        return true;
    }

    void ReClassDialog::delete_class()
    {
        if (!selected_class_exists())
        {
            report(tr("Select a class first."), true);
            return;
        }

        const std::string name = project_.class_at(structure_->selected_class())->name;
        project_.remove_class(structure_->selected_class());
        refresh_all();
        report(tr("Class '%1' deleted.").arg(to_qstring(name)), false);
    }

    void ReClassDialog::insert_node(reclass::NodeKind kind)
    {
        if (!selected_class_exists())
        {
            report(tr("Select a class first."), true);
            return;
        }

        // A node nested in a container goes into that container beside its
        // element; a node of the class itself goes in front of the selection.
        std::vector<reclass::NodePtr>* owner = &project_.class_at(structure_->selected_class())->nodes;
        std::size_t                    index = owner->size();
        if (const models::ReclassRow* row = structure_->row_for(grid_->currentIndex()); row != nullptr)
        {
            const auto ref = reclass::node_ref(project_, row->node_id);
            if (ref.has_value())
            {
                owner = ref->owner;
                index = ref->index;
            }
        }

        owner->insert(owner->begin() + static_cast<std::ptrdiff_t>(index), reclass::create_node(project_, kind));

        refresh_all();
        report(tr("%1 node added.").arg(kind_label(kind)), false);
    }

    void ReClassDialog::change_node_kind(reclass::NodeKind kind)
    {
        const models::ReclassRow* row = structure_->row_for(grid_->currentIndex());
        if (row == nullptr)
        {
            report(tr("Select a node first."), true);
            return;
        }

        const auto ref = reclass::node_ref(project_, row->node_id);
        if (!ref)
        {
            return;
        }

        reclass::change_node_kind(project_, *ref->owner, ref->index, kind);
        refresh_all();
        report(tr("Type changed to %1.").arg(kind_label(kind)), false);
    }

    std::optional<std::size_t> ReClassDialog::prompt_byte_count(const QString& title)
    {
        widgets::InputBoxOptions options;
        options.title    = title;
        options.label    = tr("Byte count");
        options.initial  = QStringLiteral("1");
        options.validate = [this](const QString& text)
        {
            bool             ok    = false;
            const qulonglong bytes = text.trimmed().toULongLong(&ok);
            if (!ok || bytes == 0)
            {
                return tr("A positive byte count is required.");
            }
            return QString();
        };

        const std::optional<QString> text = widgets::get_text(options, this);
        return text.has_value() ? std::optional<std::size_t> {text->trimmed().toULongLong()} : std::nullopt;
    }

    void ReClassDialog::add_bytes()
    {
        const std::optional<std::size_t> count = prompt_byte_count(tr("Add Bytes"));
        if (count.has_value())
        {
            static_cast<void>(add_bytes(*count));
        }
    }

    void ReClassDialog::insert_bytes()
    {
        const std::optional<std::size_t> count = prompt_byte_count(tr("Insert Bytes"));
        if (count.has_value())
        {
            static_cast<void>(insert_bytes(*count));
        }
    }

    std::optional<ReClassDialog::ContainerTarget> ReClassDialog::byte_target(bool insert)
    {
        const models::ReclassRow* row =
            grid_->currentIndex().isValid() ? structure_->row_for(grid_->currentIndex()) : nullptr;
        if (row == nullptr)
        {
            if (!selected_class_exists())
            {
                return std::nullopt;
            }
            reclass::Class* type = project_.class_at(structure_->selected_class());
            return ContainerTarget {.nodes = &type->nodes, .index = insert ? 0 : type->nodes.size()};
        }

        const auto ref = reclass::node_ref(project_, row->node_id);
        if (!ref)
        {
            return std::nullopt;
        }

        // A container takes the bytes itself, another node's parent container
        // takes them at the node's position; the reference reaches the nodes the
        // same way.
        if (auto* container = dynamic_cast<reclass::ContainerNode*>(ref->owner->at(ref->index).get());
            container != nullptr)
        {
            std::vector<reclass::NodePtr>& children = container->children_mutable();
            return ContainerTarget {.nodes = &children, .index = insert ? 0 : children.size()};
        }
        return ContainerTarget {.nodes = ref->owner, .index = insert ? ref->index : ref->owner->size()};
    }

    void ReClassDialog::insert_padding(const ContainerTarget& target, std::size_t count)
    {
        std::vector<reclass::NodePtr> padding = reclass::pad_nodes(count);
        for (reclass::NodePtr& node : padding)
        {
            node->id = project_.next_node_id();
        }
        auto& nodes = *target.nodes;
        nodes.insert(nodes.begin() + static_cast<std::ptrdiff_t>(std::min(target.index, nodes.size())),
                     std::make_move_iterator(padding.begin()),
                     std::make_move_iterator(padding.end()));
    }

    bool ReClassDialog::add_bytes(std::size_t count)
    {
        if (count == 0)
        {
            return false;
        }
        const auto target = byte_target(false);
        if (!target.has_value())
        {
            report(tr("Select a class first."), true);
            return false;
        }
        insert_padding(*target, count);
        refresh_all();
        report(tr("%1 byte(s) added.").arg(count), false);
        return true;
    }

    bool ReClassDialog::insert_bytes(std::size_t count)
    {
        if (count == 0)
        {
            return false;
        }
        const auto target = byte_target(true);
        if (!target.has_value())
        {
            report(tr("Select a class first."), true);
            return false;
        }
        insert_padding(*target, count);
        refresh_all();
        report(tr("%1 byte(s) inserted.").arg(count), false);
        return true;
    }

    void ReClassDialog::edit_name()
    {
        const models::ReclassRow* row  = structure_->row_for(grid_->currentIndex());
        reclass::Node*            node = node_at(row);
        if (node == nullptr)
        {
            report(tr("Select a node first."), true);
            return;
        }

        widgets::InputBoxOptions options;
        options.title    = tr("Node Name");
        options.label    = tr("Name");
        options.initial  = to_qstring(node->name);
        options.validate = [this](const QString& text)
        {
            return text.trimmed().isEmpty() ? tr("A node name is required.") : QString();
        };

        const std::optional<QString> name = widgets::get_text(options, this);
        if (!name.has_value())
        {
            return;
        }

        // The rename goes through the model the grid edits through, so the two
        // paths cannot report different outcomes.
        static_cast<void>(structure_->set_node_name(*row, *name));
    }

    void ReClassDialog::edit_comment()
    {
        const models::ReclassRow* row  = structure_->row_for(grid_->currentIndex());
        reclass::Node*            node = node_at(row);
        if (node == nullptr)
        {
            report(tr("Select a node first."), true);
            return;
        }

        widgets::InputBoxOptions options;
        options.title   = tr("Node Comment");
        options.label   = tr("Comment");
        options.initial = to_qstring(node->comment);

        const std::optional<QString> comment = widgets::get_text(options, this);
        if (!comment.has_value())
        {
            return;
        }

        // The note goes through the model the grid edits through, so the two
        // paths cannot report different outcomes.
        static_cast<void>(structure_->set_node_comment(*row, *comment));
    }

    void ReClassDialog::set_node_hex(bool hex)
    {
        const std::vector<const models::ReclassRow*> rows = selected_rows();
        if (rows.empty())
        {
            report(tr("Select a node first."), true);
            return;
        }

        for (const models::ReclassRow* row : rows)
        {
            if (reclass::Node* node = node_at(row); node != nullptr)
            {
                node->hex = hex;
            }
        }
        refresh_all();
        report(hex ? tr("Shown as hex.") : tr("Shown as decimal."), false);
    }

    void ReClassDialog::copy_name()
    {
        const models::ReclassRow* row  = structure_->row_for(grid_->currentIndex());
        const reclass::Node*      node = node_at(row);
        if (node == nullptr)
        {
            report(tr("Select a node first."), true);
            return;
        }

        QApplication::clipboard()->setText(to_qstring(node->name));
        report(tr("Name copied."), false);
    }

    void ReClassDialog::remove_selected_nodes()
    {
        std::vector<std::size_t> targets;
        for (const models::ReclassRow* row : selected_rows())
        {
            targets.push_back(row->node_id);
        }
        if (targets.empty())
        {
            report(tr("Select a node first."), true);
            return;
        }

        for (const std::size_t node_id : targets)
        {
            reclass::remove_node(project_, node_id);
        }

        refresh_all();
        report(tr("%1 node(s) deleted.").arg(static_cast<int>(targets.size())), false);
    }

    void ReClassDialog::dissect()
    {
        if (dissect_job_.has_value())
        {
            report(tr("A dissect is already running."), true);
            return;
        }
        if (!values_->attached())
        {
            report(tr("Attach to a target first."), true);
            return;
        }

        dissect_targets_.clear();
        for (const models::ReclassRow* row : selected_rows())
        {
            const reclass::Node* node = node_at(row);
            if (node == nullptr || !reclass::is_hex_kind(node->kind()))
            {
                continue;
            }

            const std::span<const std::byte>   bytes   = values_->node_bytes(*row);
            const std::optional<std::uint64_t> address = values_->node_address(*row);
            if (!address.has_value() || bytes.size() < reclass::kind_size(node->kind()))
            {
                continue;
            }

            dissect_targets_.push_back(DissectTarget {.node_id = row->node_id,
                                                      .kind    = node->kind(),
                                                      .offset  = row->offset,
                                                      .address = *address,
                                                      .bytes   = std::vector<std::byte>(bytes.begin(), bytes.end())});
        }

        if (dissect_targets_.empty())
        {
            report(tr("Select a hex node whose bytes were read."), true);
            return;
        }

        // A pointer is only guessed for a value that can be dereferenced: a
        // readable region answers that on its own, anything else costs one byte.
        std::vector<process::ReadManyItem> probes;
        for (const DissectTarget& target : dissect_targets_)
        {
            if (target.bytes.size() < 8)
            {
                continue;
            }
            std::uint64_t value = 0;
            std::memcpy(&value, target.bytes.data(), sizeof(value));
            if (value != 0 && !values_->inside_region(value))
            {
                probes.push_back(process::ReadManyItem {.address = value, .size = 1});
            }
        }

        if (probes.empty())
        {
            apply_dissect({});
            return;
        }

        const process::JobId                     id    = worker_->next_job_id();
        const std::vector<process::ReadManyItem> items = probes;
        dissect_job_                                   = id;

        const bool submitted = worker_->submit_read_many(
            id,
            std::move(probes),
            [this, id, items](process::JobResult&& result)
            {
                if (dissect_job_ != id)
                {
                    return;
                }
                dissect_job_.reset();

                std::vector<std::uint64_t> readable;
                if (std::holds_alternative<process::ReadManyResult>(result))
                {
                    const auto& read = std::get<process::ReadManyResult>(result);
                    for (std::size_t index = 0; index < read.items.size() && index < items.size(); ++index)
                    {
                        if (read.items[index].has_value() && !read.items[index]->empty())
                        {
                            readable.push_back(items[index].address);
                        }
                    }
                }
                apply_dissect(readable);
            });
        if (!submitted)
        {
            dissect_job_.reset();
            dissect_targets_.clear();
            report(tr("The dissect could not be submitted."), true);
        }
    }

    void ReClassDialog::apply_dissect(const std::vector<std::uint64_t>& readable)
    {
        const std::size_t total   = dissect_targets_.size();
        std::size_t       changed = 0;

        for (const DissectTarget& target : dissect_targets_)
        {
            const reclass::DissectLookup lookup {.readable = [this, &readable](std::uint64_t address, std::size_t)
                                                 {
                                                     return values_->inside_region(address)
                                                         || std::ranges::find(readable, address) != readable.end();
                                                 }};

            const auto guess = reclass::guess_kind(
                reclass::DissectInput {
                    .current = target.kind, .bytes = target.bytes, .offset = target.offset, .address = target.address},
                lookup);
            const auto ref = reclass::node_ref(project_, target.node_id);
            if (!guess.has_value() || !ref)
            {
                continue;
            }

            reclass::change_node_kind(project_, *ref->owner, ref->index, *guess);
            ++changed;
        }

        dissect_targets_.clear();
        refresh_all();
        report(tr("dissected %1 of %2 nodes").arg(changed).arg(total), false);
    }

    void ReClassDialog::follow_pointer()
    {
        if (node_at(structure_->row_for(grid_->currentIndex())) == nullptr)
        {
            report(tr("Select a node first."), true);
            return;
        }

        QMenu menu(this);
        widgets::show_explanations(menu);

        QAction* clear = widgets::described_action(menu, tr("Pointer"), tr("Drop the class reference."));
        connect(clear,
                &QAction::triggered,
                this,
                [this]
                {
                    set_followed_class(QString());
                });

        if (!project_.classes().empty())
        {
            menu.addSeparator();
        }
        for (const reclass::Class& type : project_.classes())
        {
            const QString name = to_qstring(type.name);
            QAction*      action =
                widgets::described_action(menu, name, tr("Inline %1's fields beneath this node.").arg(name));
            connect(action,
                    &QAction::triggered,
                    this,
                    [this, name]
                    {
                        set_followed_class(name);
                    });
        }

        menu.exec(tool_bar_->mapToGlobal(tool_bar_->rect().bottomLeft()));
    }

    void ReClassDialog::set_followed_class(const QString& class_name)
    {
        const models::ReclassRow* row  = structure_->row_for(grid_->currentIndex());
        reclass::Node*            node = node_at(row);
        if (row == nullptr || node == nullptr)
        {
            report(tr("Select a node first."), true);
            return;
        }
        if (!reclass::is_class_kind(node->kind()) && node->kind() != reclass::NodeKind::pointer)
        {
            report(tr("Select a pointer node first."), true);
            return;
        }

        const std::size_t node_id = row->node_id;

        if (class_name.isEmpty())
        {
            if (node->kind() == reclass::NodeKind::class_instance)
            {
                report(tr("An instance needs a class; change its type to a pointer first."), true);
                return;
            }
            const auto ref = reclass::node_ref(project_, row->node_id);
            if (!ref)
            {
                return;
            }
            reclass::change_node_kind(project_, *ref->owner, ref->index, reclass::NodeKind::pointer);
            refresh_all();
            report(tr("Pointer cleared."), false);
            return;
        }

        const reclass::Class* target = project_.find_class(class_name.toStdString());
        if (target == nullptr)
        {
            report(tr("Unknown class '%1'.").arg(class_name), true);
            return;
        }

        if (node->kind() == reclass::NodeKind::pointer)
        {
            const auto ref = reclass::node_ref(project_, row->node_id);
            if (!ref)
            {
                return;
            }
            reclass::change_node_kind(project_, *ref->owner, ref->index, reclass::NodeKind::class_pointer);
            node = (*ref->owner)[ref->index].get();
        }
        node->class_name = target->name;

        // Inline the class right away so the fields appear where the user asked.
        structure_->set_expanded(node_id, true);
        refresh_all();
        if (const QModelIndex index = structure_->index_for_node(node_id); index.isValid())
        {
            grid_->setCurrentIndex(index);
        }
        report(tr("Following '%1'.").arg(class_name), false);
    }

    reclass::Node* ReClassDialog::node_at(const models::ReclassRow* row) const
    {
        if (row == nullptr)
        {
            return nullptr;
        }
        return const_cast<reclass::Node*>(row->node);
    }

    void ReClassDialog::show_row_menu(const QPoint& position)
    {
        QMenu menu(this);
        populate_row_menu(menu);
        menu.exec(position);
    }

    void ReClassDialog::step_element(std::size_t node_id, int delta)
    {
        // The structure model re-flattens the class around the element it now
        // shows, and the grid re-measures and repaints on the reset.
        if (!structure_->step_element(node_id, delta))
        {
            report(tr("This array holds a single element."), true);
        }
    }

    void ReClassDialog::populate_row_menu(QMenu& menu)
    {
        widgets::show_explanations(menu);

        QMenu* insert = widgets::explained_submenu(menu, tr("Insert Node"));
        populate_node_kind_menu(*insert, false);

        QMenu* change = widgets::explained_submenu(menu, tr("Change Type"));
        populate_node_kind_menu(*change, true);

        // The reference puts both byte commands in the node menu as submenus of
        // the same presets the toolbar offers and enables them only where they
        // can act: adding needs a selection, inserting needs exactly one node
        // because the padding goes in front of that node.
        const std::size_t selected       = selected_rows().size();
        QMenu*            add_bytes_menu = widgets::explained_submenu(menu, tr("Add Bytes"));
        populate_add_bytes_menu(*add_bytes_menu, false);
        add_bytes_menu->setEnabled(selected > 0);

        QMenu* insert_bytes_menu = widgets::explained_submenu(menu, tr("Insert Bytes"));
        populate_add_bytes_menu(*insert_bytes_menu, true);
        insert_bytes_menu->setEnabled(selected == 1);

        menu.addSeparator();

        QAction* name = widgets::described_action(menu, tr("Edit Name"), tr("Rename the selected node."));
        connect(name, &QAction::triggered, this, &ReClassDialog::edit_name);

        QAction* comment = widgets::described_action(menu, tr("Edit Comment"), tr("Describe the selected node."));
        connect(comment, &QAction::triggered, this, &ReClassDialog::edit_comment);

        QAction* hex =
            widgets::described_action(menu, tr("Show as hex"), tr("Show the selected node's value in hexadecimal."));
        connect(hex,
                &QAction::triggered,
                this,
                [this]
                {
                    set_node_hex(true);
                });

        QAction* decimal =
            widgets::described_action(menu, tr("Show as decimal"), tr("Show the selected node's value in decimal."));
        connect(decimal,
                &QAction::triggered,
                this,
                [this]
                {
                    set_node_hex(false);
                });

        menu.addSeparator();

        QMenu*   copy      = widgets::explained_submenu(menu, tr("Copy"));
        QAction* name_copy = widgets::described_action(*copy, tr("Copy Name"), tr("Copy the selected node's name."));
        connect(name_copy, &QAction::triggered, this, &ReClassDialog::copy_name);

        QAction* dissect = widgets::described_action(
            menu, tr("Dissect"), tr("Guess the selected hex node's type from its live bytes."));
        connect(dissect, &QAction::triggered, this, &ReClassDialog::dissect);

        QAction* follow =
            widgets::described_action(menu, tr("Follow Pointer"), tr("Inline the class a pointer node refers to."));
        connect(follow, &QAction::triggered, this, &ReClassDialog::follow_pointer);

        // The two element commands walk the array the row's cursor is on one
        // element at a time. An array that holds a single element has nowhere to
        // step, so both entries are dead and say why in their tooltip.
        const std::optional<std::size_t> array_node = current_node_id();
        const std::size_t                elements = array_node.has_value() ? structure_->element_count(*array_node) : 0;
        if (elements > 1)
        {
            QAction* previous = widgets::described_action(
                menu, tr("Previous Element"), tr("Show the element before the one this row shows."));
            connect(previous,
                    &QAction::triggered,
                    this,
                    [this, node = *array_node]
                    {
                        step_element(node, -1);
                    });

            QAction* next = widgets::described_action(
                menu, tr("Next Element"), tr("Show the element after the one this row shows."));
            connect(next,
                    &QAction::triggered,
                    this,
                    [this, node = *array_node]
                    {
                        step_element(node, 1);
                    });
        }
        else
        {
            const QString reason =
                elements == 1 ? tr("This array holds a single element.") : tr("Select an array row first.");
            (void)widgets::disabled_action(menu, tr("Previous Element"), reason);
            (void)widgets::disabled_action(menu, tr("Next Element"), reason);
        }

        QAction* remove = widgets::described_action(menu, tr("Delete"), tr("Remove the selected node(s)."));
        widgets::show_shortcut_hint(*remove, *delete_action_);
        connect(remove, &QAction::triggered, this, &ReClassDialog::remove_selected_nodes);
    }

    void ReClassDialog::create_class_from_nodes()
    {
        const models::ReclassRow* row =
            grid_->currentIndex().isValid() ? structure_->row_for(grid_->currentIndex()) : nullptr;
        if (row == nullptr)
        {
            report(tr("Select node(s) first."), true);
            return;
        }
        const auto anchor = reclass::node_ref(project_, row->node_id);
        if (!anchor.has_value())
        {
            return;
        }

        std::vector<std::size_t> indexes;
        for (const std::size_t id : selected_node_ids())
        {
            const auto selected = reclass::node_ref(project_, id);
            if (!selected.has_value() || selected->owner != anchor->owner)
            {
                report(tr("Select the nodes of one container."), true);
                return;
            }
            indexes.push_back(selected->index);
        }
        std::ranges::sort(indexes);
        indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());

        const std::string name = std::format("Class{}", project_.classes().size() + 1);
        if (project_.add_class(name, std::string {}) == 0)
        {
            report(tr("The class was not created."), true);
            return;
        }

        reclass::Class&               created = project_.classes().back();
        std::vector<reclass::NodePtr> moved;
        moved.reserve(indexes.size());
        for (const std::size_t index : indexes)
        {
            moved.push_back(std::move((*anchor->owner)[index]));
        }
        for (auto it = indexes.rbegin(); it != indexes.rend(); ++it)
        {
            anchor->owner->erase(anchor->owner->begin() + static_cast<std::ptrdiff_t>(*it));
        }
        for (reclass::NodePtr& node : moved)
        {
            node->id = project_.next_node_id();
            created.nodes.push_back(std::move(node));
        }

        // The bytes the nodes covered stay covered: one class instance, as large
        // as the fields that moved, takes their place.
        reclass::NodePtr instance = reclass::make_node(reclass::NodeKind::class_instance);
        instance->id              = project_.next_node_id();
        instance->name            = name;
        instance->class_name      = name;
        const std::size_t at      = std::min(indexes.front(), anchor->owner->size());
        anchor->owner->insert(anchor->owner->begin() + static_cast<std::ptrdiff_t>(at), std::move(instance));

        refresh_all();
        report(tr("Class '%1' created from %2 node(s).").arg(to_qstring(name)).arg(moved.size()), false);
    }

    void ReClassDialog::copy_address()
    {
        const models::ReclassRow* row =
            grid_->currentIndex().isValid() ? structure_->row_for(grid_->currentIndex()) : nullptr;
        if (row == nullptr)
        {
            report(tr("Select a node first."), true);
            return;
        }
        const std::optional<std::uint64_t> address = values_->node_address(*row);
        if (!address.has_value())
        {
            report(tr("The node's address is not resolved yet."), true);
            return;
        }
        QApplication::clipboard()->setText(values_->format_address(*address));
        report(tr("Address copied."), false);
    }

    void ReClassDialog::shrink_class()
    {
        if (!selected_class_exists())
        {
            report(tr("Select a class first."), true);
            return;
        }

        // The reference cuts the padding hex nodes off the class's end.
        reclass::Class* type    = project_.class_at(structure_->selected_class());
        std::size_t     removed = 0;
        while (!type->nodes.empty() && reclass::is_hex_kind(type->nodes.back()->kind()))
        {
            type->nodes.pop_back();
            ++removed;
        }

        refresh_all();
        report(tr("%1 padding node(s) removed.").arg(removed), false);
    }

    void ReClassDialog::remove_unused_classes()
    {
        std::vector<bool> used(project_.classes().size(), false);
        for (const reclass::Class& type : project_.classes())
        {
            for (const reclass::NodePtr& node : type.nodes)
            {
                if (node->class_name.empty())
                {
                    continue;
                }
                const reclass::Class* target = project_.find_class(node->class_name);
                if (target != nullptr)
                {
                    used[static_cast<std::size_t>(target - project_.classes().data())] = true;
                }
            }
        }

        std::size_t removed = 0;
        for (std::size_t index = project_.classes().size(); index-- > 0;)
        {
            if (!used[index])
            {
                project_.remove_class(index);
                ++removed;
            }
        }

        refresh_all();
        report(tr("%1 unused class(es) removed.").arg(removed), false);
    }

    void ReClassDialog::hide_selected_nodes()
    {
        std::size_t hidden = 0;
        for (const models::ReclassRow* row : selected_rows())
        {
            reclass::Node* node = node_at(row);
            if (node == nullptr || node->hidden)
            {
                continue;
            }
            node->hidden = true;
            ++hidden;
        }
        if (hidden == 0)
        {
            report(tr("Select node(s) first."), true);
            return;
        }

        refresh_all();
        report(tr("%1 node(s) hidden.").arg(hidden), false);
    }

    void ReClassDialog::unhide_selected_nodes()
    {
        std::size_t revealed = 0;
        for (const models::ReclassRow* row : selected_rows())
        {
            reclass::Node* node = node_at(row);
            if (node == nullptr || !node->hidden)
            {
                continue;
            }
            node->hidden = false;
            ++revealed;
        }
        if (revealed == 0)
        {
            report(tr("Select hidden node(s) first."), true);
            return;
        }

        refresh_all();
        report(tr("%1 node(s) revealed.").arg(revealed), false);
    }

    void ReClassDialog::unhide_child_nodes()
    {
        std::size_t revealed = 0;
        for (const models::ReclassRow* row : selected_rows())
        {
            const reclass::Node* node = node_at(row);
            if (node == nullptr)
            {
                continue;
            }
            for (const reclass::NodePtr& child : node->children())
            {
                if (child->hidden)
                {
                    child->hidden = false;
                    ++revealed;
                }
            }
        }
        if (revealed == 0)
        {
            report(tr("No hidden child nodes."), true);
            return;
        }

        refresh_all();
        report(tr("%1 node(s) revealed.").arg(revealed), false);
    }

    void ReClassDialog::unhide_above_nodes()
    {
        unhide_run(true);
    }

    void ReClassDialog::unhide_below_nodes()
    {
        unhide_run(false);
    }

    void ReClassDialog::unhide_run(bool above)
    {
        const models::ReclassRow* row =
            grid_->currentIndex().isValid() ? structure_->row_for(grid_->currentIndex()) : nullptr;
        if (row == nullptr)
        {
            report(tr("Select a node first."), true);
            return;
        }
        const auto ref = reclass::node_ref(project_, row->node_id);
        if (!ref.has_value())
        {
            return;
        }

        // The reference reveals the unbroken run of hidden siblings on one side.
        std::size_t revealed = 0;
        if (above)
        {
            for (std::size_t index = ref->index; index-- > 0 && (*ref->owner)[index]->hidden;)
            {
                (*ref->owner)[index]->hidden = false;
                ++revealed;
            }
        }
        else
        {
            for (std::size_t index = ref->index + 1; index < ref->owner->size() && (*ref->owner)[index]->hidden;
                 ++index)
            {
                (*ref->owner)[index]->hidden = false;
                ++revealed;
            }
        }

        refresh_all();
        report(tr("%1 node(s) revealed.").arg(revealed), false);
    }

    void ReClassDialog::copy_selected_nodes()
    {
        clipboard_.clear();
        for (const models::ReclassRow* row : selected_rows())
        {
            const reclass::Node* node = node_at(row);
            if (node != nullptr)
            {
                clipboard_.push_back(node->clone());
            }
        }
        if (clipboard_.empty())
        {
            report(tr("Select node(s) first."), true);
            return;
        }
        report(tr("%1 node(s) copied.").arg(clipboard_.size()), false);
    }

    void ReClassDialog::paste_nodes()
    {
        if (clipboard_.empty())
        {
            report(tr("Copy node(s) first."), true);
            return;
        }
        const auto target = byte_target(false);
        if (!target.has_value())
        {
            report(tr("Select a class first."), true);
            return;
        }

        // A clone keeps its shape but not its ids, so a pasted sub-tree is a
        // fresh set of nodes.
        const auto assign_ids = [this](auto&& self, reclass::Node& node) -> void
        {
            node.id = project_.next_node_id();
            if (auto* container = dynamic_cast<reclass::ContainerNode*>(&node); container != nullptr)
            {
                for (reclass::NodePtr& child : container->children_mutable())
                {
                    self(self, *child);
                }
            }
        };

        std::vector<reclass::NodePtr> clones;
        clones.reserve(clipboard_.size());
        for (const reclass::NodePtr& node : clipboard_)
        {
            reclass::NodePtr clone = node->clone();
            assign_ids(assign_ids, *clone);
            clones.push_back(std::move(clone));
        }

        auto& nodes = *target->nodes;
        nodes.insert(nodes.begin() + static_cast<std::ptrdiff_t>(std::min(target->index, nodes.size())),
                     std::make_move_iterator(clones.begin()),
                     std::make_move_iterator(clones.end()));
        refresh_all();
        report(tr("%1 node(s) pasted.").arg(clones.size()), false);
    }

    std::size_t ReClassDialog::clipboard_size() const noexcept
    {
        return clipboard_.size();
    }

    void ReClassDialog::set_class_hierarchy_view(bool enabled)
    {
        if (hierarchy_ != nullptr && hierarchy_->isChecked() != enabled)
        {
            hierarchy_->setChecked(enabled);
        }
        project_view_->set_hierarchy_view(enabled);
    }

    bool ReClassDialog::class_hierarchy_view() const noexcept
    {
        return project_view_->hierarchy_view();
    }

    void ReClassDialog::search_equal_values()
    {
        const models::ReclassRow* row =
            grid_->currentIndex().isValid() ? structure_->row_for(grid_->currentIndex()) : nullptr;
        if (row == nullptr || row->node == nullptr)
        {
            report(tr("Select a node first."), true);
            return;
        }

        const int bytes = static_cast<int>(std::max<std::size_t>(row->node->value_size(), 1));
        emit      searchRequested(values_->edit_text(*row), bytes);
        report(tr("Searching for the node's value."), false);
    }

    void ReClassDialog::watch_address(bool writes)
    {
        const models::ReclassRow* row =
            grid_->currentIndex().isValid() ? structure_->row_for(grid_->currentIndex()) : nullptr;
        if (row == nullptr)
        {
            report(tr("Select a node first."), true);
            return;
        }
        const std::optional<std::uint64_t> address = values_->node_address(*row);
        if (!address.has_value())
        {
            report(tr("The node's address is not resolved yet."), true);
            return;
        }

        const std::size_t width = std::max<std::size_t>(row->node->value_size(), 1);
        emit              accessWatchRequested(
            *address, width, writes ? debug::Kind::hardware_write : debug::Kind::hardware_read_write);
        report(writes ? tr("Arming the Access Watch on writes.") : tr("Arming the Access Watch on accesses."), false);
    }

    std::vector<QString> ReClassDialog::enum_names() const
    {
        std::vector<QString> names;
        names.reserve(project_.enums().size());
        for (const reclass::EnumDescription& description : project_.enums())
        {
            names.push_back(to_qstring(description.name));
        }
        return names;
    }

    bool ReClassDialog::create_enum(const QString& name, std::size_t size)
    {
        if (project_.add_enum(name.trimmed().toStdString(), size) == nullptr)
        {
            report(tr("An enum named '%1' already exists.").arg(name.trimmed()), true);
            return false;
        }
        report(tr("Enum '%1' added.").arg(name.trimmed()), false);
        return true;
    }

    bool ReClassDialog::remove_enum(const QString& name)
    {
        for (std::size_t index = 0; index < project_.enums().size(); ++index)
        {
            if (project_.enums()[index].name == name.toStdString())
            {
                project_.remove_enum(index);
                report(tr("Enum '%1' removed.").arg(name), false);
                return true;
            }
        }
        report(tr("Unknown enum '%1'.").arg(name), true);
        return false;
    }

    void ReClassDialog::add_enum()
    {
        EnumEditorDialog editor {project_, QString {}, this};
        if (editor.exec() == QDialog::Accepted)
        {
            report(tr("Enum '%1' added.").arg(editor.applied_name()), false);
        }
    }

    void ReClassDialog::edit_enum(std::size_t index)
    {
        if (index >= project_.enums().size())
        {
            return;
        }
        EnumEditorDialog editor {project_, to_qstring(project_.enums()[index].name), this};
        if (editor.exec() == QDialog::Accepted)
        {
            report(tr("Enum '%1' saved.").arg(editor.applied_name()), false);
        }
    }

    void ReClassDialog::delete_enum(std::size_t index)
    {
        if (index >= project_.enums().size())
        {
            return;
        }
        const QString name = to_qstring(project_.enums()[index].name);
        project_.remove_enum(index);
        report(tr("Enum '%1' removed.").arg(name), false);
    }

}