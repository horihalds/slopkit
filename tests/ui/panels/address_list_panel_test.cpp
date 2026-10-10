#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"
#include "table/table_zip.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>

#include "ui/components/address_table_view.hpp"
#include "ui/components/indent_delegate.hpp"
#include "ui/dialogs/add_script.hpp"

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

    // A failing resolve keeps the last good address and is dropped silently:
    // the reason never reaches the status line.
    access.unreadable->insert(module_base);
    table.entries()[0].expression = "app+0+8";
    status.clear();
    panel.refresh();
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        worker.drain();
        QApplication::processEvents();
    }
    CHECK(status.isEmpty());
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

TEST_CASE("the address list nests by drag and deletes the whole subtree", "[ui]")
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
    add("parent", 0x1040);
    add("child", 0x2040);
    add("grandchild", 0x3040);
    table.set_selected(0);

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    // The panel builds the nesting-aware view and delegate.
    auto* view = panel.findChild<slopkit::ui::components::AddressTableView*>();
    REQUIRE(view != nullptr);
    CHECK(view->showDropIndicator());
    CHECK(dynamic_cast<slopkit::ui::widgets::IndentDelegate*>(view->itemDelegate()) != nullptr);

    auto* model = qobject_cast<slopkit::ui::models::AddressTableModel*>(view->model());
    REQUIRE(model != nullptr);

    // A drag onto a row's middle nests through the model's drop path.
    CHECK(model->drop_row(1, 0, true));
    CHECK(model->drop_row(2, 1, true));
    CHECK(table.depth_of(1) == 1);
    CHECK(table.depth_of(2) == 2);

    // Deleting the parent names the whole subtree and removes it.
    table.set_selected(0);
    QString prompt;
    QTimer::singleShot(0,
                       [&prompt]
                       {
                           for (QWidget* widget : QApplication::topLevelWidgets())
                           {
                               auto* box = qobject_cast<slopkit::ui::widgets::MessageBox*>(widget);
                               if (box == nullptr || !box->isVisible())
                               {
                                   continue;
                               }
                               prompt = box->text();
                               for (QPushButton* button : box->findChildren<QPushButton*>())
                               {
                                   if (button->text() == QStringLiteral("Yes"))
                                   {
                                       button->click();
                                       return;
                                   }
                               }
                           }
                       });
    panel.delete_selected();
    CHECK(prompt == QStringLiteral("Delete parent and its 2 children?"));
    CHECK(table.size() == 0);
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

TEST_CASE("the address list row menu toggles the Active flag", "[ui]")
{
    application();

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::table::AddressTable table;
    add_int32(table, 0x1040);
    table.entries()[0].active = true;

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    QString status;
    QObject::connect(&panel,
                     &slopkit::ui::panels::AddressListPanel::statusChanged,
                     [&](const QString& message, bool)
                     {
                         status = message;
                     });

    QMenu menu;
    panel.populate_row_menu(menu, 0);

    QAction* active = nullptr;
    for (QAction* action : menu.actions())
    {
        if (action->text() == QStringLiteral("Active"))
        {
            active = action;
        }
    }
    REQUIRE(active != nullptr);
    CHECK(active->isCheckable());
    CHECK(active->isChecked() == table.entries()[0].active);

    // Triggering the entry flips the flag off and reports it on the status line.
    active->trigger();
    CHECK_FALSE(table.entries()[0].active);
    CHECK(status == QStringLiteral("Entry inactive."));
}

