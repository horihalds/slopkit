#pragma once

#include <unordered_map>

#include <QIdentityProxyModel>
#include <QString>
#include <QVariant>

class QModelIndex;

namespace slopkit::ui::models
{
    struct ReclassRow;
    class ReclassStructureModel;
    class ReclassValueModel;

    // Fuses the structure and the values into the one tree the view sees: the
    // structure model answers the static columns, the value model the address,
    // ascii and value columns of every row.
    class ReclassProxyModel : public QIdentityProxyModel
    {
        Q_OBJECT

    public:
        ReclassProxyModel(ReclassStructureModel& structure, ReclassValueModel& values, QObject* parent = nullptr);

        [[nodiscard]] QVariant      data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
        bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

        // Repaints the live columns after a read pass, one cell at a time: a
        // row whose text did not move is left alone, so a target that is not
        // changing costs no repaint at all.
        void refresh_values();

    private:
        // The live columns, so a pass can tell what actually moved.
        struct Shown
        {
            QString address;
            QString ascii;
            QString value;
            QString hints;
            QString pointer;
        };

        void refresh(const QModelIndex& parent);

        // A rebuild replaces every row the caches are keyed by.
        void drop_cache();

        ReclassStructureModel& structure_;
        ReclassValueModel&     values_;

        std::unordered_map<const ReclassRow*, Shown> shown_;
        bool                                         cache_valid_ {};
    };

}
