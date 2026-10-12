#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <QMimeData>

#include "support/ui_helpers.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/components/indent_delegate.hpp"

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

    CHECK(static_cast<int>(slopkit::ui::models::AddressTableModel::active) == 0);
    CHECK(static_cast<int>(slopkit::ui::models::AddressTableModel::description) == 1);
    CHECK(static_cast<int>(slopkit::ui::models::AddressTableModel::value) == 4);

    CHECK(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString().isEmpty());
    CHECK(model.headerData(1, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Description"));

    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("0x1040"));
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::type), Qt::DisplayRole).toString()
          == QStringLiteral("4 Bytes"));
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::value), Qt::DisplayRole).toString()
          == QStringLiteral("1"));
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::active), Qt::CheckStateRole).toInt()
          == Qt::Unchecked);

    REQUIRE(
        model.setData(model.index(0, slopkit::ui::models::AddressTableModel::active), Qt::Checked, Qt::CheckStateRole));
    CHECK(table.entries()[0].active);
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::active), Qt::CheckStateRole).toInt()
          == Qt::Checked);

    REQUIRE(model.setData(
        model.index(0, slopkit::ui::models::AddressTableModel::description), QStringLiteral("health"), Qt::EditRole));
    CHECK(table.entries()[0].description == "health");

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

TEST_CASE("the address-table model writes one value to a selection of like entries", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    add_int32(table, 0x1040);
    add_int32(table, 0x1044);
    add_int32(table, 0x1048, std::byte {7});
    table.entries()[2].hex = true;

    slopkit::ui::models::AddressTableModel model {table, worker, target};

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

    const std::size_t                  like_rows[]  = {0, 1};
    const std::size_t                  mixed_rows[] = {0, 2};
    const std::size_t                  single_row[] = {0};
    const std::span<const std::size_t> no_rows {};
    CHECK(model.rows_share_value_format(like_rows));
    CHECK(model.rows_share_value_format(single_row));
    CHECK_FALSE(model.rows_share_value_format(mixed_rows));
    CHECK_FALSE(model.rows_share_value_format(no_rows));

    attach_app_session(worker);
    access.memory->at(0x1040) = std::byte {0};

    CHECK(model.write_values(like_rows, QStringLiteral("42")));
    CHECK(pump_ui(worker,
                  [&]
                  {
                      return access.memory->at(0x1040) == std::byte {42} && access.memory->at(0x1044) == std::byte {42};
                  }));
    CHECK_FALSE(is_error);
    CHECK(message == QStringLiteral("2 values written."));
    CHECK(table.entries()[0].bytes.front() == std::byte {42});
    CHECK(table.entries()[1].bytes.front() == std::byte {42});
    CHECK(table.entries()[2].bytes.front() == std::byte {7});

    CHECK_FALSE(model.write_values(like_rows, QStringLiteral("nope")));
    CHECK(is_error);
    CHECK(message.startsWith(QStringLiteral("Value:")));

    CHECK_FALSE(model.write_values(mixed_rows, QStringLiteral("42")));
    CHECK(message == QStringLiteral("Select entries of the same type and format."));

    CHECK(model.setData(
        model.index(2, slopkit::ui::models::AddressTableModel::value), QStringLiteral("2A"), Qt::EditRole));
    CHECK(pump_ui(worker,
                  [&]
                  {
                      return access.memory->at(0x1048) == std::byte {0x2A};
                  }));
    CHECK_FALSE(is_error);
    CHECK(message == QStringLiteral("Value written."));

    slopkit::process::AttachedTarget       detached;
    slopkit::ui::models::AddressTableModel detached_model {table, worker, detached};
    message = {};
    QObject::connect(&detached_model,
                     &slopkit::ui::models::AddressTableModel::statusChanged,
                     &detached_model,
                     [&](const QString& text, bool error)
                     {
                         message  = text;
                         is_error = error;
                     });
    CHECK_FALSE(detached_model.write_values(single_row, QStringLiteral("1")));
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

    CHECK(address_cell() == QStringLiteral("0x1040"));

    model.set_modules({module_image("app", 0x1000, 0x1000)});
    CHECK(address_cell() == QStringLiteral("app+0x40"));

    CHECK(model.address_text(0x5000000) == QStringLiteral("0x5000000"));

    model.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(address_cell() == QStringLiteral("0x1040"));

    model.set_address_mode(slopkit::ui::AddressMode::module_relative);
    model.set_modules({module_image("DyingLightGame_TheBeast_x64_rwdi.exe", 0x1000, 0x10000)});
    CHECK(address_cell() == QStringLiteral("DyingL..._rwdi.exe+0x40"));
    CHECK(
        model.data(model.index(0, slopkit::ui::models::AddressTableModel::address), slopkit::ui::widgets::kFullTextRole)
            .toString()
        == QStringLiteral("DyingLightGame_TheBeast_x64_rwdi.exe+0x40"));
}

