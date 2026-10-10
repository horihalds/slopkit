#include <catch2/catch.hpp>

#include <algorithm>
#include <utility>

#include <QAbstractTableModel>
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "support/ui_helpers.hpp"
#include "ui/components/indent_delegate.hpp"

namespace
{
    // Answers a fixed depth for its one cell, so the delegate can be exercised
    // without the address table behind it.
    class DepthModel : public QAbstractTableModel
    {
    public:
        explicit DepthModel(int depth) : depth_(depth) {}

        [[nodiscard]] int rowCount(const QModelIndex& = {}) const override
        {
            return 1;
        }

        [[nodiscard]] int columnCount(const QModelIndex& = {}) const override
        {
            return 1;
        }

        [[nodiscard]] QVariant data(const QModelIndex&, int role) const override
        {
            if (role == Qt::DisplayRole)
            {
                return QStringLiteral("nested value");
            }
            if (role == slopkit::ui::widgets::kDepthRole)
            {
                return depth_;
            }
            return {};
        }

    private:
        int depth_ {};
    };
} // namespace

TEST_CASE("the indent delegate insets a nested cell without clipping it", "[ui]")
{
    application();

    slopkit::ui::widgets::IndentDelegate delegate;

    // The left and right edges of the painted content, against the background at
    // the cell's top-left corner.
    const auto content_box = [&delegate](int depth)
    {
        DepthModel model {depth};
        QImage     image {240, 30, QImage::Format_ARGB32};
        // A colour that contrasts with both a light and a dark theme's text, so
        // the painted content is always detectable against the corner pixel.
        image.fill(QColor(255, 0, 255));
        {
            QPainter             painter {&image};
            QStyleOptionViewItem option;
            option.rect    = QRect(0, 0, image.width(), image.height());
            option.palette = QApplication::palette();
            option.state   = QStyle::State_Enabled;
            option.font    = QApplication::font();
            delegate.paint(&painter, option, model.index(0, 0));
        }

        const QColor background = image.pixelColor(0, 0);
        int          left       = image.width();
        int          right      = -1;
        for (int x = 0; x < image.width(); ++x)
        {
            for (int y = 0; y < image.height(); ++y)
            {
                if (image.pixelColor(x, y) != background)
                {
                    left  = std::min(left, x);
                    right = std::max(right, x);
                }
            }
        }
        return std::pair {left, right};
    };

    const auto flat   = content_box(0);
    const auto nested = content_box(1);

    // Both cells drew something.
    REQUIRE(flat.second >= flat.first);
    REQUIRE(nested.second >= nested.first);

    // The nested cell's content starts one indentation step further right, and
    // still fits inside the cell.
    const int indent = QApplication::style()->pixelMetric(QStyle::PM_TreeViewIndentation);
    CHECK(nested.first > flat.first);
    CHECK(nested.first - flat.first >= indent);
    CHECK(nested.second < 240);
}