TEST_CASE("the address list reorders rows by dragging and saves the order", "[ui]")
{
    application();

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    // Seed a table file with three named entries.
    const auto      path = std::filesystem::path(SLOPKIT_TMP_DIR) / "address_list_panel_test" / "reorder.skt";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::remove(path);

    slopkit::table::AddressTable seed;
    const auto                   add = [&seed](const char* description, std::uint64_t address)
    {
        slopkit::table::AddressEntry entry;
        entry.description = description;
        entry.address     = address;
        entry.type        = slopkit::scan::ValueType::int32;
        entry.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
        seed.add(entry);
    };
    add("health", 0x1000);
    add("armor", 0x2000);
    add("mana", 0x3000);
    REQUIRE(slopkit::table::save(path, seed).has_value());

    slopkit::table::AddressTable          table;
    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);

    // The view accepts an internal move only.
    CHECK(view->dragDropMode() == QAbstractItemView::InternalMove);
    CHECK(view->defaultDropAction() == Qt::MoveAction);
    CHECK(view->showDropIndicator());

    REQUIRE(panel.load_table(QString::fromStdString(path.string())));
    REQUIRE(table.size() == 3);
    CHECK(table.entries()[0].description == "health");

    auto* model = view->model();
    REQUIRE(model != nullptr);

    // Move the first row below the last, as the view does on an internal move.
    std::unique_ptr<QMimeData> payload {model->mimeData({model->index(0, 0)})};
    REQUIRE(payload != nullptr);
    REQUIRE(model->dropMimeData(payload.get(), Qt::MoveAction, 3, 0, QModelIndex()));
    CHECK(table.entries()[0].description == "armor");
    CHECK(table.entries()[1].description == "mana");
    CHECK(table.entries()[2].description == "health");

    // Saving writes that row order, and a reopen restores it.
    panel.save_table();

    const auto archive = slopkit::table::read_archive(path);
    REQUIRE(archive.has_value());
    std::string index_text;
    for (const slopkit::table::ArchiveMember& member : *archive)
    {
        if (member.name == "index.txt")
        {
            index_text = member.text;
        }
    }
    CHECK(index_text == "entries/armor.txt\nentries/mana.txt\nentries/health.txt\n");

    slopkit::table::AddressTable reloaded;
    REQUIRE(slopkit::table::load(path, reloaded).has_value());
    REQUIRE(reloaded.size() == 3);
    CHECK(reloaded.entries()[0].description == "armor");
    CHECK(reloaded.entries()[1].description == "mana");
    CHECK(reloaded.entries()[2].description == "health");

    std::filesystem::remove(path);
}

TEST_CASE("the address list offers the script commands only on a script row", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    add_int32(table, 0x1040);
    REQUIRE(table.add_script("helper", "print('hi')") == 1);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    const QList<QString> table_commands {
        QStringLiteral("Add Address Manually…"), QStringLiteral("Add Script…"), QStringLiteral("Table Settings…")};

    QMenu script_menu;
    panel.populate_row_menu(script_menu, 1);
    CHECK(action_texts(script_menu.actions())
          == QList<QString> {QStringLiteral("Run Script"),
                             QStringLiteral("Edit Script…"),
                             QStringLiteral("Delete"),
                             QStringLiteral("Add Address Manually…"),
                             QStringLiteral("Add Script…"),
                             QStringLiteral("Table Settings…")});

    // A value row keeps its own commands and gains neither script command; both
    // row kinds end with the shared table-area tail.
    QMenu value_menu;
    panel.populate_row_menu(value_menu, 0);
    const QList<QString> value_texts = action_texts(value_menu.actions());
    CHECK(value_texts.contains(QStringLiteral("Change value")));
    CHECK(value_texts.contains(QStringLiteral("Browse this memory region")));
    CHECK_FALSE(value_texts.contains(QStringLiteral("Run Script")));
    CHECK_FALSE(value_texts.contains(QStringLiteral("Edit Script…")));
    CHECK(value_texts.mid(value_texts.size() - 3) == table_commands);

    // An out-of-range row gets nothing.
    QMenu empty_menu;
    panel.populate_row_menu(empty_menu, 9);
    CHECK(action_texts(empty_menu.actions()).isEmpty());
}