TEST_CASE("the address-table model shows the resolved address and its expression tooltip", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    slopkit::table::AddressEntry entry;
    entry.address    = 0x1040;
    entry.expression = "app+40";
    entry.type       = slopkit::scan::ValueType::int32;
    entry.bytes      = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(entry);

    slopkit::table::AddressEntry long_entry;
    long_entry.address    = 0x1040;
    long_entry.expression = "DyingLightGame_TheBeast_x64_rwdi.exe+0x40";
    long_entry.type       = slopkit::scan::ValueType::int32;
    long_entry.bytes      = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(long_entry);

    slopkit::table::AddressEntry unresolved;
    unresolved.expression = "future+0x10";
    unresolved.type       = slopkit::scan::ValueType::int32;
    unresolved.bytes      = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(unresolved);

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::models::AddressTableModel model {table, worker, target};
    model.set_modules({module_image("app", 0x1000, 0x1000)});

    const auto address_index = model.index(0, slopkit::ui::models::AddressTableModel::address);
    CHECK(model.data(address_index, Qt::DisplayRole).toString() == QStringLiteral("app+0x40"));
    CHECK(model.data(address_index, Qt::EditRole).toString() == QStringLiteral("app+0x40"));
    CHECK(model.data(address_index, Qt::ToolTipRole).toString() == QStringLiteral("app+40\n0x1040"));
    CHECK(model.data(address_index, slopkit::ui::widgets::kFullTextRole).toString() == QStringLiteral("app+0x40"));

    const auto long_index = model.index(1, slopkit::ui::models::AddressTableModel::address);
    CHECK(model.data(long_index, Qt::DisplayRole).toString() == QStringLiteral("app+0x40"));
    CHECK(model.data(long_index, Qt::EditRole).toString() == QStringLiteral("app+0x40"));
    CHECK(model.data(long_index, Qt::ToolTipRole).toString()
          == QStringLiteral("DyingLightGame_TheBeast_x64_rwdi.exe+0x40\n0x1040"));
    CHECK(table.entries()[1].expression == "DyingLightGame_TheBeast_x64_rwdi.exe+0x40");

    const auto unresolved_index = model.index(2, slopkit::ui::models::AddressTableModel::address);
    CHECK(model.data(unresolved_index, Qt::DisplayRole).toString() == QStringLiteral("0x0"));
    CHECK(model.data(unresolved_index, Qt::ToolTipRole).toString() == QStringLiteral("future+0x10\n0x0"));

    model.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(model.data(address_index, Qt::DisplayRole).toString() == QStringLiteral("0x1040"));
    CHECK(model.data(address_index, Qt::EditRole).toString() == QStringLiteral("0x1040"));
    CHECK(model.data(address_index, slopkit::ui::widgets::kFullTextRole).toString() == QStringLiteral("0x1040"));

    table.entries()[0].expression.clear();
    CHECK(model.data(address_index, Qt::DisplayRole).toString() == QStringLiteral("0x1040"));
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

    QMimeData foreign;
    foreign.setText(QStringLiteral("nope"));
    CHECK_FALSE(model.canDropMimeData(&foreign, Qt::MoveAction, 0, 0, QModelIndex()));

    std::unique_ptr<QMimeData> payload {model.mimeData({model.index(0, 0)})};
    REQUIRE(payload != nullptr);
    CHECK(model.canDropMimeData(payload.get(), Qt::MoveAction, 0, 0, QModelIndex()));
    CHECK_FALSE(model.canDropMimeData(payload.get(), Qt::CopyAction, 0, 0, QModelIndex()));

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
    CHECK(model.dropMimeData(payload.get(), Qt::MoveAction, 3, 0, QModelIndex()));
    CHECK(moved_count == 1);

    REQUIRE(model.rowCount() == 3);
    CHECK(table.entries()[0].description == "second");
    CHECK(table.entries()[1].description == "third");
    CHECK(table.entries()[2].description == "first");
    CHECK(table.entries()[2].id == ids[0]);
    CHECK(table.entries()[0].id == ids[1]);

    const auto value_text = [&model](int row)
    {
        return model.data(model.index(row, slopkit::ui::models::AddressTableModel::value), Qt::DisplayRole).toString();
    };
    CHECK(value_text(0) == QStringLiteral("2"));
    CHECK(value_text(1) == QStringLiteral("3"));
    CHECK(value_text(2) == QStringLiteral("1"));

    const auto requests = model.next_live_request();
    REQUIRE(requests.size() == 3);
    CHECK(requests[0].address == 0x2000);
    CHECK(requests[1].address == 0x3000);
    CHECK(requests[2].address == 0x1000);

    std::unique_ptr<QMimeData> self {model.mimeData({model.index(0, 0)})};
    REQUIRE(self != nullptr);
    CHECK_FALSE(model.dropMimeData(self.get(), Qt::MoveAction, 0, 0, QModelIndex()));
    CHECK(table.entries()[0].description == "second");
}

