#pragma once

#include <cstddef>
#include <cstdint>
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
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTimer;

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // The Add Address dialog: description, address expression, value type, the
    // dynamic types' size and the hex toggle. It appends a table::AddressEntry;
    // a pointer-chain expression is resolved through the access worker before
    // the entry is added, a deref-free one resolves synchronously. While the
    // user edits, a one-shot debounce resolves the text and previews the
    // address and the value read there.
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
        enum class PreviewState
        {
            idle,
            resolving,
            resolved,
            unresolved,
        };

        // The live preview of the typed expression: the text it describes, its
        // resolution and the value read at the resolved address.
        struct Preview
        {
            std::string                           text; // the text this result describes
            PreviewState                          state {PreviewState::idle};
            std::uint64_t                         address {};  // valid when state == resolved
            std::optional<std::vector<std::byte>> value;       // the last value read, if any
            std::string                           reason;      // why the text is unresolved
            std::optional<process::JobId>         resolve_job; // in flight
            std::optional<process::JobId>         read_job;    // in flight
        };

        void commit();
        void reject_address(const QString& message);
        void on_address_changed();
        void resolve_now(const std::string& text);
        void finish_resolve(process::JobId id, const std::string& text, process::JobResult&& result);
        void set_preview_resolved(const std::string& text, std::uint64_t address);
        void set_preview_unresolved(const std::string& text, const QString& reason);
        void request_value_read(std::uint64_t address, std::size_t size);
        void apply_value_read(process::JobId id, process::JobResult&& result);
        [[nodiscard]] std::optional<std::size_t> preview_value_size() const;
        void                                     refresh_preview();
        void                                     clear_preview();
        void                                     add_entry(std::uint64_t address, const std::string& expression);
        void                                     update_size_row();
        void                                     reset_form();

        table::AddressTable&   table_;
        process::AccessWorker& worker_;
        script::SymbolTable&   symbols_;
        ui::ModuleSpans        spans_;

        Preview                    preview_;
        std::optional<std::string> pending_add_; // add this text once its resolve lands

        QLineEdit*            description_edit_ {};
        QLineEdit*            address_edit_ {};
        QComboBox*            type_combo_ {};
        QLineEdit*            size_edit_ {};
        QWidget*              size_row_ {};
        QCheckBox*            hex_check_ {};
        QPushButton*          add_button_ {};
        QLabel*               preview_label_ {};
        QTimer*               preview_timer_ {};
        widgets::StatusLabel* status_ {};
    };

} // namespace slopkit::ui::dialogs