TEST_CASE("the address list table menu carries the table-area commands", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    add_int32(table, 0x1040);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    const QList<QString> table_commands {
        QStringLiteral("Add Address Manually…"), QStringLiteral("Add Script…"), QStringLiteral("Table Settings…")};

    // The panel menu holds exactly the three table-area commands, no separator
    // and everything enabled, exactly as the removed buttons were.
    QMenu panel_menu;
    panel.populate_panel_menu(panel_menu);
    CHECK(action_texts(panel_menu.actions()) == table_commands);
    for (QAction* action : panel_menu.actions())
    {
        CHECK_FALSE(action->isSeparator());
        CHECK(action->isEnabled());
    }

    // The two dialog requests are raised from their entries; Add Script is the
    // panel's own dialog, so it raises neither.
    int add_requests      = 0;
    int settings_requests = 0;
    QObject::connect(&panel,
                     &slopkit::ui::panels::AddressListPanel::addAddressRequested,
                     &panel,
                     [&]
                     {
                         ++add_requests;
                     });
    QObject::connect(&panel,
                     &slopkit::ui::panels::AddressListPanel::tableSettingsRequested,
                     &panel,
                     [&]
                     {
                         ++settings_requests;
                     });
    panel_menu.actions().at(0)->trigger();
    panel_menu.actions().at(2)->trigger();
    panel_menu.actions().at(1)->trigger();
    CHECK(add_requests == 1);
    CHECK(settings_requests == 1);

    // An empty table is not a dead end: the same three entries show.
    slopkit::table::AddressTable          empty_table;
    slopkit::ui::panels::AddressListPanel empty_panel {empty_table, worker, target};
    QMenu                                 empty_menu;
    empty_panel.populate_panel_menu(empty_menu);
    CHECK(action_texts(empty_menu.actions()) == table_commands);
}

TEST_CASE("the address list prefills the Add Script window from a hook request", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable          table;
    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    const QString description = QStringLiteral("Hook app+1A2B40");
    const QString source      = QStringLiteral("function activate()\n    return true\nend\n");
    panel.add_hook_script(description, source);

    auto* dialog = panel.findChild<slopkit::ui::dialogs::AddScriptDialog*>();
    REQUIRE(dialog != nullptr);
    auto* description_edit = dialog->findChild<QLineEdit*>(QStringLiteral("description_edit"));
    auto* script_edit      = dialog->findChild<QPlainTextEdit*>(QStringLiteral("script_edit"));
    REQUIRE(description_edit != nullptr);
    REQUIRE(script_edit != nullptr);
    CHECK(description_edit->text() == description);
    CHECK(script_edit->toPlainText() == source);

    // The dialog is up and the table has gained nothing yet.
    CHECK(dialog->isVisible());
    CHECK(table.empty());

    // Add is the only thing that creates the row.
    auto* commit = dialog->findChild<QPushButton*>(QStringLiteral("commit_button"));
    REQUIRE(commit != nullptr);
    commit->click();
    REQUIRE(table.size() == 1);
    CHECK(table.entries()[0].kind == slopkit::table::EntryKind::script);
    CHECK(table.entries()[0].description == description.toStdString());
    CHECK(table.entries()[0].script == source.toStdString());

    dialog->close();
}

TEST_CASE("the address list context menu covers rows and the empty area", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    add_int32(table, 0x1040);
    REQUIRE(table.add_script("helper", "print('hi')") == 1);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    panel.resize(800, 400);
    panel.show();
    QCoreApplication::processEvents();

    auto* view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);

    const QList<QString> table_commands {
        QStringLiteral("Add Address Manually…"), QStringLiteral("Add Script…"), QStringLiteral("Table Settings…")};

    // A click on a value row moves the selection there and shows its own
    // commands followed by the shared table-area tail.
    const QModelIndex value_index = view->model()->index(0, 0);
    REQUIRE_FALSE(view->visualRect(value_index).isEmpty());
    table.set_selected(-1);
    QMenu value_menu;
    panel.populate_context_menu(value_menu, view->visualRect(value_index).center());
    const QList<QString> value_texts = action_texts(value_menu.actions());
    CHECK(value_texts.contains(QStringLiteral("Change value")));
    CHECK(value_texts.mid(value_texts.size() - 3) == table_commands);
    CHECK(table.selected() == 0);

    // A click on a script row keeps its own commands plus the same tail.
    const QModelIndex script_index = view->model()->index(1, 0);
    REQUIRE_FALSE(view->visualRect(script_index).isEmpty());
    QMenu script_menu;
    panel.populate_context_menu(script_menu, view->visualRect(script_index).center());
    CHECK(action_texts(script_menu.actions())
          == QList<QString> {QStringLiteral("Run Script"),
                             QStringLiteral("Edit Script…"),
                             QStringLiteral("Delete"),
                             QStringLiteral("Add Address Manually…"),
                             QStringLiteral("Add Script…"),
                             QStringLiteral("Table Settings…")});
    CHECK(table.selected() == 1);

    // A click below the rows shows only the table commands and leaves the
    // selection exactly where it was.
    table.set_selected(0);
    const QPoint below {view->viewport()->width() / 2, view->viewport()->height() - 2};
    REQUIRE_FALSE(view->indexAt(below).isValid());
    QMenu empty_menu;
    panel.populate_context_menu(empty_menu, below);
    CHECK(action_texts(empty_menu.actions()) == table_commands);
    CHECK(table.selected() == 0);

    panel.close();
}

