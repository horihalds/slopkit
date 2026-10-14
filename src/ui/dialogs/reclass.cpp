#include "ui/dialogs/reclass.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <string>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "expr/expression.hpp"
#include "reclass/dissect.hpp"
#include "reclass/edit.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/components/input_box.hpp"
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
        // The node kinds this version builds, in the order the menus list them.
        constexpr std::array<reclass::NodeKind, 19> kNodeKinds {
            reclass::NodeKind::int8,          reclass::NodeKind::int16,   reclass::NodeKind::int32,
            reclass::NodeKind::int64,         reclass::NodeKind::uint8,   reclass::NodeKind::uint16,
            reclass::NodeKind::uint32,        reclass::NodeKind::uint64,  reclass::NodeKind::float32,
            reclass::NodeKind::float64,       reclass::NodeKind::boolean, reclass::NodeKind::character,
            reclass::NodeKind::hex8,          reclass::NodeKind::hex16,   reclass::NodeKind::hex32,
            reclass::NodeKind::hex64,         reclass::NodeKind::pointer, reclass::NodeKind::class_instance,
            reclass::NodeKind::class_pointer,
        };

        QString kind_label(reclass::NodeKind kind)
        {
            return to_qstring(reclass::describe(kind));
        }

        // A column is as wide as the widest text it usually holds, measured once
        // from the font. Letting the header measure the model instead would make
        // it walk every row again on each pass a scroll or a resize lays out,
        // which is the one thing a grid that repaints on a live cadence cannot
        // afford.
        int content_width(const QWidget& widget, const QFont& font, const QString& sample)
        {
            const int margin = 2 * widget.style()->pixelMetric(QStyle::PM_HeaderMargin, nullptr, &widget);
            return QFontMetrics(font).horizontalAdvance(sample) + margin;
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
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        auto* splitter = new QSplitter(Qt::Horizontal, this);
        splitter->addWidget(build_class_pane());
        splitter->addWidget(build_grid_pane());
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        layout->addWidget(splitter, 1);

        auto* bottom = new QHBoxLayout();
        bottom->setSpacing(6);
        status_ = new widgets::StatusLabel(this);
        status_->setObjectName(QStringLiteral("reclass_status"));
        close_ = widgets::secondary_button(tr("Close"), this);
        close_->setObjectName(QStringLiteral("reclass_close"));
        bottom->addWidget(status_, 1);
        bottom->addWidget(close_);
        layout->addLayout(bottom);
    }

    QWidget* ReClassDialog::build_class_pane()
    {
        auto* pane   = new QWidget(this);
        auto* layout = new QVBoxLayout(pane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        class_list_ = new QListWidget(pane);
        class_list_->setObjectName(QStringLiteral("reclass_class_list"));
        class_list_->setContextMenuPolicy(Qt::CustomContextMenu);
        layout->addWidget(class_list_, 1);

        auto* buttons = new QHBoxLayout();
        buttons->setSpacing(4);
        add_class_ = widgets::secondary_button(tr("Add Class…"), pane);
        add_class_->setObjectName(QStringLiteral("reclass_add_class"));
        delete_class_ = widgets::secondary_button(tr("Delete Class"), pane);
        delete_class_->setObjectName(QStringLiteral("reclass_delete_class"));
        buttons->addWidget(add_class_);
        buttons->addWidget(delete_class_);
        layout->addLayout(buttons);

        return pane;
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

        auto* controls = new QHBoxLayout();
        controls->setSpacing(4);
        add_node_ = new QToolButton(pane);
        add_node_->setObjectName(QStringLiteral("reclass_add_node"));
        add_node_->setText(tr("Add Node"));
        add_node_->setPopupMode(QToolButton::InstantPopup);
        add_node_->setMenu(build_add_node_menu());
        delete_node_ = widgets::secondary_button(tr("Delete"), pane);
        delete_node_->setObjectName(QStringLiteral("reclass_delete_node"));
        dissect_ = widgets::secondary_button(tr("Dissect"), pane);
        dissect_->setObjectName(QStringLiteral("reclass_dissect"));
        follow_ = widgets::secondary_button(tr("Follow Pointer"), pane);
        follow_->setObjectName(QStringLiteral("reclass_follow"));
        controls->addWidget(add_node_);
        controls->addWidget(delete_node_);
        controls->addWidget(dissect_);
        controls->addWidget(follow_);
        controls->addStretch(1);
        layout->addLayout(controls);

        grid_ = new QTreeView(pane);
        grid_->setObjectName(QStringLiteral("reclass_grid"));
        grid_->setModel(proxy_);
        grid_->setEditTriggers(QAbstractItemView::DoubleClicked);
        grid_->setSelectionBehavior(QAbstractItemView::SelectRows);
        grid_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        grid_->setAllColumnsShowFocus(true);
        grid_->setUniformRowHeights(true);
        grid_->setContextMenuPolicy(Qt::CustomContextMenu);
        grid_->setAlternatingRowColors(true);
        grid_->setItemDelegate(new widgets::ElidedTooltipDelegate(grid_));

        // The address, offset, type, name and comment columns take a fixed width
        // derived from their widest text, while the value column - the widest of
        // them and the one a live pass rewrites - owns the slack.
        QHeaderView* header = grid_->header();
        header->setSectionResizeMode(QHeaderView::Interactive);
        header->setSectionResizeMode(models::ReclassStructureModel::value, QHeaderView::Stretch);
        header->resizeSection(models::ReclassStructureModel::address,
                              content_width(*grid_, mono_font(), ui::format_absolute(0)));
        header->resizeSection(models::ReclassStructureModel::offset,
                              content_width(*grid_, mono_font(), QStringLiteral("FFFFFFFF")));
        header->resizeSection(
            models::ReclassStructureModel::type,
            content_width(*grid_, pane->font(), to_qstring(reclass::describe(reclass::NodeKind::class_pointer))));
        header->resizeSection(models::ReclassStructureModel::name,
                              content_width(*grid_, pane->font(), QStringLiteral("field_00000000")));
        header->resizeSection(models::ReclassStructureModel::comment,
                              content_width(*grid_, pane->font(), tr("Comment")));
        layout->addWidget(grid_, 1);

        return pane;
    }

    QMenu* ReClassDialog::build_add_node_menu()
    {
        auto* menu = new QMenu(this);
        populate_node_kind_menu(*menu, false);
        return menu;
    }

    void ReClassDialog::populate_node_kind_menu(QMenu& menu, bool change)
    {
        widgets::show_explanations(menu);
        for (const reclass::NodeKind kind : kNodeKinds)
        {
            const QString label  = kind_label(kind);
            const QString detail = change ? tr("Replace the selected node's type with %1.").arg(label)
                                          : tr("Insert a %1 node before the selection.").arg(label);
            QAction*      action = widgets::described_action(menu, label, detail);
            connect(action,
                    &QAction::triggered,
                    this,
                    [this, kind, change]
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

    void ReClassDialog::connect_signals()
    {
        connect(add_class_, &QPushButton::clicked, this, &ReClassDialog::add_class);
        connect(delete_class_, &QPushButton::clicked, this, &ReClassDialog::delete_class);
        connect(close_, &QPushButton::clicked, this, &QDialog::close);
        connect(class_list_, &QListWidget::currentRowChanged, this, &ReClassDialog::on_class_selected);
        connect(class_list_,
                &QWidget::customContextMenuRequested,
                this,
                [this](const QPoint& position)
                {
                    show_class_menu(position);
                });

        connect(delete_node_, &QPushButton::clicked, this, &ReClassDialog::remove_selected_nodes);
        connect(dissect_, &QPushButton::clicked, this, &ReClassDialog::dissect);
        connect(follow_, &QPushButton::clicked, this, &ReClassDialog::follow_pointer);
        connect(grid_,
                &QWidget::customContextMenuRequested,
                this,
                [this](const QPoint& position)
                {
                    show_row_menu(position);
                });
        connect(grid_, &QTreeView::expanded, this, &ReClassDialog::on_grid_expanded);
        connect(grid_, &QTreeView::collapsed, this, &ReClassDialog::on_grid_collapsed);

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

        delete_action_ = new QAction(tr("Delete Node"), this);
        delete_action_->setShortcut(QKeySequence(Qt::Key_Delete));
        delete_action_->setShortcutContext(Qt::WindowShortcut);
        addAction(delete_action_);
        connect(delete_action_, &QAction::triggered, this, &ReClassDialog::remove_selected_nodes);

        widgets::chain_tab_order({class_list_, add_class_, delete_class_, grid_, delete_node_, close_});
    }

    models::ReclassStructureModel* ReClassDialog::structure() const noexcept
    {
        return structure_;
    }

    models::ReclassProxyModel* ReClassDialog::proxy() const noexcept
    {
        return proxy_;
    }

    QListWidget* ReClassDialog::class_list() const noexcept
    {
        return class_list_;
    }

    QTreeView* ReClassDialog::grid() const noexcept
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

    QPushButton* ReClassDialog::add_class_button() const noexcept
    {
        return add_class_;
    }

    QPushButton* ReClassDialog::delete_class_button() const noexcept
    {
        return delete_class_;
    }

    QToolButton* ReClassDialog::add_node_button() const noexcept
    {
        return add_node_;
    }

    QPushButton* ReClassDialog::delete_node_button() const noexcept
    {
        return delete_node_;
    }

    QPushButton* ReClassDialog::dissect_button() const noexcept
    {
        return dissect_;
    }

    QPushButton* ReClassDialog::follow_button() const noexcept
    {
        return follow_;
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
        refresh_all();
    }

    void ReClassDialog::set_modules(std::vector<process::ModuleInfo> modules)
    {
        values_->set_modules(std::move(modules));
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
        for (const QModelIndex& index : grid_->selectionModel()->selectedRows(models::ReclassStructureModel::name))
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

        refresh_class_list();
        values_->invalidate();
        sync_expansion();

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

    void ReClassDialog::refresh_class_list()
    {
        const std::size_t size     = project_.classes().size();
        const std::size_t selected = size == 0 ? 0 : std::min(structure_->selected_class(), size - 1);

        {
            const QSignalBlocker blocker(class_list_);
            class_list_->clear();
            for (const reclass::Class& type : project_.classes())
            {
                new QListWidgetItem(to_qstring(type.name), class_list_);
            }
            if (size > 0)
            {
                class_list_->setCurrentRow(static_cast<int>(selected));
            }
        }

        structure_->set_class(selected);
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
                ? tr("%1 · %2 → %3").arg(to_qstring(type->name), expression, values_->format_address(*address))
                : tr("%1 · %2 → not resolved yet").arg(to_qstring(type->name), expression));
    }

    void ReClassDialog::refresh_class_tooltips()
    {
        for (int row = 0; row < class_list_->count(); ++row)
        {
            QListWidgetItem*      item = class_list_->item(row);
            const reclass::Class* type = project_.class_at(static_cast<std::size_t>(row));
            if (item == nullptr || type == nullptr)
            {
                continue;
            }

            const QString                      expression = to_qstring(type->address_expression);
            const std::optional<std::uint64_t> address    = values_->class_address(static_cast<std::size_t>(row));
            item->setToolTip(address.has_value() ? tr("%1 → %2").arg(expression, values_->format_address(*address))
                                                 : tr("%1 → not resolved yet").arg(expression));
        }
    }

    void ReClassDialog::sync_expansion()
    {
        if (syncing_)
        {
            return;
        }
        syncing_ = true;
        for (const std::size_t node_id : structure_->expanded_nodes())
        {
            const QModelIndex index = structure_->index_for_node(node_id);
            if (index.isValid() && !grid_->isExpanded(index))
            {
                grid_->expand(index);
            }
        }
        syncing_ = false;
    }

    void ReClassDialog::update_actions()
    {
        const bool has_class = selected_class_exists();
        const bool has_node  = grid_->currentIndex().isValid();

        add_node_->setEnabled(has_class);
        delete_class_->setEnabled(has_class);
        delete_node_->setEnabled(has_node);
        delete_action_->setEnabled(has_node);
        dissect_->setEnabled(has_node && values_->attached());
        follow_->setEnabled(has_node);
    }

    void ReClassDialog::on_class_selected(int row)
    {
        if (row < 0)
        {
            return;
        }
        structure_->set_class(static_cast<std::size_t>(row));
        refresh_all();
    }

    void ReClassDialog::on_grid_expanded(const QModelIndex& index)
    {
        if (const models::ReclassRow* row = structure_->row_for(index); row != nullptr)
        {
            structure_->set_expanded(row->node_id, true);
            sync_expansion();
            update_actions();
        }
    }

    void ReClassDialog::on_grid_collapsed(const QModelIndex& index)
    {
        if (const models::ReclassRow* row = structure_->row_for(index); row != nullptr)
        {
            structure_->set_expanded(row->node_id, false);
            sync_expansion();
            update_actions();
        }
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

        const models::ReclassRow* row = structure_->row_for(grid_->currentIndex());

        reclass::Class* type =
            row == nullptr ? project_.class_at(structure_->selected_class()) : project_.class_at(row->class_index);
        if (type == nullptr)
        {
            return;
        }

        const auto        layout = reclass::layout_class(project_, *type);
        const std::size_t index  = row == nullptr ? type->nodes.size() : row->node_index;

        std::size_t offset = 0;
        if (layout.has_value() && index < layout->offsets.size())
        {
            offset = layout->offsets[index];
        }
        else if (layout.has_value())
        {
            // A node appended at the end sits at the class's total size.
            offset = layout->size;
        }

        reclass::Node node;
        node.id   = project_.next_node_id();
        node.kind = kind;
        node.name = reclass::default_node_name(kind, offset);
        type->nodes.insert(type->nodes.begin() + static_cast<std::ptrdiff_t>(index), std::move(node));

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

        reclass::Class* type = project_.class_at(row->class_index);
        if (type == nullptr || row->node_index >= type->nodes.size())
        {
            return;
        }

        reclass::change_node_kind(project_, type->nodes, row->node_index, kind);
        refresh_all();
        report(tr("Type changed to %1.").arg(kind_label(kind)), false);
    }

    void ReClassDialog::add_bytes()
    {
        if (!selected_class_exists())
        {
            report(tr("Select a class first."), true);
            return;
        }

        widgets::InputBoxOptions options;
        options.title    = tr("Add Bytes");
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
        if (!text.has_value())
        {
            return;
        }

        const std::size_t bytes = text->trimmed().toULongLong();
        reclass::append_bytes(project_, project_.classes()[structure_->selected_class()].nodes, bytes);
        refresh_all();
        report(tr("%1 byte(s) added.").arg(bytes), false);
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

        node->name = name->trimmed().toStdString();
        refresh_all();
        report(tr("Node renamed."), false);
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

        node->comment = comment->toStdString();
        refresh_all();
        report(tr("Comment saved."), false);
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
        std::vector<std::pair<std::size_t, std::size_t>> targets;
        for (const models::ReclassRow* row : selected_rows())
        {
            targets.emplace_back(row->class_index, row->node_index);
        }
        if (targets.empty())
        {
            report(tr("Select a node first."), true);
            return;
        }

        std::ranges::sort(targets, std::greater {});
        for (const auto& [class_index, node_index] : targets)
        {
            reclass::Class* type = project_.class_at(class_index);
            if (type != nullptr && node_index < type->nodes.size())
            {
                type->nodes.erase(type->nodes.begin() + static_cast<std::ptrdiff_t>(node_index));
            }
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
            if (node == nullptr || !reclass::is_hex_kind(node->kind))
            {
                continue;
            }

            const std::span<const std::byte>   bytes   = values_->node_bytes(*row);
            const std::optional<std::uint64_t> address = values_->node_address(*row);
            if (!address.has_value() || bytes.size() < reclass::kind_size(node->kind))
            {
                continue;
            }

            dissect_targets_.push_back(DissectTarget {.class_index = row->class_index,
                                                      .node_index  = row->node_index,
                                                      .kind        = node->kind,
                                                      .offset      = row->offset,
                                                      .address     = *address,
                                                      .bytes = std::vector<std::byte>(bytes.begin(), bytes.end())});
        }

        if (dissect_targets_.empty())
        {
            report(tr("Select a hex node whose bytes were read."), true);
            return;
        }

        // A pointer is only guessed for a value that can be dereferenced: a
        // module image answers that on its own, anything else costs one byte.
        std::vector<process::ReadManyItem> probes;
        for (const DissectTarget& target : dissect_targets_)
        {
            if (target.bytes.size() < 8)
            {
                continue;
            }
            std::uint64_t value = 0;
            std::memcpy(&value, target.bytes.data(), sizeof(value));
            if (value != 0 && !values_->inside_module(value))
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
                                                     return values_->inside_module(address)
                                                         || std::ranges::find(readable, address) != readable.end();
                                                 }};

            const auto guess = reclass::guess_kind(
                reclass::DissectInput {
                    .current = target.kind, .bytes = target.bytes, .offset = target.offset, .address = target.address},
                lookup);
            reclass::Class* type = project_.class_at(target.class_index);
            if (!guess.has_value() || type == nullptr || target.node_index >= type->nodes.size())
            {
                continue;
            }

            reclass::change_node_kind(project_, type->nodes, target.node_index, *guess);
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

        menu.exec(follow_->mapToGlobal(follow_->rect().bottomLeft()));
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
        if (!reclass::is_class_kind(node->kind) && node->kind != reclass::NodeKind::pointer)
        {
            report(tr("Select a pointer node first."), true);
            return;
        }

        const std::size_t node_id = row->node_id;

        if (class_name.isEmpty())
        {
            if (node->kind == reclass::NodeKind::class_instance)
            {
                report(tr("An instance needs a class; change its type to a pointer first."), true);
                return;
            }
            node->kind = reclass::NodeKind::pointer;
            node->class_name.clear();
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

        node->class_name = target->name;
        if (node->kind == reclass::NodeKind::pointer)
        {
            node->kind = reclass::NodeKind::class_pointer;
        }

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
        reclass::Class* type = project_.class_at(row->class_index);
        if (type == nullptr || row->node_index >= type->nodes.size())
        {
            return nullptr;
        }
        return &type->nodes[row->node_index];
    }

    void ReClassDialog::show_class_menu(const QPoint& position)
    {
        QMenu menu(this);
        widgets::show_explanations(menu);

        if (selected_class_exists())
        {
            QAction* rename = widgets::described_action(menu, tr("Rename Class…"), tr("Change the class's name."));
            connect(rename, &QAction::triggered, this, &ReClassDialog::rename_class);

            QAction* address = widgets::described_action(
                menu, tr("Set Class Address…"), tr("Change the expression the class is pinned to."));
            connect(address, &QAction::triggered, this, &ReClassDialog::set_class_address);

            menu.addSeparator();
            QAction* remove = widgets::described_action(
                menu,
                tr("Delete Class"),
                tr("Remove the class; nodes that reference it stay and read 'unknown class'."));
            connect(remove, &QAction::triggered, this, &ReClassDialog::delete_class);
        }
        else
        {
            (void)widgets::disabled_action(menu, tr("Rename Class…"), tr("Select a class first."));
        }

        menu.exec(class_list_->viewport()->mapToGlobal(position));
    }

    void ReClassDialog::show_row_menu(const QPoint& position)
    {
        QMenu menu(this);
        widgets::show_explanations(menu);

        QMenu* insert = widgets::explained_submenu(menu, tr("Insert Node"));
        populate_node_kind_menu(*insert, false);

        QMenu* change = widgets::explained_submenu(menu, tr("Change Type"));
        populate_node_kind_menu(*change, true);

        QAction* bytes =
            widgets::described_action(menu, tr("Add Bytes…"), tr("Append padding bytes at the end of the class."));
        connect(bytes, &QAction::triggered, this, &ReClassDialog::add_bytes);

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

        QAction* remove = widgets::described_action(menu, tr("Delete"), tr("Remove the selected node(s)."));
        widgets::show_shortcut_hint(*remove, *delete_action_);
        connect(remove, &QAction::triggered, this, &ReClassDialog::remove_selected_nodes);

        menu.exec(grid_->viewport()->mapToGlobal(position));
    }

}
