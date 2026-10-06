#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the address list Value column follows live memory", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::ui::SettingsController  settings {scratch_settings_file("live_address.ini")};

    slopkit::table::AddressTable table;
    add_int32(table, 0x1040);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = view->model();
    REQUIRE(model != nullptr);

    attach_app_session(worker);

    const QModelIndex value_index = model->index(0, slopkit::ui::models::AddressTableModel::value);
    const auto        value_text  = [&]()
    {
        return model->data(value_index, Qt::DisplayRole).toString();
    };
    const auto foreground = [&]()
    {
        return model->data(value_index, Qt::ForegroundRole).value<QColor>();
    };

    slopkit::ui::LiveValues live {worker, target, settings};
    live.add_surface(&panel);

    // The stored value shows until the first reading lands.
    CHECK(value_text() == QStringLiteral("1"));

    (*access.memory)[0x1040] = {std::byte {7}, std::byte {0}, std::byte {0}, std::byte {0}};
    live.request_now();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return value_text() == QStringLiteral("7");
                    }));

    // A changed reading is flagged until the next change.
    (*access.memory)[0x1040][0] = std::byte {9};
    live.request_now();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return value_text() == QStringLiteral("9");
                    }));
    CHECK(foreground() == slopkit::ui::active_theme().warning);

    // A settled read clears the highlight.
    live.request_now();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return foreground() != slopkit::ui::active_theme().warning;
                    }));
    CHECK(value_text() == QStringLiteral("9"));
}

TEST_CASE("the address list re-resolves stored expressions on a slower cadence", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target      = fake_target();
    constexpr std::uint64_t          module_base = 0x100000;

    const auto store_pointer = [&](std::uint64_t value)
    {
        std::vector<std::byte> bytes(8);
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            bytes[index] = static_cast<std::byte>((value >> (8 * index)) & 0xFF);
        }
        (*access.memory)[module_base] = std::move(bytes);
    };
    store_pointer(0x200000);

    slopkit::table::AddressTable table;
    slopkit::table::AddressEntry chained;
    chained.expression = "app+0+8";
    chained.type       = slopkit::scan::ValueType::int32;
    chained.bytes      = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(chained);
    slopkit::table::AddressEntry relative;
    relative.expression = "app+20";
    relative.type       = slopkit::scan::ValueType::int32;
    relative.bytes      = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(relative);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = view->model();
    REQUIRE(model != nullptr);

    int data_changes = 0;
    QObject::connect(model,
                     &QAbstractItemModel::dataChanged,
                     [&](const QModelIndex&, const QModelIndex&, const QList<int>&)
                     {
                         ++data_changes;
                     });
    QString status;
    QObject::connect(&panel,
                     &slopkit::ui::panels::AddressListPanel::statusChanged,
                     [&](const QString& message, bool)
                     {
                         status = message;
                     });

    attach_app_session(worker);
    panel.set_modules({module_image("app", module_base, 0x1000)});

    // One refresh resolves the whole batch: the chain dereferences, the
    // module-relative entry is a plain offset.
    panel.refresh();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table.entries()[0].address == 0x200008 && table.entries()[1].address == 0x100020;
                    }));
    CHECK(data_changes > 0);
    const QModelIndex address_index = model->index(0, slopkit::ui::models::AddressTableModel::address);
    CHECK(model->data(address_index, Qt::DisplayRole).toString() == QStringLiteral("app+0+8"));
    CHECK(model->data(address_index, Qt::ToolTipRole).toString() == QStringLiteral("200008"));

    // A second refresh inside the interval resolves nothing: moving the pointer
    // and refreshing must leave the stored address untouched.
    store_pointer(0x300000);
    panel.refresh();
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        worker.drain();
        QApplication::processEvents();
    }
    CHECK(table.entries()[0].address == 0x200008);

    // An edited expression resolves at once instead of waiting for the interval.
    table.entries()[0].expression = "app+0+4";
    panel.refresh();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table.entries()[0].address == 0x300004;
                    }));

    // A failing resolve keeps the last good address and reports the reason.
    access.unreadable->insert(module_base);
    table.entries()[0].expression = "app+0+8";
    status.clear();
    panel.refresh();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return status.contains(QStringLiteral("cannot read pointer"));
                    }));
    CHECK(table.entries()[0].address == 0x300004);
}

TEST_CASE("an unreadable address shows a question mark", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::ui::SettingsController  settings {scratch_settings_file("live_unreadable.ini")};

    slopkit::table::AddressTable table;
    add_int32(table, 0x1040);
    add_int32(table, 0x2000);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = view->model();
    REQUIRE(model != nullptr);

    attach_app_session(worker);

    const auto value_text = [&](int row)
    {
        return model->data(model->index(row, slopkit::ui::models::AddressTableModel::value), Qt::DisplayRole)
            .toString();
    };

    slopkit::ui::LiveValues live {worker, target, settings};
    live.add_surface(&panel);

    (*access.memory)[0x1040] = {std::byte {5}, std::byte {0}, std::byte {0}, std::byte {0}};
    access.unreadable->insert(0x2000);

    live.request_now();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return value_text(0) == QStringLiteral("5") && value_text(1) == QStringLiteral("?");
                    }));

    // The failing row is muted and explains itself; the readable one is not.
    const QModelIndex failing = model->index(1, slopkit::ui::models::AddressTableModel::value);
    CHECK(model->data(failing, Qt::ForegroundRole).value<QColor>() == slopkit::ui::active_theme().text_muted);
    CHECK(model->data(failing, Qt::ToolTipRole).toString() == QStringLiteral("Not readable"));
    CHECK(
        model->data(model->index(0, slopkit::ui::models::AddressTableModel::value), Qt::ForegroundRole).value<QColor>()
        != slopkit::ui::active_theme().text_muted);
}

