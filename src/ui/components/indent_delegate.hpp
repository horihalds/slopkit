#pragma once

#include "ui/components/validating_delegate.hpp"

class QPainter;
class QStyleOptionViewItem;

namespace slopkit::ui::widgets
{
    inline constexpr int kDepthRole = Qt::UserRole + 2;

    // Indents a nested cell and edits it the way every other cell editor does:
    // the validation comes with the base, the painting is this one's own.
    class IndentDelegate : public components::ValidatingDelegate
    {
        Q_OBJECT

    public:
        explicit IndentDelegate(QObject* parent = nullptr);

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    };

}
