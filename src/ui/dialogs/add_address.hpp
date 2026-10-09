#pragma once

#include <optional>
#include <string>
#include <vector>

#include "process/access_worker.hpp"
#include "process/types.hpp"
#include "script/symbols.hpp"
#include "table/address_table.hpp"
#include "ui/address_format.hpp"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QShowEvent;

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // The Add Address dialog: description, address expression, value type, the
    // dynamic types' size and the hex toggle. It appends a table::AddressEntry;
    // a pointer-chain expression is resolved through the access worker before
    // the entry is added, a deref-free one resolves synchronously.
    class AddAddressDialog : public QDialog
    {
        Q_OBJECT

    public:
        AddAddressDialog(table::AddressTable&   table,
                         process::AccessWorker& worker,
                         QWidget*               parent  = nullptr,
                         script::SymbolTable&   symbols = script::default_symbol_table());

        // The module images address expressions resolve against.
        void set_modules(std::vector<process::ModuleInfo> modules);

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        void commit();
        void reject_address(const QString& message);
        void begin_resolve(std::string expression);
        void finish_resolve(process::JobId id, const std::string& expression, process::JobResult&& result);
        void add_entry(std::uint64_t address, const std::string& expression);
        void update_size_row();
        void reset_form();

        table::AddressTable&          table_;
        process::AccessWorker&        worker_;
        script::SymbolTable&          symbols_;
        ui::ModuleSpans               spans_;
        std::optional<process::JobId> resolve_job_;

        QLineEdit*            description_edit_ {};
        QLineEdit*            address_edit_ {};
        QComboBox*            type_combo_ {};
        QLineEdit*            size_edit_ {};
        QWidget*              size_row_ {};
        QCheckBox*            hex_check_ {};
        QPushButton*          add_button_ {};
        widgets::StatusLabel* status_ {};
    };

} // namespace slopkit::ui::dialogs
