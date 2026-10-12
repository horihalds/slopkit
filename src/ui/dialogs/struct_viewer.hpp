#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "script/symbols.hpp"
#include "structs/definition.hpp"
#include "ui/address_format.hpp"
#include "ui/components/struct_document.hpp"
#include "ui/live_values.hpp"

#include <QDialog>
#include <QString>

class QAction;
class QModelIndex;
class QShowEvent;
class QHideEvent;
class QPlainTextEdit;
class QTableView;

namespace slopkit::ui::widgets
{
    class StatusLabel;
}

namespace slopkit::ui::dialogs
{
    class AddFieldDialog;
}

namespace slopkit::ui::models
{
    class StructModel;
}

namespace slopkit::ui::dialogs
{
    // The detached Struct View window: a definition editor over a live target,
    // with a read-only C++ preview of the reconstructed layout. It is a
    // `ui::LiveSurface`, so its reads ride the shared live pass.
    class StructViewerDialog : public QDialog, public ui::LiveSurface
    {
        Q_OBJECT

    public:
        StructViewerDialog(process::AccessWorker&   worker,
                           process::AttachedTarget& target,
                           structs::Structs&        definitions,
                           QWidget*                 parent  = nullptr,
                           script::SymbolTable&     symbols = script::default_symbol_table());

        void refresh_definitions();
        void refresh_preview();

        void set_modules(std::vector<process::ModuleInfo> modules);

        void set_address_mode(ui::AddressMode mode);

        // Opens a new struct based at `address`, seeded from a row menu command.
        void dissect(std::uint64_t address);

        [[nodiscard]] std::vector<ui::LiveRequest> next_live_request() override;
        void apply_live_readings(std::span<const ui::LiveReading> readings) override;

        [[nodiscard]] models::StructModel*  model() const noexcept;
        [[nodiscard]] QTableView*           table_view() const noexcept;
        [[nodiscard]] QPlainTextEdit*       preview() const noexcept;
        [[nodiscard]] widgets::StatusLabel* status_label() const noexcept;
        [[nodiscard]] QAction*              add_struct_action() const noexcept;
        [[nodiscard]] QAction*              add_field_action() const noexcept;
        [[nodiscard]] QAction*              remove_action() const noexcept;
        [[nodiscard]] QAction*              hex_action() const noexcept;
        [[nodiscard]] QAction*              follow_action() const noexcept;
        [[nodiscard]] QAction*              unfollow_action() const noexcept;
        [[nodiscard]] QAction*              copy_action() const noexcept;
        [[nodiscard]] QAction*              export_action() const noexcept;

    signals:
        void statusChanged(const QString& message, bool is_error);

    protected:
        void showEvent(QShowEvent* event) override;
        void hideEvent(QHideEvent* event) override;

    private:
        void build_layout();
        void update_command_state(const QModelIndex& current);

        void on_add_struct();
        void on_add_field();
        void on_remove();
        void on_show_hex();
        void on_follow();
        void on_unfollow();
        void on_copy_cpp();
        void on_export_cpp();
        void on_type_activated(const QModelIndex& index);

        void add_struct_at(const QString& name, const QString& expression);
        void edit_field(std::size_t row);
        void materialize_children(std::uint64_t parent, const structs::FieldType& type, int budget);

        [[nodiscard]] std::optional<std::size_t> selected_row() const;
        void                                     select_id(std::uint64_t id);

        structs::Structs&          definitions_;
        components::StructDocument document_;
        models::StructModel*       model_ {};
        QTableView*                view_ {};
        QPlainTextEdit*            preview_ {};
        AddFieldDialog*            field_dialog_ {};
        widgets::StatusLabel*      status_ {};
        QAction*                   add_struct_action_ {};
        QAction*                   add_field_action_ {};
        QAction*                   remove_action_ {};
        QAction*                   hex_action_ {};
        QAction*                   follow_action_ {};
        QAction*                   unfollow_action_ {};
        QAction*                   copy_action_ {};
        QAction*                   export_action_ {};
    };

}
