#pragma once

#include "ui/components/elided_tooltip_delegate.hpp"

class QPainter;
class QStyleOptionViewItem;

namespace slopkit::ui::widgets
{

    // The role the address-table model answers with a row's nesting depth, so a
    // delegate can indent the row under its parent. A paint-only role of its own,
    // like `kFullTextRole`, rather than overloading a cell's text.
    inline constexpr int kDepthRole = Qt::UserRole + 2;

    // Draws like its ElidedTooltipDelegate base, but insets a cell's content by
    // the row's nesting depth (read from `kDepthRole`) so a child sits under its
    // parent. Only the column the model gives a depth to is inset, so the other
    // columns keep their alignment. The cell background is painted across the
    // whole cell first, so a selected row shows no unpainted gap.
    class IndentDelegate : public ElidedTooltipDelegate
    {
        Q_OBJECT

    public:
        explicit IndentDelegate(QObject* parent = nullptr);

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    };

} // namespace slopkit::ui::widgets