TEST_CASE("a drag moves the cursor row among a multi-row selection", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    const auto                   add_row = [&table](const QString& description, std::uint64_t address)
    {
        slopkit::table::AddressEntry entry;
        entry.description = description.toStdString();
        entry.address     = address;
        entry.type        = slopkit::scan::ValueType::int32;
        entry.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
        table.add(entry);
    };
    add_row(QStringLiteral("first"), 0x1000);
    add_row(QStringLiteral("second"), 0x2000);
    add_row(QStringLiteral("third"), 0x3000);
    add_row(QStringLiteral("fourth"), 0x4000);

    slopkit::plugin::PluginHost            host;
    slopkit::process::PluginAccess         access {host};
    slopkit::process::AccessWorker         worker {access};
    slopkit::process::AttachedTarget       target;
    slopkit::ui::models::AddressTableModel model {table, worker, target};

    table.set_selected_rows(std::vector<std::size_t> {0, 2});
    table.set_selected(2);
    std::unique_ptr<QMimeData> payload {model.mimeData({model.index(0, 0), model.index(2, 0)})};
    REQUIRE(payload != nullptr);
    CHECK(payload->data(QStringLiteral("application/x-slopkit-address-row")).toInt() == 2);

    table.set_selected(3);
    std::unique_ptr<QMimeData> lowest {model.mimeData({model.index(0, 0), model.index(2, 0)})};
    REQUIRE(lowest != nullptr);
    CHECK(lowest->data(QStringLiteral("application/x-slopkit-address-row")).toInt() == 0);
}