TEST_CASE("a value being written is left out of the live pass", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::ui::SettingsController  settings {scratch_settings_file("live_writing.ini")};

    slopkit::table::AddressTable table;
    add_int32(table, 0x1040);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = view->model();
    REQUIRE(model != nullptr);

    attach_app_session(worker);

    const QModelIndex value_index = model->index(0, slopkit::ui::models::AddressTableModel::value);
    const auto        value_text  = [&]()
    {
        return model->data(value_index, Qt::DisplayRole).toString();
    };

    slopkit::ui::LiveValues live {worker, target, settings};
    live.add_surface(&panel);

    (*access.memory)[0x1040] = {std::byte {2}, std::byte {0}, std::byte {0}, std::byte {0}};
    live.request_now();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return value_text() == QStringLiteral("2");
                    }));

    // A write in flight keeps its placeholder and is absent from the pass.
    REQUIRE(model->setData(value_index, QStringLiteral("99"), Qt::EditRole));
    CHECK(value_text() == QStringLiteral("Writing..."));
    CHECK(panel.next_live_request().empty());

    // The completion shows the written value without a change highlight.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return value_text() == QStringLiteral("99");
                    }));
    CHECK(model->data(value_index, Qt::ForegroundRole).value<QColor>() != slopkit::ui::active_theme().warning);
}

TEST_CASE("the address list delete confirmation follows the address mode", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    slopkit::table::AddressEntry entry;
    entry.address = 0x1040;
    entry.type    = slopkit::scan::ValueType::int32;
    entry.bytes   = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(entry);
    table.set_selected(0);

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    // Dismisses the modal confirmation and returns its text. `delete_selected`
    // is synchronous, so the timer fires inside the box's nested event loop.
    const auto confirmation_text = [&panel]()
    {
        QString prompt;
        QTimer::singleShot(0,
                           [&prompt]
                           {
                               for (QWidget* widget : QApplication::topLevelWidgets())
                               {
                                   if (auto* box = qobject_cast<slopkit::ui::widgets::MessageBox*>(widget);
                                       box != nullptr && box->isVisible())
                                   {
                                       prompt = box->text();
                                       box->reject();
                                       return;
                                   }
                               }
                           });
        panel.delete_selected();
        return prompt;
    };

    // No module map yet: the confirmation echoes the absolute address.
    CHECK(confirmation_text() == QStringLiteral("Delete 1040?"));

    // Inside a module image the confirmation echoes module+RVA.
    panel.set_modules({module_image("app", 0x1000, 0x1000)});
    CHECK(confirmation_text() == QStringLiteral("Delete app+40?"));

    // Absolute mode switches the confirmation back.
    panel.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(confirmation_text() == QStringLiteral("Delete 1040?"));
}

TEST_CASE("the address list row menu offers the access watch entries", "[ui]")
{
    application();

    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};

    slopkit::table::AddressTable table;
    add_int32(table, 0x1040);

    QAction*   watch_writes       = nullptr;
    QAction*   watch_accesses     = nullptr;
    const auto find_watch_actions = [&](QMenu& menu)
    {
        watch_writes   = nullptr;
        watch_accesses = nullptr;
        for (QAction* action : menu.actions())
        {
            if (action->text() == QStringLiteral("Find out what writes this address"))
            {
                watch_writes = action;
            }
            if (action->text() == QStringLiteral("Find out what accesses this address"))
            {
                watch_accesses = action;
            }
        }
    };

    // Without a target the entries are present but disabled.
    slopkit::process::AttachedTarget      none;
    slopkit::ui::panels::AddressListPanel detached_panel {table, worker, none};
    QMenu                                 detached_menu;
    detached_panel.populate_row_menu(detached_menu, 0);
    find_watch_actions(detached_menu);
    CHECK(detached_menu.toolTipsVisible());
    REQUIRE(watch_writes != nullptr);
    REQUIRE(watch_accesses != nullptr);
    CHECK_FALSE(watch_writes->isEnabled());
    CHECK_FALSE(watch_accesses->isEnabled());
    CHECK(watch_writes->toolTip() == QStringLiteral("Attach to a target first."));

    // With a target attached they emit the entry's address and width, with the
    // writes or the read/write kind.
    slopkit::process::AttachedTarget      target = fake_target();
    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    std::uint64_t        requested_address = 0;
    std::size_t          requested_width   = 0;
    slopkit::debug::Kind requested_kind    = slopkit::debug::Kind::software;
    int                  requests          = 0;
    QObject::connect(&panel,
                     &slopkit::ui::panels::AddressListPanel::accessWatchRequested,
                     &panel,
                     [&](std::uint64_t address, std::size_t width, slopkit::debug::Kind kind)
                     {
                         requested_address = address;
                         requested_width   = width;
                         requested_kind    = kind;
                         ++requests;
                     });

    QMenu menu;
    panel.populate_row_menu(menu, 0);
    find_watch_actions(menu);
    CHECK(menu.toolTipsVisible());
    REQUIRE(watch_writes != nullptr);
    REQUIRE(watch_accesses != nullptr);
    CHECK(watch_writes->isEnabled());
    CHECK(watch_accesses->isEnabled());

    watch_writes->trigger();
    REQUIRE(requests == 1);
    CHECK(requested_address == 0x1040);
    CHECK(requested_width == 4);
    CHECK(requested_kind == slopkit::debug::Kind::hardware_write);

    watch_accesses->trigger();
    REQUIRE(requests == 2);
    CHECK(requested_kind == slopkit::debug::Kind::hardware_read_write);
}
