#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ui/access_watch.hpp"
#include "ui/address_format.hpp"

#include <QAbstractTableModel>
#include <QString>

namespace slopkit::ui::models
{

    // The resolved operands of the instruction the user picked in the listing:
    // its address, operand text, width and access kind. Moved out of the Access
    // Watch dialog so it can be built and tested without a session; the address
    // text arrives through the injected formatter.
    class InstructionAccessModel : public QAbstractTableModel
    {
    public:
        enum Column
        {
            address,
            operand,
            width,
            access,
            column_count,
        };

        // `full_format` supplies the untruncated text for the hover; when it is
        // empty the display formatter is used, so a caller that only has one
        // formatter keeps working.
        InstructionAccessModel(QObject* parent, AddressText format, AddressText full_format = {});

        void set(std::vector<ui::ResolvedAccess> accesses);

        [[nodiscard]] int           row_count() const;
        [[nodiscard]] std::uint64_t address_at(int row) const;
        [[nodiscard]] std::size_t   width_at(int row) const;
        [[nodiscard]] bool          resolved_at(int row) const;
        [[nodiscard]] bool          any_resolved() const;

        int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
        int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& index, int role) const override;
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    private:
        struct Row
        {
            std::uint64_t address {};
            QString       operand;
            std::size_t   width {};
            bool          writes {};
            bool          resolved {};
        };

        AddressText      format_;
        AddressText      full_format_;
        std::vector<Row> rows_;
    };

} // namespace slopkit::ui::models