TEST_CASE("Run Script without an attached target is refused", "[ui]")
{
    application();

    // Nothing is attached, so the panel refuses before submitting anything.
    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::table::AddressTable table;
    REQUIRE(table.add_script("helper", "return 1") == 0);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    QString status;
    bool    is_error = false;
    QObject::connect(&panel,
                     &slopkit::ui::panels::AddressListPanel::statusChanged,
                     &panel,
                     [&](const QString& text, bool error)
                     {
                         status   = text;
                         is_error = error;
                     });

    panel.run_script(0);

    CHECK(is_error);
    CHECK(status == QStringLiteral("Run Script needs an attached target."));
}

TEST_CASE("Run Script logs its output and reports the outcome", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    REQUIRE(table.add_script("helper", "print('hello', 1)\nprint('world')") == 0);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    QString status;
    bool    is_error = true;
    QObject::connect(&panel,
                     &slopkit::ui::panels::AddressListPanel::statusChanged,
                     &panel,
                     [&](const QString& text, bool error)
                     {
                         status   = text;
                         is_error = error;
                     });

    attach_app_session(worker);

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    panel.run_script(0);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return status != QStringLiteral("Running script…");
                    }));

    // The script's own output is logged under the script category, in order.
    std::vector<std::string> script_records;
    for (const slopkit::log::Record& record : records)
    {
        if (record.category == "script")
        {
            script_records.push_back(record.message);
        }
    }
    const auto position = [&script_records](const std::string& text)
    {
        return std::find(script_records.begin(), script_records.end(), text);
    };
    REQUIRE(position("hello\t1") != script_records.end());
    REQUIRE(position("world") != script_records.end());
    CHECK(position("hello\t1") < position("world"));

    CHECK_FALSE(is_error);
    CHECK(status == QStringLiteral("Script ok."));
}

TEST_CASE("a failing script reports its first error line once", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    REQUIRE(table.add_script("helper", "error('boom')") == 0);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    QString status;
    bool    is_error = false;
    QObject::connect(&panel,
                     &slopkit::ui::panels::AddressListPanel::statusChanged,
                     &panel,
                     [&](const QString& text, bool error)
                     {
                         status   = text;
                         is_error = error;
                     });

    attach_app_session(worker);

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    panel.run_script(0);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return is_error;
                    }));

    CHECK(status.startsWith(QStringLiteral("Script failed: ")));
    CHECK(status.contains(QStringLiteral("boom")));
    // The traceback does not reach the one-line status or the log record.
    CHECK_FALSE(status.contains(QChar('\n')));

    int failures = 0;
    for (const slopkit::log::Record& record : records)
    {
        if (record.category == "script" && record.level == slopkit::log::Level::warning)
        {
            ++failures;
            CHECK(record.message.find("boom") != std::string::npos);
            CHECK(record.message.find('\n') == std::string::npos);
        }
    }
    CHECK(failures == 1);
}

namespace
{
    // The status text and its error flag, fed by the panel's signal.
    struct ScriptStatus
    {
        QString text;
        bool    is_error {false};
    };

    void watch_script_status(slopkit::ui::panels::AddressListPanel& panel, ScriptStatus& status)
    {
        QObject::connect(&panel,
                         &slopkit::ui::panels::AddressListPanel::statusChanged,
                         [&status](const QString& text, bool error)
                         {
                             status.text     = text;
                             status.is_error = error;
                         });
    }
} // namespace

