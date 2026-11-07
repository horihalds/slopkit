#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ui/address_format.hpp"

#include <QAbstractTableModel>
#include <QString>

namespace slopkit::ui::models
{
    struct CodeFilterRow
    {
        std::uint64_t target {};
        std::uint64_t instruction {};
        QString       module_offset;
        QString       kind;
        bool          executed {};
        std::size_t   executions {};
    };

    class CodeFilterModel : public QAbstractTableModel
    {
    public:
        enum Column
        {
            address,
            module_offset,
            kind,
            state,
            executions,
            column_count,
        };

        CodeFilterModel(QObject* parent, AddressText address_text);

        void set_rows(std::vector<CodeFilterRow> rows);

        [[nodiscard]] int           row_count() const;
        [[nodiscard]] std::uint64_t target_at(int row) const;

        int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
        int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& index, int role) const override;
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    private:
        AddressText                address_text_;
        std::vector<CodeFilterRow> rows_;
    };
}
