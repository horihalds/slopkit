#pragma once

#include <optional>
#include <string_view>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "table/address_table.hpp"
#include "ui/address_format.hpp"

#include <QString>
#include <QWidget>

class QPoint;
class QTableView;

namespace slopkit::ui::models
{
    class AddressTableModel;
} // namespace slopkit::ui::models

namespace slopkit::ui::panels
{

    // The bottom zone: the editable address list with its context menu. It owns
    // no session; edits are encoded by the model and the writes are submitted to
    // the access worker.
    class AddressListPanel : public QWidget
    {
        Q_OBJECT

    public:
        AddressListPanel(table::AddressTable&     table,
                         process::AccessWorker&   worker,
                         process::AttachedTarget& target,
                         QWidget*                 parent = nullptr);

        // Entry points used by the menu bar and the toolbar.
        void save_table();
        void save_table_as();
        void delete_selected();
        void toggle_freeze_selected();

        // Runs the native file dialog only and returns the chosen path, or
        // nullopt when the dialog was cancelled. Split out of load_table() so
        // tests can drive a load without the native file dialog.
        [[nodiscard]] std::optional<QString> choose_table_path();

        // Parses a table file into a scratch table without touching the panel
        // state; reports a failure the same way load_table() does.
        [[nodiscard]] std::optional<table::AddressTable> parse_table(const QString& path);

        // Replaces the panel's table with a parsed one, remembers the path and
        // reports the outcome; emits tableLoaded().
        void adopt_table(const QString& path, table::AddressTable&& loaded);

        // Appends the incoming entries that are not already present, keeps the
        // current path and settings and reports the outcome without emitting
        // tableLoaded().
        table::MergeSummary merge_table(const table::AddressTable& incoming);

        // Loads a table file into the panel's table (parse_table + adopt_table).
        [[nodiscard]] bool load_table(const QString& path);

        // Reports a status line outcome; report_freeze_error() forwards to it.
        void report_status(std::string_view message, bool is_error);

        // Reports a freeze failure raised outside the panel.
        void report_freeze_error(std::string_view message);

        // Polled by the window's tick.
        void refresh();

        // Sets the module image spans used to render module-relative addresses.
        void set_modules(std::vector<process::ModuleInfo> modules);

        // Chooses how static addresses are shown in the Address column.
        void set_address_mode(ui::AddressMode mode);

        // Seeds the remembered table path; an unchanged value is not reported.
        void set_table_path(const QString& path);

        // The table slopkit last loaded or saved.
        [[nodiscard]] QString table_path() const;

        // Sets the directory the file dialogs start in.
        void set_dialog_directory(const QString& directory);

        // The address list's only focusable control; the window joins its own
        // chain onto it, so no panel-local chain call is needed.
        [[nodiscard]] QWidget* tab_order_first() const noexcept; // the address list

    signals:
        // A request to show an address, fed by the context menu.
        void browseRequested(quint64 address);

        // A table file was loaded into the panel's table.
        void tableLoaded();

        // The remembered table path changed on a load or a save, so the window
        // can persist it.
        void tablePathChanged(const QString& path);

        // A status line outcome for the window's status area; emitted only when
        // the text or its kind actually changes.
        void statusChanged(const QString& message, bool is_error);

    private:
        void show_context_menu(const QPoint& position);
        void set_status(const QString& message, bool is_error);
        void save_to_path(const QString& path);

        table::AddressTable&       table_;
        models::AddressTableModel* model_ {};
        QTableView*                table_view_ {};

        // The pre-filled path of the open/save dialogs.
        QString table_path_;
        // The directory the file dialogs start in.
        QString dialog_directory_;
        // The last status emitted, so a repeated report is not re-emitted.
        QString status_;
        bool    status_is_error_ {false};
    };

} // namespace slopkit::ui::panels
