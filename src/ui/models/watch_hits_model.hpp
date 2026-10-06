#pragma once

#include <cstdint>
#include <vector>

#include "debug/access_watch.hpp"
#include "ui/address_format.hpp"

#include <QAbstractTableModel>
#include <QString>

namespace slopkit::ui::models
{

    // The coalesced accesses of the one hardware watch the controller runs: one
    // row per instruction, updated in place as its hit count and thread change.
    // Moved out of the Access Watch dialog so it can be built and tested without
    // a session; the address text arrives through the injected formatter.
    class WatchHitsModel : public QAbstractTableModel
    {
    public:
        enum Column
        {
            instruction,
            text,
            thread,
            count,
            column_count,
        };

        // `full_format` supplies the untruncated text for the hover; when it is
        // empty the display formatter is used, so a caller that only has one
        // formatter keeps working.
        WatchHitsModel(QObject* parent, AddressText format, AddressText full_format = {});

        void sync(const debug::AccessWatch& watch);

        [[nodiscard]] bool          needs_text(int row) const;
        [[nodiscard]] std::uint64_t instruction_at(int row) const;
        [[nodiscard]] std::uint64_t display_instruction_at(int row) const;
        [[nodiscard]] std::uint64_t count_at(int row) const;
        [[nodiscard]] QString       text_at(int row) const;
        void                        set_text(int row, std::uint64_t recovered, QString instruction_text);

        int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
        int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& index, int role) const override;
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    private:
        struct Row
        {
            std::uint64_t instruction {}; // the RIP the stop reported (one past the access)
            std::uint64_t recovered {};   // the instruction's own address, once decoded
            QString       text;
            std::uint32_t tid {};
            std::uint64_t count {};
        };

        AddressText      format_;
        AddressText      full_format_;
        std::uint64_t    address_ {};
        bool             watching_ {false};
        std::vector<Row> rows_;
    };

} // namespace slopkit::ui::models
