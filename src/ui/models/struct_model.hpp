#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "structs/definition.hpp"
#include "ui/components/struct_document.hpp"
#include "ui/live_values.hpp"

#include <QAbstractTableModel>
#include <QString>

namespace slopkit::ui::models
{
    // Presents a `structs::Structs` as a flat, depth-indented table whose live
    // values come from a `components::StructDocument`, so the model itself never
    // touches the target.
    class StructModel : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        enum Column
        {
            offset,
            type,
            name,
            value,
            column_count,
        };

        StructModel(structs::Structs& definitions, components::StructDocument& document, QObject* parent = nullptr);

        [[nodiscard]] int           rowCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] int           columnCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] QVariant      data(const QModelIndex& index, int role) const override;
        [[nodiscard]] QVariant      headerData(int section, Qt::Orientation orientation, int role) const override;
        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
        bool                        setData(const QModelIndex& index, const QVariant& edited, int role) override;

        [[nodiscard]] int depth_at(std::size_t row) const;

        void refresh();
        void notify_values_changed();

        // The node behind a view row, or 0 when the model has since changed.
        [[nodiscard]] std::uint64_t id_at(std::size_t row) const;

    signals:
        void statusChanged(const QString& message, bool is_error);

    private:
        structs::Structs&           definitions_;
        components::StructDocument& document_;
    };

}
