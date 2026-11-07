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
#include "ui/models/copy_format.hpp"
#include "ui/models/live_cells.hpp"

#include <QAbstractTableModel>
#include <QStringList>

namespace slopkit::ui::models
{
    class AddressTableModel : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        enum Column
        {
            active,
            description,
            address,
            type,
            value,
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

        bool drop_row(int source, int target, bool nest);

        [[nodiscard]] int depth_at(std::size_t row) const;

        void refresh();

        void set_modules(std::vector<process::ModuleInfo> modules);

        void set_address_mode(ui::AddressMode mode);

        [[nodiscard]] QString address_text(std::uint64_t address) const;

        [[nodiscard]] QString copy_text(std::size_t row, ui::models::CopyFormat format) const;

        [[nodiscard]] std::vector<LiveRequest> next_live_request();

        void apply_live_readings(std::span<const LiveReading> readings);

        void note_entry_changed(std::size_t row);

        bool write_values(std::span<const std::size_t> rows, const QString& text);

        [[nodiscard]] bool rows_share_value_format(std::span<const std::size_t> rows) const;

    signals:
        void statusChanged(const QString& message, bool is_error);

        void scriptActiveRequested(std::size_t row, bool wanted);

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
        std::vector<std::uint64_t>    writing_entries_;

        LiveCells cells_;
    };

}
