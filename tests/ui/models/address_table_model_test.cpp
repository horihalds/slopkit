#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <QMimeData>

#include "support/ui_helpers.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"

TEST_CASE("the address-table model edits the table", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    slopkit::table::AddressEntry entry;
    entry.address = 0x1040;
    entry.type    = slopkit::scan::ValueType::int32;
    entry.bytes   = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(entry);

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::models::AddressTableModel model {table, worker, target};

    CHECK(model.rowCount() == 1);
    CHECK(model.columnCount() == 5);
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("1040"));
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::type), Qt::DisplayRole).toString()
          == QStringLiteral("4 Bytes"));
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::value), Qt::DisplayRole).toString()
          == QStringLiteral("1"));
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::frozen), Qt::CheckStateRole).toInt()
          == Qt::Unchecked);

    // The Frozen column doubles as the freeze toggle.
    REQUIRE(
        model.setData(model.index(0, slopkit::ui::models::AddressTableModel::frozen), Qt::Checked, Qt::CheckStateRole));
    CHECK(table.entries()[0].active);
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::frozen), Qt::CheckStateRole).toInt()
          == Qt::Checked);

    // A description edit lands in the table.
    REQUIRE(model.setData(
        model.index(0, slopkit::ui::models::AddressTableModel::description), QStringLiteral("health"), Qt::EditRole));
    CHECK(table.entries()[0].description == "health");

    // Without a target the value edit is refused and the model says why.
    QString message;
    bool    is_error = false;
    QObject::connect(&model,
                     &slopkit::ui::models::AddressTableModel::statusChanged,
                     &model,
                     [&](const QString& text, bool error)
                     {
                         message  = text;
                         is_error = error;
                     });
    CHECK_FALSE(model.setData(
        model.index(0, slopkit::ui::models::AddressTableModel::value), QStringLiteral("42"), Qt::EditRole));
    CHECK(is_error);
    CHECK(message.contains(QStringLiteral("Not attached")));
}

TEST_CASE("the address-table model renders static addresses as module+RVA", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    slopkit::table::AddressEntry entry;
    entry.address = 0x1040;
    entry.type    = slopkit::scan::ValueType::int32;
    entry.bytes   = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(entry);

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::models::AddressTableModel model {table, worker, target};

    const auto address_cell = [&model]()
    {
        return model.data(model.index(0, slopkit::ui::models::AddressTableModel::address), Qt::DisplayRole).toString();
    };

    // Without a module map the address stays absolute even in the default mode.
    CHECK(address_cell() == QStringLiteral("1040"));

    // Inside a module image the Address column renders name+RVA.
    model.set_modules({module_image("app", 0x1000, 0x1000)});
    CHECK(address_cell() == QStringLiteral("app+40"));

    // A heap address is never labelled.
    CHECK(model.address_text(0x5000000) == QStringLiteral("5000000"));

    // Absolute mode restores the raw text.
    model.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(address_cell() == QStringLiteral("1040"));

    // A long module name shortens the display but keeps the full spelling for
    // the hover role.
    model.set_address_mode(slopkit::ui::AddressMode::module_relative);
    model.set_modules({module_image("DyingLightGame_TheBeast_x64_rwdi.exe", 0x1000, 0x10000)});
    CHECK(address_cell() == QStringLiteral("DyingL..._rwdi.exe+40"));
    CHECK(
        model.data(model.index(0, slopkit::ui::models::AddressTableModel::address), slopkit::ui::widgets::kFullTextRole)
            .toString()
        == QStringLiteral("DyingLightGame_TheBeast_x64_rwdi.exe+40"));
}

TEST_CASE("the address-table model shows an entry's expression and its resolved tooltip", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    slopkit::table::AddressEntry entry;
    entry.address    = 0x1040;
    entry.expression = "app+40";
    entry.type       = slopkit::scan::ValueType::int32;
    entry.bytes      = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(entry);

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::models::AddressTableModel model {table, worker, target};

    const auto address_index = model.index(0, slopkit::ui::models::AddressTableModel::address);
    CHECK(model.data(address_index, Qt::DisplayRole).toString() == QStringLiteral("app+40"));
    CHECK(model.data(address_index, Qt::ToolTipRole).toString() == QStringLiteral("1040"));

    // A plain entry keeps the resolved address in the cell and carries no tooltip.
    table.entries()[0].expression.clear();
    CHECK(model.data(address_index, Qt::DisplayRole).toString() == QStringLiteral("1040"));
    CHECK(model.data(address_index, Qt::ToolTipRole).toString().isEmpty());
}

