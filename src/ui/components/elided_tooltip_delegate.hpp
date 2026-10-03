#pragma once

#include <QStyledItemDelegate>

namespace slopkit::ui::widgets
{

    // Shows a cell's text as a tooltip only when it does not fit its column, so
    // a hover over a clipped cell reveals the whole value while a hover over a
    // fully visible one stays quiet. The default delegate would show a tooltip
    // for any cell the model gives a tooltip text.
    class ElidedTooltipDelegate : public QStyledItemDelegate
    {
        Q_OBJECT

    public:
        explicit ElidedTooltipDelegate(QObject* parent = nullptr);

    protected:
        bool helpEvent(QHelpEvent*                 event,
                       QAbstractItemView*          view,
                       const QStyleOptionViewItem& option,
                       const QModelIndex&          index) override;
    };

} // namespace slopkit::ui::widgets
