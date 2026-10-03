#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "table/address_table.hpp"
#include "ui/address_format.hpp"

#include <QAbstractTableModel>

namespace slopkit::ui::models
{

    // Description / Address / Type / Value / Frozen rows over the address table.
    // It owns no session: value edits are encoded here and executed by the
    // AccessWorker, with the same pending-job-id guard the immediate-mode panel
    // used.
    class AddressTableModel : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        enum Column
        {
            description,
            address,
            type,
            value,
            frozen,
            column_count,
        };

        AddressTableModel(table::AddressTable&     table,
                          process::AccessWorker&   worker,
                          process::AttachedTarget& target,
                          QObject*                 parent = nullptr);

        [[nodiscard]] int           rowCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] int           columnCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] QVariant      data(const QModelIndex& index, int role) const override;
        [[nodiscard]] QVariant      headerData(int section, Qt::Orientation orientation, int role) const override;
        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
        bool                        setData(const QModelIndex& index, const QVariant& value, int role) override;

        // Re-reads the table after a completion or a freeze pass; emits nothing
        // when the entries are unchanged, so an idle poll never repaints.
        void refresh();

        // Sets the module image spans used to render module-relative addresses.
        void set_modules(std::vector<process::ModuleInfo> modules);

        // Chooses how static addresses are shown in the Address column.
        void set_address_mode(ui::AddressMode mode);

        // The rendered Address column text for `address`.
        [[nodiscard]] QString address_text(std::uint64_t address) const;

    signals:
        void statusChanged(const QString& message, bool is_error);

    private:
        bool write_value_at(int row, const QString& text);
        bool same_as_last() const;
        void note_table_changed();

        table::AddressTable&     table_;
        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;

        std::vector<table::AddressEntry> last_entries_;

        ui::ModuleSpans module_spans_;
        ui::AddressMode address_mode_ {ui::AddressMode::module_relative};

        std::optional<process::JobId> write_pending_;
        std::optional<std::uint64_t>  writing_entry_;
    };

} // namespace slopkit::ui::models
