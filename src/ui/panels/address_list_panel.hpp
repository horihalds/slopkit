#pragma once

#include <chrono>
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

#include <QString>
#include <QWidget>

class QPoint;
class QMenu;
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
    class AddressListPanel : public QWidget, public ui::LiveSurface
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

        // LiveSurface: the rows of the address list, delegated to the model.
        [[nodiscard]] std::vector<ui::LiveRequest> next_live_request() override;
        void apply_live_readings(std::span<const ui::LiveReading> readings) override;

        // Adds the per-row entries for `row` to `menu`, wiring each to its
        // action; split out of show_context_menu so a test can inspect it
        // without running the modal menu.
        void populate_row_menu(QMenu& menu, std::size_t row);

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

        // A request to arm an access watch on an address, from the row menu.
        void accessWatchRequested(std::uint64_t address, std::size_t width, slopkit::debug::Kind kind);

    private:
        void show_context_menu(const QPoint& position);
        void set_status(const QString& message, bool is_error);
        void save_to_path(const QString& path);
        // Re-resolves every entry that carries an expression, at most once per
        // interval unless the expression set or the module map changed.
        void maybe_resolve_expressions();

        table::AddressTable&                  table_;
        process::AccessWorker&                worker_;
        process::AttachedTarget&              target_;
        ui::ModuleSpans                       modules_;
        models::AddressTableModel*            model_ {};
        QTableView*                           table_view_ {};
        std::optional<process::JobId>         resolve_job_;
        bool                                  resolving_ {false};
        bool                                  force_resolve_ {true};
        std::chrono::steady_clock::time_point last_resolve_ {};
        // The ids and expression texts of the last batch, so a changed set of
        // expressions resolves at once instead of waiting for the interval.
        std::string                           expression_signature_;

        // The pre-filled path of the open/save dialogs.
        QString table_path_;
        // The directory the file dialogs start in.
        QString dialog_directory_;
        // The last status emitted, so a repeated report is not re-emitted.
        QString status_;
        bool    status_is_error_ {false};
    };

} // namespace slopkit::ui::panels