TEST_CASE("the script row's Active checkbox runs activate and deactivate", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    REQUIRE(table.add_script("helper",
                             R"(
function activate() return true, "prepared" end
function deactivate() return true end
)") == 0);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = view->model();
    REQUIRE(model != nullptr);

    ScriptStatus status;
    watch_script_status(panel, status);
    attach_app_session(worker);

    const QModelIndex active_index = model->index(0, slopkit::ui::models::AddressTableModel::active);
    REQUIRE(model->data(active_index, Qt::CheckStateRole).toInt() == Qt::Unchecked);

    // Ticking submits one activate job and leaves the flag for the verdict.
    CHECK_FALSE(model->setData(active_index, Qt::Checked, Qt::CheckStateRole));
    CHECK(status.text == QStringLiteral("Activating…"));
    CHECK_FALSE(table.entries()[0].active);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return status.text != QStringLiteral("Activating…");
                    }));
    CHECK_FALSE(status.is_error);
    CHECK(status.text == QStringLiteral("Script active: prepared"));
    CHECK(table.entries()[0].active);
    CHECK(model->data(active_index, Qt::CheckStateRole).toInt() == Qt::Checked);

    // Unticking runs deactivate() and clears the flag again.
    CHECK_FALSE(model->setData(active_index, Qt::Unchecked, Qt::CheckStateRole));
    CHECK(status.text == QStringLiteral("Deactivating…"));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return status.text != QStringLiteral("Deactivating…");
                    }));
    CHECK_FALSE(status.is_error);
    CHECK(status.text == QStringLiteral("Script inactive."));
    CHECK_FALSE(table.entries()[0].active);
}

TEST_CASE("a failed update tick clears the Active box and reports it once", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    REQUIRE(table.add_script("helper",
                             R"(
function activate() return true end
function deactivate() return true end
function update() end
)") == 0);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = view->model();
    REQUIRE(model != nullptr);

    ScriptStatus status;
    watch_script_status(panel, status);
    attach_app_session(worker);

    const QModelIndex active_index = model->index(0, slopkit::ui::models::AddressTableModel::active);
    const auto        failure      = slopkit::process::ScriptUpdateFailure {.description = "helper", .reason = "nope"};

    // True when any captured record's message contains `text`.
    const auto has_record = [](const std::vector<slopkit::log::Record>& records, std::string_view text)
    {
        for (const auto& record : records)
        {
            if (record.message.find(text) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    };

    SECTION("a ticked row is cleared with one warning and a status line")
    {
        CHECK_FALSE(model->setData(active_index, Qt::Checked, Qt::CheckStateRole));
        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return table.entries()[0].active;
                        }));
        REQUIRE(worker.active_scripts() == 1);

        std::vector<slopkit::log::Record> records;
        SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                                {
                            records.push_back(record);
                                                }};

        panel.note_script_update_failed(failure);

        CHECK_FALSE(table.entries()[0].active);
        CHECK(model->data(active_index, Qt::CheckStateRole).toInt() == Qt::Unchecked);
        CHECK(status.is_error);
        CHECK(status.text == QStringLiteral("Update failed: nope"));

        std::vector<slopkit::log::Record> script_records;
        for (const auto& record : records)
        {
            if (record.category == "script")
            {
                script_records.push_back(record);
            }
        }
        REQUIRE(script_records.size() == 1);
        CHECK(script_records[0].message == "script 'helper' update failed: nope");

        // No deactivate job was submitted: the status stayed on the report and
        // the worker still tracks the script.
        CHECK(worker.active_scripts() == 1);
    }

    SECTION("a failure for a row that is no longer active is only logged")
    {
        // The row was never ticked (or a detach already cleared it): the flag
        // and the status line are left alone, the record is still written.
        std::vector<slopkit::log::Record> records;
        SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                                {
                            records.push_back(record);
                                                }};

        panel.note_script_update_failed(failure);

        CHECK_FALSE(table.entries()[0].active);
        CHECK(model->data(active_index, Qt::CheckStateRole).toInt() == Qt::Unchecked);
        CHECK(status.text.isEmpty());
        CHECK(has_record(records, "script 'helper' update failed: nope"));
    }

    SECTION("a manual hook job in flight is not disturbed")
    {
        CHECK_FALSE(model->setData(active_index, Qt::Checked, Qt::CheckStateRole));
        REQUIRE(status.text == QStringLiteral("Activating…")); // the activate job is in flight

        std::vector<slopkit::log::Record> records;
        SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                                {
                            records.push_back(record);
                                                }};

        panel.note_script_update_failed(failure);

        // The in-flight activate owns the row and the status line.
        CHECK_FALSE(table.entries()[0].active);
        CHECK(status.text == QStringLiteral("Activating…"));
        CHECK(has_record(records, "script 'helper' update failed: nope"));

        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return table.entries()[0].active;
                        }));
    }
}

