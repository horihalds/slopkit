#pragma once

#include <QStyledItemDelegate>

namespace slopkit::ui::widgets
{

    // The role a model uses to carry a cell's untruncated text (a shortened
    // module name inside `module+<RVA>`) for the elided delegate to reveal.
    // A role of its own rather than `Qt::ToolTipRole`, which the address tables
    // already use for messages of their own.
    inline constexpr int kFullTextRole = Qt::UserRole + 1;

    // Shows a cell's text as a tooltip when the model supplies one or when the
    // text does not fit its column, so a hover over a clipped cell reveals the
    // whole value while a hover over a fully visible one stays quiet. A model's
    // `Qt::ToolTipRole` message wins, then a non-empty `kFullTextRole` text that
    // differs from the painted one, then the column-width elision rule; the
    // default delegate would show a tooltip for any cell the model gives a
    // tooltip text.
    class ElidedTooltipDelegate : public QStyledItemDelegate
    {
        Q_OBJECT

    public:
        explicit ElidedTooltipDelegate(QObject* parent = nullptr);

        // Public so tests can drive it with a synthetic QHelpEvent; the toolkit
        // calls it for real hovers.
        bool helpEvent(QHelpEvent*                 event,
                       QAbstractItemView*          view,
                       const QStyleOptionViewItem& option,
                       const QModelIndex&          index) override;
    };

} // namespace slopkit::ui::widgets
