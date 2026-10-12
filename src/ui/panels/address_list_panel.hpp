#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "debug/breakpoints.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "table/address_table.hpp"
#include "ui/address_format.hpp"
#include "ui/live_values.hpp"
#include "ui/models/copy_format.hpp"

#include <QString>
#include <QWidget>

class QAction;
class QPoint;
class QMenu;
class QTableView;

namespace slopkit::ui::dialogs
{
    class AddScriptDialog;
}

namespace slopkit::ui::models
{
    class AddressTableModel;
}

namespace slopkit::ui::panels
{
    class AddressListPanel : public QWidget, public ui::LiveSurface
    {
        Q_OBJECT

    public:
        AddressListPanel(table::AddressTable&     table,
                         process::AccessWorker&   worker,
                         process::AttachedTarget& target,
                         QWidget*                 parent = nullptr);

        void save_table();
        void save_table_as();
        void delete_selected();
        void toggle_active_selected();

        // The row menu's `Change value`: asks once, in the shared input box, for
        // a value and writes it to every selected value row. The rows must share
        // a type and Hex setting; the model refuses a mixed selection.
        void change_value();

        [[nodiscard]] std::optional<QString> choose_table_path();

        // The start path of the Save Table As dialog: the current table file
        // when the table has one (only its directory can change), otherwise
        // `<target name>.skt` in the remembered directory, or in the default
        // table directory when nothing is remembered. Pure: it never touches the
        // filesystem.
        [[nodiscard]] QString suggested_table_path() const;

        using SavePathPrompt = std::function<std::optional<QString>(QWidget* parent, const QString& suggested)>;
        void set_save_path_prompt(SavePathPrompt prompt);

        [[nodiscard]] std::optional<table::AddressTable> parse_table(const QString& path);

        void adopt_table(const QString& path, table::AddressTable&& loaded);

        table::MergeSummary merge_table(const table::AddressTable& incoming);

        [[nodiscard]] bool load_table(const QString& path);

        void report_status(std::string_view message, bool is_error);

        void report_active_error(std::string_view message);

        void note_script_update_failed(const process::ScriptUpdateFailure& failure);

        void refresh();

        void note_entry_edited(std::size_t row);

        void set_modules(std::vector<process::ModuleInfo> modules);

        void set_address_mode(ui::AddressMode mode);

        void set_table_path(const QString& path);

        [[nodiscard]] QString table_path() const;

        void set_dialog_directory(const QString& directory);

        [[nodiscard]] QWidget* tab_order_first() const noexcept;

        [[nodiscard]] std::vector<ui::LiveRequest> next_live_request() override;
        void apply_live_readings(std::span<const ui::LiveReading> readings) override;

        void populate_row_menu(QMenu& menu, std::size_t row);

        void populate_panel_menu(QMenu& menu);

        void populate_context_menu(QMenu& menu, const QPoint& position);

        void add_script();
        void add_hook_script(const QString& description, const QString& source);

        void edit_script(std::size_t row);

        void run_script(std::size_t row);

    signals:
        void browseRequested(quint64 address);

        void tableLoaded();

        void tablePathChanged(const QString& path);

        void statusChanged(const QString& message, bool is_error);

        void accessWatchRequested(std::uint64_t address, std::size_t width, slopkit::debug::Kind kind);

        void addAddressRequested();

        void editEntryRequested(std::size_t row);

        void tableSettingsRequested();

    private:
        void                                   show_context_menu(const QPoint& position);
        void                                   set_status(const QString& message, bool is_error);
        void                                   save_to_path(const QString& path);
        [[nodiscard]] std::vector<std::size_t> command_rows() const;
        void                                   copy_row(std::size_t row, models::CopyFormat format);
        void                                   sync_view_selection();
        static std::optional<QString>          default_save_path_prompt(QWidget* parent, const QString& suggested);
        void                                   maybe_resolve_expressions();

        void ensure_script_dialog();
        void finish_script(process::JobId id, process::JobResult&& result);
        void toggle_script_active(std::size_t row, bool wanted);
        void finish_script_active(process::JobId id, std::uint64_t entry_id, bool wanted, process::JobResult&& result);

        table::AddressTable&                  table_;
        process::AccessWorker&                worker_;
        process::AttachedTarget&              target_;
        ui::ModuleSpans                       modules_;
        models::AddressTableModel*            model_ {};
        QTableView*                           table_view_ {};
        QAction*                              remove_action_ {};
        QAction*                              change_value_action_ {};
        bool                                  syncing_ {false};
        std::optional<process::JobId>         resolve_job_;
        std::optional<process::JobId>         script_job_;
        dialogs::AddScriptDialog*             script_dialog_ {};
        bool                                  resolving_ {false};
        bool                                  force_resolve_ {true};
        std::chrono::steady_clock::time_point last_resolve_ {};
        std::string                           expression_signature_;

        QString        table_path_;
        QString        dialog_directory_;
        SavePathPrompt save_path_prompt_ {&AddressListPanel::default_save_path_prompt};
        QString        status_;
        bool           status_is_error_ {false};
    };

}