TEST_CASE("the address-table model nests rows and reports their depth", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    const auto                   add_row = [&table](const QString& description, std::uint64_t address)
    {
        slopkit::table::AddressEntry entry;
        entry.description = description.toStdString();
        entry.address     = address;
        entry.type        = slopkit::scan::ValueType::int32;
        entry.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
        table.add(entry);
    };
    add_row(QStringLiteral("first"), 0x1000);
    add_row(QStringLiteral("second"), 0x2000);
    add_row(QStringLiteral("third"), 0x3000);
    add_row(QStringLiteral("fourth"), 0x4000);

    slopkit::plugin::PluginHost            host;
    slopkit::process::PluginAccess         access {host};
    slopkit::process::AccessWorker         worker {access};
    slopkit::process::AttachedTarget       target;
    slopkit::ui::models::AddressTableModel model {table, worker, target};

    const auto depth_of = [&model](int row)
    {
        return model
            .data(model.index(row, slopkit::ui::models::AddressTableModel::description),
                  slopkit::ui::widgets::kDepthRole)
            .toInt();
    };
    CHECK(model.depth_at(0) == 0);
    CHECK(depth_of(0) == 0);

    int moved_count  = 0;
    int nested_count = 0;
    QObject::connect(&model,
                     &QAbstractItemModel::rowsMoved,
                     &model,
                     [&moved_count](const QModelIndex&, int, int, const QModelIndex&, int)
                     {
                         ++moved_count;
                     });
    QObject::connect(&model,
                     &slopkit::ui::models::AddressTableModel::statusChanged,
                     &model,
                     [&nested_count](const QString& message, bool)
                     {
                         if (message == QStringLiteral("Row nested."))
                         {
                             ++nested_count;
                         }
                     });

    CHECK(model.drop_row(1, 0, true));
    CHECK(moved_count == 0);
    CHECK(nested_count == 1);
    CHECK(depth_of(0) == 0);
    CHECK(depth_of(1) == 1);
    CHECK(depth_of(2) == 0);
    CHECK(table.entries()[1].parent == table.entries()[0].id);

    CHECK_FALSE(model.drop_row(0, 1, true));
    CHECK(nested_count == 1);
    CHECK(table.entries()[0].description == "first");

    CHECK_FALSE(model.drop_row(1, 0, true));
    CHECK(nested_count == 1);

    CHECK(model.drop_row(3, 1, true));
    CHECK(moved_count == 1);
    CHECK(nested_count == 2);
    CHECK(table.entries()[2].description == "fourth");
    CHECK(depth_of(0) == 0);
    CHECK(depth_of(1) == 1);
    CHECK(depth_of(2) == 2);
    CHECK(depth_of(3) == 0);
    CHECK(table.entries()[2].parent == table.entries()[1].id);

    std::unique_ptr<QMimeData> first_payload {model.mimeData({model.index(0, 0)})};
    REQUIRE(first_payload != nullptr);
    CHECK_FALSE(model.canDropMimeData(first_payload.get(), Qt::MoveAction, -1, 0, model.index(0, 0)));
    CHECK_FALSE(model.canDropMimeData(first_payload.get(), Qt::MoveAction, -1, 0, model.index(1, 0)));
    CHECK_FALSE(model.canDropMimeData(first_payload.get(), Qt::MoveAction, -1, 0, model.index(2, 0)));
    CHECK(model.canDropMimeData(first_payload.get(), Qt::MoveAction, -1, 0, model.index(3, 0)));

    std::unique_ptr<QMimeData> third_payload {model.mimeData({model.index(3, 0)})};
    REQUIRE(third_payload != nullptr);
    CHECK(model.dropMimeData(third_payload.get(), Qt::MoveAction, -1, 0, model.index(0, 0)));
    CHECK(nested_count == 3);
    CHECK(depth_of(3) == 1);
    CHECK(table.entries()[3].parent == table.entries()[0].id);

    std::unique_ptr<QMimeData> self {model.mimeData({model.index(0, 0)})};
    REQUIRE(self != nullptr);
    CHECK_FALSE(model.dropMimeData(self.get(), Qt::MoveAction, 0, 0, QModelIndex()));
    CHECK(table.entries()[0].description == "first");
}

