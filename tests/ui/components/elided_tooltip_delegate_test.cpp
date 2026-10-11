#include <catch2/catch.hpp>

#include <QAbstractTableModel>
#include <QVariant>

#include "support/ui_helpers.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"

namespace
{
    using slopkit::ui::widgets::ElidedTooltipDelegate;
    using slopkit::ui::widgets::kFullTextRole;

    class StubModel : public QAbstractTableModel
    {
    public:
        QString display;
        QString tooltip;
        QString full;

        int rowCount(const QModelIndex&) const override
        {
            return 1;
        }

        int columnCount(const QModelIndex&) const override
        {
            return 1;
        }

        QVariant data(const QModelIndex&, int role) const override
        {
            switch (role)
            {
            case Qt::DisplayRole:
                return display;
            case Qt::ToolTipRole:
                return tooltip;
            case kFullTextRole:
                return full;
            default:
                return {};
            }
        }
    };
}

TEST_CASE("the elided tooltip delegate prefers a model tooltip then the full text", "[ui]")
{
    application();
    StubModel  model;
    QTableView view;
    view.setModel(&model);
    view.resize(80, 40);
    view.show();

    ElidedTooltipDelegate delegate;
    const QModelIndex     index = model.index(0, 0);
    QStyleOptionViewItem  option;
    option.rect = view.visualRect(index);

    QHelpEvent event(QEvent::ToolTip, QPoint(1, 1), view.viewport()->mapToGlobal(QPoint(1, 1)));

    model.display = QStringLiteral("a long clipped cell value");
    model.tooltip = QStringLiteral("Freeze this value");
    model.full    = QStringLiteral("The full untruncated text");
    CHECK(delegate.helpEvent(&event, &view, option, index));
    CHECK(QToolTip::text() == QStringLiteral("Freeze this value"));

    model.tooltip.clear();
    CHECK(delegate.helpEvent(&event, &view, option, index));
    CHECK(QToolTip::text() == QStringLiteral("The full untruncated text"));

    model.display = QStringLiteral("x");
    model.full.clear();
    CHECK(delegate.helpEvent(&event, &view, option, index));
    CHECK(QToolTip::text() != QStringLiteral("x"));
}
