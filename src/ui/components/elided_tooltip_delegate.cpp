#include "ui/components/elided_tooltip_delegate.hpp"

#include <QAbstractItemView>
#include <QEvent>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QModelIndex>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QToolTip>

namespace slopkit::ui::widgets
{

    ElidedTooltipDelegate::ElidedTooltipDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    bool ElidedTooltipDelegate::helpEvent(QHelpEvent*                 event,
                                          QAbstractItemView*          view,
                                          const QStyleOptionViewItem& option,
                                          const QModelIndex&          index)
    {
        if (event->type() != QEvent::ToolTip || view == nullptr || !index.isValid())
        {
            return QStyledItemDelegate::helpEvent(event, view, option, index);
        }

        // A model's own tooltip message wins, so attaching this delegate to a
        // table cannot drop the tooltips it already provides.
        if (const QString message = index.data(Qt::ToolTipRole).toString(); !message.isEmpty())
        {
            QToolTip::showText(event->globalPos(), message, view);
            return true;
        }

        // initStyleOption applies the cell's FontRole, and the view's visualRect
        // gives the rectangle the text has to fit, so the elision measured here
        // is the one the view paints.
        QStyleOptionViewItem cell = option;
        initStyleOption(&cell, index);
        cell.rect = view->visualRect(index);

        const QString text      = index.data(Qt::DisplayRole).toString();
        const QRect   text_rect = view->style()->subElementRect(QStyle::SE_ItemViewItemText, &cell, view);

        // The untruncated spelling, when the model offers one that differs from
        // the painted text (a shortened module name and its RVA).
        if (const QString full = index.data(kFullTextRole).toString(); !full.isEmpty() && full != text)
        {
            QToolTip::showText(event->globalPos(), full, view);
            return true;
        }

        if (text.isEmpty() || QFontMetrics(cell.font).horizontalAdvance(text) <= text_rect.width())
        {
            QToolTip::hideText();
            event->ignore();
            return true;
        }

        QToolTip::showText(event->globalPos(), text, view);
        return true;
    }

} // namespace slopkit::ui::widgets
