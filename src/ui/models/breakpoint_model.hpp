#pragma once

#include <cstdint>
#include <vector>

#include <QAbstractTableModel>
#include <QString>

#include "debug/breakpoints.hpp"
#include "debug/controller.hpp"

namespace slopkit::ui::models
{

    // A read-only view over the session's breakpoint table, with the Enabled
    // column checkable: a toggle is routed to the controller, which re-arms or
    // disarms the breakpoint. The rows are a snapshot, refreshed on every
    // breakpointsChanged.
    class BreakpointModel : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        enum Column
        {
            enabled,
            kind,
            size,
            address,
            hits,
            column_count,
        };

        explicit BreakpointModel(slopkit::debug::Controller& controller, QObject* parent = nullptr);

        void refresh();

        [[nodiscard]] std::uint64_t id_at(int row) const;
        [[nodiscard]] bool          is_armed_at(int row) const;

        [[nodiscard]] int           rowCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] int           columnCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] QVariant      data(const QModelIndex& index, int role) const override;
        [[nodiscard]] QVariant      headerData(int section, Qt::Orientation orientation, int role) const override;
        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
        bool                        setData(const QModelIndex& index, const QVariant& value, int role) override;

    private:
        slopkit::debug::Controller&             controller_;
        std::vector<slopkit::debug::Breakpoint> rows_;
    };

} // namespace slopkit::ui::models
