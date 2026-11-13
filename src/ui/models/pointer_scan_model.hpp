#pragma once

#include <cstdint>
#include <vector>

#include "ui/address_format.hpp"

#include <QAbstractTableModel>
#include <QString>

namespace slopkit::ui::models
{
    struct PointerScanRow
    {
        std::uint64_t base {};
        QString       offsets;
    };

    class PointerScanModel : public QAbstractTableModel
    {
    public:
        enum Column
        {
            base,
            offsets,
            column_count,
        };

        PointerScanModel(QObject* parent, AddressText address_text);

        void set_rows(std::vector<PointerScanRow> rows);

        [[nodiscard]] int           row_count() const;
        [[nodiscard]] std::uint64_t base_at(int row) const;

        int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
        int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& index, int role) const override;
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    private:
        AddressText                 address_text_;
        std::vector<PointerScanRow> rows_;
    };
}
