#pragma once

#include <QStyledItemDelegate>

namespace slopkit::ui::widgets
{
    inline constexpr int kFullTextRole = Qt::UserRole + 1;

    class ElidedTooltipDelegate : public QStyledItemDelegate
    {
        Q_OBJECT

    public:
        explicit ElidedTooltipDelegate(QObject* parent = nullptr);

        bool helpEvent(QHelpEvent*                 event,
                       QAbstractItemView*          view,
                       const QStyleOptionViewItem& option,
                       const QModelIndex&          index) override;
    };

}
