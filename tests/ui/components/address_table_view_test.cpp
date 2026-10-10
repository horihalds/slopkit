#include <catch2/catch.hpp>

#include <memory>

#include <QApplication>
#include <QDropEvent>
#include <QHeaderView>
#include <QMimeData>
#include <QPoint>
#include <QRect>

#include "support/ui_helpers.hpp"
#include "ui/components/address_table_view.hpp"

namespace
{
    // Exposes the protected drag/drop handlers so the test can hand the view a
    // synthetic event directly, instead of relying on a live drag session.
    class TestableAddressTableView : public slopkit::ui::components::AddressTableView
    {
    public:
        using AddressTableView::AddressTableView;
        using AddressTableView::dragMoveEvent;
        using AddressTableView::dropEvent;
    };
} // namespace

TEST_CASE("the address view resolves a drag into an insert or a nest", "[ui]")
{
    using slopkit::ui::components::AddressTableView;

    const QRect row {0, 100, 200, 40}; // top y = 100, height 40

    // The top edge inserts in front of the row; the bottom edge after it.
    const auto top = AddressTableView::resolve_drop(row, 3, QPoint(50, 101));
    CHECK(top.row == 3);
    CHECK_FALSE(top.nest);

    const auto bottom = AddressTableView::resolve_drop(row, 3, QPoint(50, 139));
    CHECK(bottom.row == 4);
    CHECK_FALSE(bottom.nest);

    // The middle nests under the row.
    const auto middle = AddressTableView::resolve_drop(row, 3, QPoint(50, 120));
    CHECK(middle.row == 3);
    CHECK(middle.nest);

    // The band edge is exactly a quarter: at 10px it is still the middle, just
    // under it is the top edge.
    CHECK(AddressTableView::resolve_drop(row, 3, QPoint(50, 110)).nest);
    const auto just_outside = AddressTableView::resolve_drop(row, 3, QPoint(50, 109));
    CHECK_FALSE(just_outside.nest);
    CHECK(just_outside.row == 3);

    // A degenerate row is harmless.
    const auto empty = AddressTableView::resolve_drop(QRect(0, 0, 10, 0), 1, QPoint(0, 0));
    CHECK(empty.row == 1);
    CHECK_FALSE(empty.nest);
}

TEST_CASE("the address view forwards a drop through the model", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    const auto                   add = [&table](const char* description, std::uint64_t address)
    {
        slopkit::table::AddressEntry entry;
        entry.description = description;
        entry.address     = address;
        entry.type        = slopkit::scan::ValueType::int32;
        entry.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
        table.add(entry);
    };
    add("first", 0x1000);
    add("second", 0x2000);
    add("third", 0x3000);

    slopkit::plugin::PluginHost            host;
    slopkit::process::PluginAccess         access {host};
    slopkit::process::AccessWorker         worker {access};
    slopkit::process::AttachedTarget       target;
    slopkit::ui::models::AddressTableModel model {table, worker, target};

    TestableAddressTableView view;
    view.setModel(&model);
    view.setDragDropMode(QAbstractItemView::InternalMove);
    view.verticalHeader()->setDefaultSectionSize(30);
    view.setColumnWidth(0, 120);
    view.resize(400, 200);
    view.show();
    QApplication::processEvents();

    // A drop on the middle of the first row nests the second row under it.
    const QRect first = view.visualRect(model.index(0, 0));
    REQUIRE_FALSE(first.isEmpty());
    REQUIRE(view.indexAt(first.center()).row() == 0);

    const std::unique_ptr<QMimeData> mime {model.mimeData({model.index(1, 0)})};
    REQUIRE(mime != nullptr);
    QDropEvent drop {QPointF(first.center()), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
    view.dropEvent(&drop);

    CHECK(model.depth_at(1) == 1);
    CHECK(table.entries()[1].parent == table.entries()[0].id);

    // A drop on the row's top edge inserts in front of it instead: the nested
    // row returns to the top level.
    const QRect third_row = view.visualRect(model.index(2, 0));
    QDropEvent  edge {
        QPointF(third_row.left() + 5, third_row.top() + 1), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
    view.dropEvent(&edge);
    CHECK(model.depth_at(1) == 0);
}