TEST_CASE("the address-table model reorders rows through a move drop", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    const auto                   add_row = [&table](const QString& description, std::uint64_t address, std::byte value)
    {
        slopkit::table::AddressEntry entry;
        entry.description = description.toStdString();
        entry.address     = address;
        entry.type        = slopkit::scan::ValueType::int32;
        entry.bytes       = {value, std::byte {0}, std::byte {0}, std::byte {0}};
        table.add(entry);
    };
    add_row(QStringLiteral("first"), 0x1000, std::byte {1});
    add_row(QStringLiteral("second"), 0x2000, std::byte {2});
    add_row(QStringLiteral("third"), 0x3000, std::byte {3});

    const std::vector<std::uint64_t> ids {table.entries()[0].id, table.entries()[1].id, table.entries()[2].id};

    slopkit::plugin::PluginHost            host;
    slopkit::process::PluginAccess         access {host};
    slopkit::process::AccessWorker         worker {access};
    slopkit::process::AttachedTarget       target;
    slopkit::ui::models::AddressTableModel model {table, worker, target};

    CHECK(model.supportedDropActions() == Qt::MoveAction);
    CHECK(model.mimeTypes().contains(QStringLiteral("application/x-slopkit-address-row")));

    // A foreign payload or a copy action is rejected.
    QMimeData foreign;
    foreign.setText(QStringLiteral("nope"));
    CHECK_FALSE(model.canDropMimeData(&foreign, Qt::MoveAction, 0, 0, QModelIndex()));

    std::unique_ptr<QMimeData> payload {model.mimeData({model.index(0, 0)})};
    REQUIRE(payload != nullptr);
    CHECK(model.canDropMimeData(payload.get(), Qt::MoveAction, 0, 0, QModelIndex()));
    CHECK_FALSE(model.canDropMimeData(payload.get(), Qt::CopyAction, 0, 0, QModelIndex()));

    // Seed a live reading per row so a stale value would be visible after a move.
    (void)model.next_live_request();
    const std::vector<slopkit::ui::LiveReading> readings {
        {.id = 0, .readable = true, .bytes = {std::byte {0x7F}, std::byte {0}, std::byte {0}, std::byte {0}}},
        {.id = 1, .readable = true, .bytes = {std::byte {0x7E}, std::byte {0}, std::byte {0}, std::byte {0}}},
        {.id = 2, .readable = true, .bytes = {std::byte {0x7D}, std::byte {0}, std::byte {0}, std::byte {0}}},
    };
    model.apply_live_readings(readings);

    int moved_count = 0;
    QObject::connect(&model,
                     &QAbstractItemModel::rowsMoved,
                     &model,
                     [&moved_count](const QModelIndex&, int, int, const QModelIndex&, int)
                     {
                         ++moved_count;
                     });
    // Drop row 0 below the last row (insertion row 3).
    CHECK(model.dropMimeData(payload.get(), Qt::MoveAction, 3, 0, QModelIndex()));
    CHECK(moved_count == 1);

    REQUIRE(model.rowCount() == 3);
    CHECK(table.entries()[0].description == "second");
    CHECK(table.entries()[1].description == "third");
    CHECK(table.entries()[2].description == "first");
    // The ids moved with their entries.
    CHECK(table.entries()[2].id == ids[0]);
    CHECK(table.entries()[0].id == ids[1]);

    // The moved and shifted rows lost their stale reading and fall back to the
    // entries' own cached bytes.
    const auto value_text = [&model](int row)
    {
        return model.data(model.index(row, slopkit::ui::models::AddressTableModel::value), Qt::DisplayRole).toString();
    };
    CHECK(value_text(0) == QStringLiteral("2"));
    CHECK(value_text(1) == QStringLiteral("3"));
    CHECK(value_text(2) == QStringLiteral("1"));

    // The next live pass asks for the new order.
    const auto requests = model.next_live_request();
    REQUIRE(requests.size() == 3);
    CHECK(requests[0].address == 0x2000);
    CHECK(requests[1].address == 0x3000);
    CHECK(requests[2].address == 0x1000);

    // Dropping a row onto itself changes nothing.
    std::unique_ptr<QMimeData> self {model.mimeData({model.index(0, 0)})};
    REQUIRE(self != nullptr);
    CHECK_FALSE(model.dropMimeData(self.get(), Qt::MoveAction, 0, 0, QModelIndex()));
    CHECK(table.entries()[0].description == "second");
}