TEST_CASE("a refused activation reverts the checkbox", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    REQUIRE(table.add_script("helper", "function activate() return false, \"blocked\" end") == 0);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = view->model();

    ScriptStatus status;
    watch_script_status(panel, status);
    attach_app_session(worker);

    const QModelIndex active_index = model->index(0, slopkit::ui::models::AddressTableModel::active);
    CHECK_FALSE(model->setData(active_index, Qt::Checked, Qt::CheckStateRole));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return status.text != QStringLiteral("Activating…");
                    }));

    CHECK(status.is_error);
    CHECK(status.text == QStringLiteral("Activate failed: blocked"));
    CHECK_FALSE(table.entries()[0].active);
    CHECK(model->data(active_index, Qt::CheckStateRole).toInt() == Qt::Unchecked);
}

TEST_CASE("a script without the hook reports the refusal", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    REQUIRE(table.add_script("helper", "return 1") == 0);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = view->model();

    ScriptStatus status;
    watch_script_status(panel, status);
    attach_app_session(worker);

    CHECK_FALSE(model->setData(
        model->index(0, slopkit::ui::models::AddressTableModel::active), Qt::Checked, Qt::CheckStateRole));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return status.text != QStringLiteral("Activating…");
                    }));

    CHECK(status.is_error);
    CHECK(status.text == QStringLiteral("Activate failed: activate() is not defined"));
    CHECK_FALSE(table.entries()[0].active);
}

TEST_CASE("toggling a script row without a target is refused", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target; // nothing is attached

    slopkit::table::AddressTable table;
    REQUIRE(table.add_script("helper", "function activate() return true end") == 0);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = view->model();

    ScriptStatus status;
    watch_script_status(panel, status);

    CHECK_FALSE(model->setData(
        model->index(0, slopkit::ui::models::AddressTableModel::active), Qt::Checked, Qt::CheckStateRole));

    CHECK(status.is_error);
    CHECK(status.text == QStringLiteral("Activate needs an attached target."));
    CHECK_FALSE(table.entries()[0].active);
}

TEST_CASE("a second script toggle while one is in flight is refused", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    REQUIRE(table.add_script("helper", "function activate() return true end") == 0);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = view->model();

    ScriptStatus status;
    watch_script_status(panel, status);
    attach_app_session(worker);

    const QModelIndex active_index = model->index(0, slopkit::ui::models::AddressTableModel::active);
    CHECK_FALSE(model->setData(active_index, Qt::Checked, Qt::CheckStateRole)); // in flight
    CHECK(status.text == QStringLiteral("Activating…"));
    CHECK_FALSE(model->setData(active_index, Qt::Unchecked, Qt::CheckStateRole)); // refused

    CHECK(status.is_error);
    CHECK(status.text == QStringLiteral("A script is already running."));

    // Let the first job finish so nothing is left pending.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return status.text != QStringLiteral("A script is already running.");
                    }));
    CHECK(status.text == QStringLiteral("Script active."));
    CHECK(table.entries()[0].active);
}

TEST_CASE("the Active command routes a script row through its hooks", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    REQUIRE(table.add_script("helper", "function activate() return true end") == 0);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    ScriptStatus status;
    watch_script_status(panel, status);
    attach_app_session(worker);

    table.set_selected(0);
    panel.toggle_active_selected();

    CHECK(status.text == QStringLiteral("Activating…"));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return status.text != QStringLiteral("Activating…");
                    }));
    CHECK(status.text == QStringLiteral("Script active."));
    CHECK(table.entries()[0].active);
}

