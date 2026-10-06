#include <catch2/catch.hpp>

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
