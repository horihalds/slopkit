#pragma once

#include <QIdentityProxyModel>
#include <QVariant>

class QModelIndex;

namespace slopkit::ui::models
{
    class ReclassStructureModel;
    class ReclassValueModel;

    // Fuses the structure and the values into the one tree the view sees: the
    // structure model answers the static columns, the value model the address
    // and value columns of every row.
    class ReclassProxyModel : public QIdentityProxyModel
    {
        Q_OBJECT

    public:
        ReclassProxyModel(ReclassStructureModel& structure, ReclassValueModel& values, QObject* parent = nullptr);

        [[nodiscard]] QVariant      data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
        bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

        // Repaints the two live columns of every row after a read pass.
        void refresh_values();

    private:
        void refresh(const QModelIndex& parent);

        ReclassStructureModel& structure_;
        ReclassValueModel&     values_;
    };

}
