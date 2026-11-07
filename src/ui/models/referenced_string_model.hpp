#pragma once

#include <cstdint>
#include <vector>

#include "ui/address_format.hpp"

#include <QAbstractTableModel>
#include <QString>

namespace slopkit::ui::models
{
    struct ReferencedStringRow
    {
        QString       text;
        std::uint64_t string_address {};
        QString       module;
        std::uint64_t instruction {};
    };

    class ReferencedStringModel : public QAbstractTableModel
    {
    public:
        enum Column
        {
            string,
            address,
            module,
            referenced_by,
            column_count,
        };

        ReferencedStringModel(QObject* parent, AddressText address_text);

        void set_rows(std::vector<ReferencedStringRow> rows);

        [[nodiscard]] int           row_count() const;
        [[nodiscard]] std::uint64_t instruction_at(int row) const;

        int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
        int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& index, int role) const override;
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    private:
        AddressText                      address_text_;
        std::vector<ReferencedStringRow> rows_;
    };

}
