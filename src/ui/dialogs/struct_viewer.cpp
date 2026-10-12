#include "ui/dialogs/struct_viewer.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "structs/export.hpp"
#include "ui/components/indent_delegate.hpp"
#include "ui/components/input_box.hpp"
#include "ui/components/widgets.hpp"
#include "ui/dialogs/add_field.hpp"
#include "ui/fonts.hpp"
#include "ui/models/struct_model.hpp"
#include "ui/text.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

namespace slopkit::ui::dialogs
{
    namespace
    {
        constexpr int kMaterializeDepth = 16;
    }

    StructViewerDialog::StructViewerDialog(process::AccessWorker&   worker,
                                           process::AttachedTarget& target,
                                           structs::Structs&        definitions,
                                           QWidget*                 parent,
                                           script::SymbolTable&     symbols)
        : QDialog(parent), definitions_(definitions), document_(worker, target, definitions, this, symbols)
    {
        setWindowTitle(tr("Struct View"));
        setObjectName(QStringLiteral("struct_viewer"));
        resize(880, 560);
        setMinimumSize(560, 360);
        // Built without a parent (docs/UI_DESIGN.md#wayland) so the compositor
        // stacks it like any other window; it must not quit the app on close.
        setAttribute(Qt::WA_QuitOnClose, false);

        build_layout();

        connect(&document_,
                &components::StructDocument::valuesChanged,
                this,
                [this]
                {
                    model_->notify_values_changed();
                });
        connect(&document_, &components::StructDocument::statusChanged, this, &StructViewerDialog::statusChanged);

        refresh_preview();
    }

    void StructViewerDialog::build_layout()
    {
        model_ = new models::StructModel(definitions_, document_, this);
        connect(model_, &models::StructModel::statusChanged, this, &StructViewerDialog::statusChanged);

        field_dialog_ = new AddFieldDialog(this);

        view_ = new QTableView(this);
        view_->setObjectName(QStringLiteral("struct_table"));
        view_->setModel(model_);
        view_->setItemDelegate(new widgets::IndentDelegate(view_));
        view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        view_->setSelectionMode(QAbstractItemView::SingleSelection);
        view_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
        view_->verticalHeader()->setVisible(false);
        view_->horizontalHeader()->setStretchLastSection(true);
        view_->setColumnWidth(models::StructModel::offset, 80);
        view_->setColumnWidth(models::StructModel::type, 140);
        view_->setColumnWidth(models::StructModel::name, 200);

        preview_ = new QPlainTextEdit(this);
        preview_->setObjectName(QStringLiteral("struct_cpp_preview"));
        preview_->setReadOnly(true);
        preview_->setFont(mono_font());
        preview_->setPlaceholderText(tr("Add a struct to see its C++ declaration."));

        auto* splitter = new QSplitter(Qt::Vertical, this);
        splitter->addWidget(view_);
        splitter->addWidget(preview_);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 2);

        add_struct_action_ = new QAction(tr("Add Struct…"), this);
        add_struct_action_->setObjectName(QStringLiteral("struct_add_struct"));
        add_field_action_ = new QAction(tr("Add Field…"), this);
        add_field_action_->setObjectName(QStringLiteral("struct_add_field"));
        remove_action_ = new QAction(tr("Remove"), this);
        remove_action_->setObjectName(QStringLiteral("struct_remove"));
        hex_action_ = new QAction(tr("Show as hex"), this);
        hex_action_->setObjectName(QStringLiteral("struct_show_hex"));
        hex_action_->setCheckable(true);
        follow_action_ = new QAction(tr("Follow"), this);
        follow_action_->setObjectName(QStringLiteral("struct_follow"));
        unfollow_action_ = new QAction(tr("Unfollow"), this);
        unfollow_action_->setObjectName(QStringLiteral("struct_unfollow"));
        copy_action_ = new QAction(tr("Copy C++"), this);
        copy_action_->setObjectName(QStringLiteral("struct_copy_cpp"));
        export_action_ = new QAction(tr("Export C++…"), this);
        export_action_->setObjectName(QStringLiteral("struct_export_cpp"));

        remove_action_->setEnabled(false);
        hex_action_->setEnabled(false);
        follow_action_->setEnabled(false);
        unfollow_action_->setEnabled(false);

