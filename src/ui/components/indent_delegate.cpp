#include "ui/components/indent_delegate.hpp"

#include <QApplication>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

namespace slopkit::ui::widgets
{
    IndentDelegate::IndentDelegate(QObject* parent) : components::ValidatingDelegate(parent) {}

    void IndentDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        const int depth = index.data(kDepthRole).toInt();
        if (depth <= 0)
        {
            ElidedTooltipDelegate::paint(painter, option, index);
            return;
        }

        const QWidget* widget = option.widget;
        QStyle*        style  = widget != nullptr ? widget->style() : QApplication::style();
        const int      indent = depth * style->pixelMetric(QStyle::PM_TreeViewIndentation, nullptr, widget);

        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &background, painter, widget);

        QStyleOptionViewItem inset = option;
        inset.rect                 = option.rect.adjusted(indent, 0, 0, 0);
        ElidedTooltipDelegate::paint(painter, inset, index);
    }

}
