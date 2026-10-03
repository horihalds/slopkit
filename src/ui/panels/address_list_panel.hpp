#pragma once

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

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

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
        void open_table();
        void save_table();
        void save_table_as();
        void delete_selected();
        void toggle_freeze_selected();

        // Reports a freeze failure raised outside the panel.
        void report_freeze_error(std::string_view message);

        // Polled by the window's tick.
        void refresh();

        // Sets the module image spans used to render module-relative addresses.
        void set_modules(std::vector<process::ModuleInfo> modules);

        // Chooses how static addresses are shown in the Address column.
        void set_address_mode(ui::AddressMode mode);

    signals:
        // A request to show an address, fed by the context menu.
        void browseRequested(quint64 address);

    private:
        void show_context_menu(const QPoint& position);
        void set_status(const QString& message, bool is_error);
        void save_to_path(const QString& path);

        table::AddressTable&       table_;
        models::AddressTableModel* model_ {};
        QTableView*                table_view_ {};
        widgets::StatusLabel*      status_label_ {};

        // The pre-filled path of the open/save dialogs.
        QString table_path_;
        QString status_;
        bool    status_is_error_ {false};
    };

} // namespace slopkit::ui::panels