TEST_CASE("the address-table model edits only a script row's Description and Active cells", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    slopkit::table::AddressEntry value;
    value.description = "health";
    value.address     = 0x1040;
    value.type        = slopkit::scan::ValueType::int32;
    value.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(value);
    REQUIRE(table.add_script("helper", "print('hi')") == 1);

    slopkit::plugin::PluginHost            host;
    slopkit::process::PluginAccess         access {host};
    slopkit::process::AccessWorker         worker {access};
    slopkit::process::AttachedTarget       target;
    slopkit::ui::models::AddressTableModel model {table, worker, target};

    using slopkit::ui::models::AddressTableModel;

    CHECK(model.data(model.index(1, AddressTableModel::type), Qt::DisplayRole).toString() == QStringLiteral("script"));
    CHECK(model.data(model.index(1, AddressTableModel::address), Qt::DisplayRole).toString().isEmpty());
    CHECK(model.data(model.index(1, AddressTableModel::value), Qt::DisplayRole).toString().isEmpty());

    CHECK(model.data(model.index(1, AddressTableModel::active), Qt::CheckStateRole).toInt() == Qt::Unchecked);
    CHECK(model.flags(model.index(1, AddressTableModel::active)) & Qt::ItemIsUserCheckable);
    for (int column = 0; column < model.columnCount(); ++column)
    {
        const QModelIndex index = model.index(1, column);
        CHECK(model.flags(index) & Qt::ItemIsSelectable);
        if (column == static_cast<int>(AddressTableModel::description))
        {
            CHECK(model.flags(index) & Qt::ItemIsEditable);
        }
        else
        {
            CHECK_FALSE(model.flags(index) & Qt::ItemIsEditable);
        }
        if (column != static_cast<int>(AddressTableModel::active))
        {
            CHECK_FALSE(model.flags(index) & Qt::ItemIsUserCheckable);
        }
    }
    CHECK(model.flags(model.index(0, AddressTableModel::description)) & Qt::ItemIsEditable);
    CHECK(model.flags(model.index(0, AddressTableModel::active)) & Qt::ItemIsUserCheckable);

    std::size_t requested_row  = 99;
    bool        requested_want = false;
    int         requests       = 0;
    QObject::connect(&model,
                     &AddressTableModel::scriptActiveRequested,
                     [&](std::size_t row, bool wanted)
                     {
                         requested_row  = row;
                         requested_want = wanted;
                         ++requests;
                     });
    int         changes = 0;
    QModelIndex changed_first;
    QModelIndex changed_last;
    QObject::connect(&model,
                     &QAbstractItemModel::dataChanged,
                     [&](const QModelIndex& first, const QModelIndex& last, const QList<int>&)
                     {
                         changed_first = first;
                         changed_last  = last;
                         ++changes;
                     });

    CHECK_FALSE(model.setData(model.index(1, AddressTableModel::active), Qt::Checked, Qt::CheckStateRole));
    CHECK(requests == 1);
    CHECK(requested_row == 1);
    CHECK(requested_want);
    CHECK_FALSE(table.entries()[1].active);
    CHECK(changes == 1);
    CHECK(changed_first.row() == 1);
    CHECK(changed_last.row() == 1);

    changes       = 0;
    changed_first = QModelIndex();
    changed_last  = QModelIndex();
    CHECK(model.setData(model.index(1, AddressTableModel::description), QStringLiteral("  renamed  "), Qt::EditRole));
    CHECK(table.entries()[1].description == "renamed");
    CHECK(changes == 1);
    CHECK(changed_first.row() == 1);
    CHECK(changed_first.column() == static_cast<int>(AddressTableModel::description));

    bool blank_refused = false;
    QObject::connect(&model,
                     &AddressTableModel::statusChanged,
                     [&](const QString&, bool error)
                     {
                         blank_refused = error;
                     });
    CHECK_FALSE(model.setData(model.index(1, AddressTableModel::description), QStringLiteral("   "), Qt::EditRole));
    CHECK(blank_refused);
    CHECK(table.entries()[1].description == "renamed");

    CHECK_FALSE(model.setData(model.index(1, AddressTableModel::value), QStringLiteral("1"), Qt::EditRole));
    CHECK_FALSE(model.setData(model.index(1, AddressTableModel::address), QStringLiteral("1000"), Qt::EditRole));
    CHECK(table.entries()[1].description == "renamed");

    changes = 0;
    model.note_entry_changed(1);
    CHECK(changes == 1);
    CHECK(changed_first.row() == 1);
    CHECK(changed_last.row() == 1);
    CHECK(changed_last.column() == model.columnCount() - 1);

    const auto requests2 = model.next_live_request();
    REQUIRE(requests2.size() == 1);
    CHECK(requests2[0].address == 0x1040);
}

TEST_CASE("the address-table model copies a row's address in the three forms", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    add_int32(table, 0x1040);
    REQUIRE(table.add_script("helper", "print('hi')") == 1);

    slopkit::plugin::PluginHost            host;
    slopkit::process::PluginAccess         access {host};
    slopkit::process::AccessWorker         worker {access};
    slopkit::process::AttachedTarget       target;
    slopkit::ui::models::AddressTableModel model {table, worker, target};
    model.set_modules({module_image("app", 0x1000, 0x1000)});

    using slopkit::ui::models::CopyFormat;

    CHECK(model.copy_text(0, CopyFormat::module_relative) == QStringLiteral("app+0x40"));
    CHECK(model.copy_text(0, CopyFormat::absolute) == QStringLiteral("0x1040"));
    CHECK(model.copy_text(0, CopyFormat::address_and_value) == QStringLiteral("app+0x40: 1"));

    // Show as hex follows the cell's own rendering into the copied value.
    table.entries()[0].hex = true;
    CHECK(model.copy_text(0, CopyFormat::address_and_value) == QStringLiteral("app+0x40: 00000001"));
    table.entries()[0].hex = false;

    // A value that could not be read leaves the address alone.
    (void)model.next_live_request();
    model.apply_live_readings(std::vector<slopkit::ui::LiveReading> {
        {.id = 0, .readable = false, .bytes = {}}
    });
    CHECK(model.copy_text(0, CopyFormat::address_and_value) == QStringLiteral("app+0x40"));

    // A script row and an out-of-range row have no address to copy.
    CHECK(model.copy_text(1, CopyFormat::module_relative).isEmpty());
    CHECK(model.copy_text(2, CopyFormat::absolute).isEmpty());
}