        auto* toolbar = new QToolBar(this);
        toolbar->setObjectName(QStringLiteral("struct_toolbar"));
        toolbar->addAction(add_struct_action_);
        toolbar->addAction(add_field_action_);
        toolbar->addAction(remove_action_);
        toolbar->addSeparator();
        toolbar->addAction(hex_action_);
        toolbar->addAction(follow_action_);
        toolbar->addAction(unfollow_action_);
        toolbar->addSeparator();
        toolbar->addAction(copy_action_);
        toolbar->addAction(export_action_);

        status_ = new widgets::StatusLabel(this);
        status_->setObjectName(QStringLiteral("struct_status"));

        auto* layout = new QVBoxLayout(this);
        layout->addWidget(toolbar);
        layout->addWidget(splitter, 1);
        layout->addWidget(status_);

        connect(add_struct_action_, &QAction::triggered, this, &StructViewerDialog::on_add_struct);
        connect(add_field_action_, &QAction::triggered, this, &StructViewerDialog::on_add_field);
        connect(remove_action_, &QAction::triggered, this, &StructViewerDialog::on_remove);
        connect(hex_action_, &QAction::triggered, this, &StructViewerDialog::on_show_hex);
        connect(follow_action_, &QAction::triggered, this, &StructViewerDialog::on_follow);
        connect(unfollow_action_, &QAction::triggered, this, &StructViewerDialog::on_unfollow);
        connect(copy_action_, &QAction::triggered, this, &StructViewerDialog::on_copy_cpp);
        connect(export_action_, &QAction::triggered, this, &StructViewerDialog::on_export_cpp);
        connect(view_, &QTableView::doubleClicked, this, &StructViewerDialog::on_type_activated);
        connect(view_->selectionModel(),
                &QItemSelectionModel::currentRowChanged,
                this,
                [this](const QModelIndex& current)
                {
                    update_command_state(current);
                });
    }

    void StructViewerDialog::update_command_state(const QModelIndex& current)
    {
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        const std::size_t                          row   = static_cast<std::size_t>(current.row());
        const bool                                 valid = current.isValid() && row < nodes.size();
        remove_action_->setEnabled(valid);
        hex_action_->setEnabled(valid && nodes[row].parent != 0);
        if (!valid)
        {
            follow_action_->setEnabled(false);
            unfollow_action_->setEnabled(false);
            return;
        }
        const std::uint64_t id = nodes[row].id;
        follow_action_->setEnabled(!document_.followed(id) && document_.can_follow(id));
        unfollow_action_->setEnabled(document_.followed(id));
        hex_action_->setChecked(nodes[row].hex);
    }

    std::optional<std::size_t> StructViewerDialog::selected_row() const
    {
        const QModelIndex current = view_->currentIndex();
        return current.isValid() ? std::optional<std::size_t> {static_cast<std::size_t>(current.row())} : std::nullopt;
    }

    void StructViewerDialog::select_id(std::uint64_t id)
    {
        const std::size_t row = definitions_.index_of(id);
        if (row < definitions_.size())
        {
            view_->setCurrentIndex(model_->index(static_cast<int>(row), models::StructModel::name));
        }
    }

    void StructViewerDialog::refresh_definitions()
    {
        log::debug(log::category::ui, std::format("struct view: {} node(s)", definitions_.size()));
        model_->refresh();
        document_.refresh();
        refresh_preview();
    }

    void StructViewerDialog::refresh_preview()
    {
        preview_->setPlainText(to_qstring(structs::render_cpp(definitions_, document_.pointer_size())));
    }

    void StructViewerDialog::set_modules(std::vector<process::ModuleInfo> modules)
    {
        document_.set_modules(std::move(modules));
        model_->notify_values_changed();
    }

    void StructViewerDialog::set_address_mode(ui::AddressMode mode)
    {
        document_.set_address_mode(mode);
        model_->notify_values_changed();
    }

    void StructViewerDialog::add_struct_at(const QString& name, const QString& expression)
    {
        const QString trimmed_name = name.trimmed();
        const QString trimmed_expr = expression.trimmed();
        if (trimmed_name.isEmpty() || trimmed_expr.isEmpty())
        {
            return;
        }
        const std::uint64_t id = definitions_.add_struct(trimmed_name.toStdString(), trimmed_expr.toStdString());
        if (id == 0)
        {
            log::warning(log::category::ui, "struct view: the struct could not be added");
            return;
        }
        document_.refresh();
        refresh_definitions();
        select_id(id);
        log::info(log::category::ui, std::format("struct '{}' added", trimmed_name.toStdString()));
    }

    void StructViewerDialog::on_add_struct()
    {
        widgets::InputBoxOptions name_options;
        name_options.title   = tr("Add Struct");
        name_options.label   = tr("Name");
        name_options.initial = tr("Struct");
        name_options.ok_text = tr("Next");
        const auto name      = widgets::get_text(name_options, this);
        if (!name.has_value())
        {
            return;
        }

        widgets::InputBoxOptions expression_options;
        expression_options.title       = tr("Add Struct");
        expression_options.label       = tr("Base address expression");
        expression_options.placeholder = tr("module+0x10, a symbol, or [ptr]+0x8");
        expression_options.ok_text     = tr("Add");
        const auto expression          = widgets::get_text(expression_options, this);
        if (!expression.has_value())
        {
            return;
        }
        add_struct_at(*name, *expression);
    }

    void StructViewerDialog::dissect(std::uint64_t address)
    {
        const QString expression = QStringLiteral("0x%1").arg(QString::number(address, 16).toUpper());

        widgets::InputBoxOptions options;
        options.title   = tr("Dissect as struct");
        options.label   = tr("Name");
        options.initial = tr("Struct_%1").arg(QString::number(address, 16).toUpper());
        options.ok_text = tr("Add");
        const auto name = widgets::get_text(options, this);
        if (!name.has_value())
        {
            return;
        }
        add_struct_at(*name, expression);
        show();
        raise();
        activateWindow();
    }

    void StructViewerDialog::materialize_children(std::uint64_t parent, const structs::FieldType& type, int budget)
    {
        if (budget <= 0 || (type.kind != structs::FieldKind::structure && type.kind != structs::FieldKind::pointer)
            || type.structure.empty())
        {
            return;
        }
        const structs::StructNode* target = definitions_.find_struct(type.structure);
        if (target == nullptr)
        {
            return;
        }

        const std::size_t                          depth = definitions_.depth_of(definitions_.index_of(target->id));
        std::vector<structs::StructNode>           children;
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        std::size_t                                end   = definitions_.index_of(target->id) + 1;
        while (end < nodes.size() && definitions_.depth_of(end) > depth)
        {
            if (definitions_.depth_of(end) == depth + 1)
            {
                children.push_back(nodes[end]);
            }
            ++end;
        }

        for (const structs::StructNode& child : children)
        {
            const std::uint64_t id = definitions_.add_field(parent, child.name, child.offset, child.type);
            if (id != 0)
            {
                materialize_children(id, child.type, budget - 1);
            }
        }
    }

    void StructViewerDialog::on_add_field()
    {
        const auto row = selected_row();
        if (!row.has_value())
        {
            emit statusChanged(tr("Select a struct or structure first."), true);
            return;
        }
        const std::uint64_t        parent = model_->id_at(*row);
        const structs::StructNode* node   = definitions_.find(parent);
        if (node == nullptr)
        {
            return;
        }
        field_dialog_->begin_add(definitions_, tr("field"), 0);
        if (field_dialog_->exec() != QDialog::Accepted)
        {
            return;
        }
        const auto draft = field_dialog_->draft();
        if (!draft.has_value())
        {
            return;
        }
        const std::uint64_t id = definitions_.add_field(parent, draft->name.toStdString(), draft->offset, draft->type);
        if (id == 0)
        {
            emit statusChanged(tr("The field could not be added."), true);
            return;
        }
        materialize_children(id, draft->type, kMaterializeDepth);
        document_.refresh();
        refresh_definitions();
        select_id(id);
        emit statusChanged(tr("Field added."), false);
    }

    void StructViewerDialog::edit_field(std::size_t row)
    {
        const std::uint64_t        id   = model_->id_at(row);
        const structs::StructNode* node = definitions_.find(id);
        if (node == nullptr)
        {
            return;
        }
        field_dialog_->begin_edit(definitions_, *node);
        if (field_dialog_->exec() != QDialog::Accepted)
        {
            return;
        }
        const auto draft = field_dialog_->draft();
        if (!draft.has_value())
        {
            return;
        }
        definitions_.rename(id, draft->name.toStdString());
        definitions_.set_offset(id, draft->offset);
        definitions_.set_type(id, draft->type);
        if (const structs::StructNode* updated = definitions_.find(id);
            updated != nullptr
            && (updated->type.kind == structs::FieldKind::structure
                || updated->type.kind == structs::FieldKind::pointer))
        {
            // Only fill an empty subtree; an already-expanded pointer keeps its shape.
            const std::size_t index = definitions_.index_of(id);
            const bool        empty =
                index + 1 >= definitions_.size() || definitions_.depth_of(index + 1) <= definitions_.depth_of(index);
            if (empty)
            {
                materialize_children(id, updated->type, kMaterializeDepth);
            }
        }
        document_.refresh();
        refresh_definitions();
        select_id(id);
        emit statusChanged(tr("Field updated."), false);
    }

    void StructViewerDialog::on_type_activated(const QModelIndex& index)
    {
        if (index.isValid() && index.column() == models::StructModel::type)
        {
            edit_field(static_cast<std::size_t>(index.row()));
        }
    }

    void StructViewerDialog::on_remove()
    {
        const auto row = selected_row();
        if (!row.has_value())
        {
            return;
        }
        const std::uint64_t id = model_->id_at(*row);
        if (!definitions_.remove(id))
        {
            return;
        }
        document_.refresh();
        refresh_definitions();
        emit statusChanged(tr("Removed."), false);
    }

    void StructViewerDialog::on_show_hex()
    {
        const auto row = selected_row();
        if (!row.has_value())
        {
            return;
        }
        const std::uint64_t        id   = model_->id_at(*row);
        const structs::StructNode* node = definitions_.find(id);
        if (node == nullptr || node->parent == 0)
        {
            return;
        }
        definitions_.set_hex(id, !node->hex);
        model_->notify_values_changed();
        document_.refresh();
    }

    void StructViewerDialog::on_follow()
    {
        const auto row = selected_row();
        if (!row.has_value())
        {
            return;
        }
        document_.set_followed(model_->id_at(*row), true);
        model_->notify_values_changed();
        refresh_preview();
        update_command_state(view_->currentIndex());
    }

    void StructViewerDialog::on_unfollow()
    {
        const auto row = selected_row();
        if (!row.has_value())
        {
            return;
        }
        document_.set_followed(model_->id_at(*row), false);
        model_->notify_values_changed();
        refresh_preview();
        update_command_state(view_->currentIndex());
    }

    void StructViewerDialog::on_copy_cpp()
    {
        QGuiApplication::clipboard()->setText(preview_->toPlainText());
        emit statusChanged(tr("Copied the C++ declaration."), false);
    }

    void StructViewerDialog::on_export_cpp()
    {
        const QString path =
            QFileDialog::getSaveFileName(this, tr("Export C++"), QStringLiteral("structs.h"), tr("C++ header (*.h)"));
        if (path.isEmpty())
        {
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            log::warning(log::category::ui, std::format("struct view: cannot write {}", path.toStdString()));
            emit statusChanged(tr("Export failed."), true);
            return;
        }
        file.write(preview_->toPlainText().toUtf8());
        file.close();
        log::info(log::category::ui, std::format("struct view: exported to {}", path.toStdString()));
        emit statusChanged(tr("Exported to %1").arg(path), false);
    }

    std::vector<ui::LiveRequest> StructViewerDialog::next_live_request()
    {
        return document_.next_live_request();
    }

    void StructViewerDialog::apply_live_readings(std::span<const ui::LiveReading> readings)
    {
        document_.apply_live_readings(readings);
    }

    void StructViewerDialog::showEvent(QShowEvent* event)
    {
        document_.set_visible(true);
        document_.refresh();
        QDialog::showEvent(event);
    }

    void StructViewerDialog::hideEvent(QHideEvent* event)
    {
        document_.set_visible(false);
        QDialog::hideEvent(event);
    }

    models::StructModel* StructViewerDialog::model() const noexcept
    {
        return model_;
    }

    QTableView* StructViewerDialog::table_view() const noexcept
    {
        return view_;
    }

    QPlainTextEdit* StructViewerDialog::preview() const noexcept
    {
        return preview_;
    }

    widgets::StatusLabel* StructViewerDialog::status_label() const noexcept
    {
        return status_;
    }

    QAction* StructViewerDialog::add_struct_action() const noexcept
    {
        return add_struct_action_;
    }

    QAction* StructViewerDialog::add_field_action() const noexcept
    {
        return add_field_action_;
    }

    QAction* StructViewerDialog::remove_action() const noexcept
    {
        return remove_action_;
    }

    QAction* StructViewerDialog::hex_action() const noexcept
    {
        return hex_action_;
    }

    QAction* StructViewerDialog::follow_action() const noexcept
    {
        return follow_action_;
    }

    QAction* StructViewerDialog::unfollow_action() const noexcept
    {
        return unfollow_action_;
    }

    QAction* StructViewerDialog::copy_action() const noexcept
    {
        return copy_action_;
    }

    QAction* StructViewerDialog::export_action() const noexcept
    {
        return export_action_;
    }

}
