#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "table/address_table.hpp"
#include "ui/address_format.hpp"
#include "ui/live_values.hpp"
#include "ui/models/live_cells.hpp"

#include <QAbstractTableModel>
#include <QStringList>

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

        // Drag-reorder contract: a row can be moved within the list only, and the
        // drop is forwarded to AddressTable::move so ids and fields stay put.
        [[nodiscard]] Qt::DropActions supportedDropActions() const override;
        [[nodiscard]] QStringList     mimeTypes() const override;
        [[nodiscard]] QMimeData*      mimeData(const QModelIndexList& indexes) const override;
        [[nodiscard]] bool            canDropMimeData(const QMimeData*   data,
                                                      Qt::DropAction     action,
                                                      int                row,
                                                      int                column,
                                                      const QModelIndex& parent) const override;
        bool                          dropMimeData(
            const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) override;

        // Re-reads the table after a completion or a freeze pass; emits nothing
        // when the entries are unchanged, so an idle poll never repaints.
        void refresh();

        // Sets the module image spans used to render module-relative addresses.
        void set_modules(std::vector<process::ModuleInfo> modules);

        // Chooses how static addresses are shown in the Address column.
        void set_address_mode(ui::AddressMode mode);

        // The rendered Address column text for `address`.
        [[nodiscard]] QString address_text(std::uint64_t address) const;

        // The addresses of the rows that should be re-read, in row order; the
        // entry being written and zero-width entries are skipped. The ids echo
        // the row index, so a reading can be matched back to its row.
        [[nodiscard]] std::vector<LiveRequest> next_live_request();

        // Applies one live pass: a reading whose row has since changed identity
        // or address is dropped. Emits dataChanged for the value cells whose
        // text or colour changed.
        void apply_live_readings(std::span<const LiveReading> readings);

    signals:
        void statusChanged(const QString& message, bool is_error);

    private:
        bool                          write_value_at(int row, const QString& text);
        bool                          same_as_last() const;
        void                          note_table_changed();
        void                          seed_live_write(std::uint64_t entry_id);
        [[nodiscard]] const LiveCell* live_cell_at(std::size_t row) const;

        table::AddressTable&     table_;
        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;

        std::vector<table::AddressEntry> last_entries_;

        ui::ModuleSpans module_spans_;
        ui::AddressMode address_mode_ {ui::AddressMode::module_relative};

        std::optional<process::JobId> write_pending_;
        std::optional<std::uint64_t>  writing_entry_;

        // The per-row live readings, parallel to the table rows; the in-flight
        // request identity lives inside it too.
        LiveCells cells_;
    };

} // namespace slopkit::ui::models
