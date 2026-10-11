#pragma once

#include "ui/components/elided_tooltip_delegate.hpp"

class QPainter;
class QStyleOptionViewItem;

namespace slopkit::ui::widgets
{
    inline constexpr int kDepthRole = Qt::UserRole + 2;

    class IndentDelegate : public ElidedTooltipDelegate
    {
        Q_OBJECT

    public:
        explicit IndentDelegate(QObject* parent = nullptr);

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    };

}