TEST_CASE("the Active command still flips a value row directly", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::table::AddressTable table;
    add_int32(table, 0x1040);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    ScriptStatus status;
    watch_script_status(panel, status);

    table.set_selected(0);
    panel.toggle_active_selected();

    CHECK(table.entries()[0].active);
    CHECK(status.text == QStringLiteral("Entry active."));
}

TEST_CASE("detaching clears active script rows only once", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target; // detached

    slopkit::table::AddressTable table;
    add_int32(table, 0x1040);
    REQUIRE(table.add_script("helper", "function activate() return true end") == 1);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    ScriptStatus status;
    watch_script_status(panel, status);

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    // A row left ticked while the target goes away would be a lie.
    table.entries()[0].active = true;
    table.entries()[1].active = true;

    const auto detached_records = [&records]
    {
        int count = 0;
        for (const slopkit::log::Record& record : records)
        {
            if (record.category == "script" && record.message.find("target detached") != std::string::npos)
            {
                ++count;
            }
        }
        return count;
    };

    panel.refresh();

    CHECK(table.entries()[0].active); // value rows keep their flag
    CHECK_FALSE(table.entries()[1].active);
    CHECK(status.text == QStringLiteral("Scripts deactivated: the target detached."));
    CHECK(detached_records() == 1);

    // The second tick finds nothing left to clear and writes no record.
    panel.refresh();
    CHECK(detached_records() == 1);
}

TEST_CASE("a script completion for a deleted row is harmless", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::table::AddressTable table;
    REQUIRE(table.add_script("helper", "function activate() return true end") == 0);

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = view->model();

    ScriptStatus status;
    watch_script_status(panel, status);
    attach_app_session(worker);

    CHECK_FALSE(model->setData(
        model->index(0, slopkit::ui::models::AddressTableModel::active), Qt::Checked, Qt::CheckStateRole));

    // The row vanishes between the click and the verdict.
    table.remove(0);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return status.text != QStringLiteral("Activating…");
                    }));
    CHECK(status.text == QStringLiteral("Script active."));
    CHECK(table.empty());
}

TEST_CASE("the address list renames a script row from its Description cell", "[ui]")
{
    application();

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    // Seed a table file with one script row so a rename can be saved and read back.
    const auto      path = std::filesystem::path(SLOPKIT_TMP_DIR) / "address_list_panel_test" / "script_rename.skt";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::remove(path);

    slopkit::table::AddressTable seed;
    REQUIRE(seed.add_script("helper", "return 1") == 0);
    REQUIRE(slopkit::table::save(path, seed).has_value());

    slopkit::table::AddressTable          table;
    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    auto*                                 view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(panel.load_table(QString::fromStdString(path.string())));
    REQUIRE(table.size() == 1);

    auto* model = view->model();
    REQUIRE(model != nullptr);
    const QModelIndex description = model->index(0, slopkit::ui::models::AddressTableModel::description);

    // The Description cell is editable and a double-click opens the editor,
    // exactly as it does for a value row.
    CHECK(model->flags(description) & Qt::ItemIsEditable);
    CHECK(view->editTriggers().testFlag(QAbstractItemView::DoubleClicked));
    CHECK(view->editTriggers().testFlag(QAbstractItemView::EditKeyPressed));
    panel.show();
    view->setCurrentIndex(description);
    view->edit(description);
    CHECK(view->viewport()->findChild<QLineEdit*>() != nullptr); // the editor opened

    // Committing a name renames the entry and survives a Save Table / reopen.
    CHECK(model->setData(description, QStringLiteral("renamed"), Qt::EditRole));
    CHECK(table.entries()[0].description == "renamed");

    panel.save_table();
    slopkit::table::AddressTable reloaded;
    REQUIRE(slopkit::table::load(path, reloaded).has_value());
    REQUIRE(reloaded.size() == 1);
    CHECK(reloaded.entries()[0].description == "renamed");

    std::filesystem::remove(path);
}
