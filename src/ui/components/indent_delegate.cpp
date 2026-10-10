#include "ui/components/indent_delegate.hpp"

#include <QApplication>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

namespace slopkit::ui::widgets
{
    IndentDelegate::IndentDelegate(QObject* parent) : ElidedTooltipDelegate(parent) {}

    void IndentDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        // A depth of zero (a top-level row, or any column the model does not give
        // a depth to) is the plain delegate.
        const int depth = index.data(kDepthRole).toInt();
        if (depth <= 0)
        {
            ElidedTooltipDelegate::paint(painter, option, index);
            return;
        }

        const QWidget* widget = option.widget;
        QStyle*        style  = widget != nullptr ? widget->style() : QApplication::style();
        // A logical-unit value the style already scales with the rest of the UI.
        const int      indent = depth * style->pixelMetric(QStyle::PM_TreeViewIndentation, nullptr, widget);

        // The background first, at the full cell rect, so the selection highlight
        // is not broken by the inset; then the content, shifted right by the
        // depth.
        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &background, painter, widget);

        QStyleOptionViewItem inset = option;
        inset.rect                 = option.rect.adjusted(indent, 0, 0, 0);
        ElidedTooltipDelegate::paint(painter, inset, index);
    }

} // namespace slopkit::ui::widgets
