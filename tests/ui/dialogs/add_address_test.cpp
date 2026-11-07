#include <catch2/catch.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include "process/access.hpp"
#include "process/access_worker.hpp"
#include "process/types.hpp"
#include "scan/types.hpp"
#include "script/symbols.hpp"
#include "support/fake_process.hpp"
#include "table/address_table.hpp"
#include "ui/components/widgets.hpp"
#include "ui/dialogs/add_address.hpp"

namespace
{
    using slopkit::test::application;
    using slopkit::test::FakeAccess;
    using slopkit::test::FakeBackend;
    using slopkit::test::FakeMemory;
    using slopkit::test::module_image;
    using slopkit::test::pump_ui;

    // The shared QApplication already exists; kept for the cases' readability.
    void ensure_application()
    {
        static_cast<void>(application());
    }

    using slopkit::scan::ValueType;
    using slopkit::table::AddressTable;
    using slopkit::ui::dialogs::AddAddressDialog;

    void attach_session(slopkit::process::AccessWorker& worker)
    {
        bool attached = false;
        worker.submit_attach_app(worker.next_job_id(),
                                 42,
                                 "fake",
                                 [&](slopkit::process::JobResult&&)
                                 {
                                     attached = true;
                                 });
        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return attached;
                        }));
    }

    void store_pointer(FakeMemory& memory, std::uint64_t address, std::uint64_t value)
    {
        std::vector<std::byte> bytes(8);
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            bytes[index] = static_cast<std::byte>((value >> (8 * index)) & 0xFF);
        }
        memory[address] = std::move(bytes);
    }

    QLineEdit* edit_named(AddAddressDialog& dialog, const QString& name)
    {
        return dialog.findChild<QLineEdit*>(name);
    }

    QPushButton* button_labelled(AddAddressDialog& dialog, const QString& label)
    {
        for (QPushButton* button : dialog.findChildren<QPushButton*>())
        {
            if (button->text() == label)
            {
                return button;
            }
        }
        return nullptr;
    }

    slopkit::ui::widgets::StatusLabel* status_label_of(AddAddressDialog& dialog)
    {
        return dialog.findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("status_label"));
    }

    slopkit::ui::widgets::Panel* panel_titled(AddAddressDialog& dialog, const QString& title)
    {
        for (QLabel* label : dialog.findChildren<QLabel*>())
        {
            if (label->text() == title)
            {
                return qobject_cast<slopkit::ui::widgets::Panel*>(label->parentWidget());
            }
        }
        return nullptr;
    }
}

TEST_CASE("the add address dialog resets its form every time it is shown", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    auto* description = edit_named(dialog, QStringLiteral("description_edit"));
    auto* address     = edit_named(dialog, QStringLiteral("address_edit"));
    auto* size        = edit_named(dialog, QStringLiteral("size_edit"));
    auto* type        = dialog.findChild<QComboBox*>(QStringLiteral("type_combo"));
    auto* hex         = dialog.findChild<QCheckBox*>(QStringLiteral("hex_check"));
    REQUIRE(description != nullptr);
    REQUIRE(address != nullptr);
    REQUIRE(size != nullptr);
    REQUIRE(type != nullptr);
    REQUIRE(hex != nullptr);

    auto* entry_panel = panel_titled(dialog, QStringLiteral("Address entry"));
    auto* value_panel = panel_titled(dialog, QStringLiteral("Value format"));
    auto* status      = status_label_of(dialog);
    REQUIRE(entry_panel != nullptr);
    REQUIRE(value_panel != nullptr);
    REQUIRE(status != nullptr);
    CHECK(status->text().isEmpty());
    CHECK(status->parentWidget() == &dialog);
    CHECK(entry_panel->isAncestorOf(address));
    CHECK(value_panel->isAncestorOf(type));

    dialog.show();
    QApplication::processEvents();
    CHECK(description->text() == QStringLiteral("New address"));
    CHECK(address->text().isEmpty());
    CHECK(type->currentIndex() == static_cast<int>(ValueType::int32));
    CHECK(size->text() == QStringLiteral("32"));
    CHECK_FALSE(hex->isChecked());

    // Dirty every field, then reopen: the previous input must be gone.
    description->setText(QStringLiteral("Custom"));
    address->setText(QStringLiteral("0x2000"));
    type->setCurrentIndex(static_cast<int>(ValueType::string));
    size->setText(QStringLiteral("128"));
    hex->setChecked(true);

    dialog.hide();
    dialog.show();
    QApplication::processEvents();

    CHECK(description->text() == QStringLiteral("New address"));
    CHECK(address->text().isEmpty());
    CHECK(type->currentIndex() == static_cast<int>(ValueType::int32));
    CHECK(size->text() == QStringLiteral("32"));
    CHECK_FALSE(hex->isChecked());
}

TEST_CASE("confirming an address adds it and closes the dialog", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    auto* description = edit_named(dialog, QStringLiteral("description_edit"));
    auto* address     = edit_named(dialog, QStringLiteral("address_edit"));
    auto* add         = button_labelled(dialog, QStringLiteral("Add"));
    REQUIRE(description != nullptr);
    REQUIRE(address != nullptr);
    REQUIRE(add != nullptr);

    dialog.show();
    QApplication::processEvents();
    CHECK(dialog.isVisible());

    description->setText(QStringLiteral("Health"));
    address->setText(QStringLiteral("0x4000"));
    add->click();

    CHECK_FALSE(dialog.isVisible());
    REQUIRE(table.size() == 1);
    const auto& entry = table.entries().front();
    CHECK(entry.description == "Health");
    CHECK(entry.address == 0x4000);
    CHECK(entry.expression == "0x4000");
    CHECK(entry.type == ValueType::int32);
}

TEST_CASE("the add address dialog accepts a module expression and stores it", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    dialog.set_modules({module_image("app", 0x100000, 0x1000)});

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* add     = button_labelled(dialog, QStringLiteral("Add"));
    REQUIRE(address != nullptr);
    REQUIRE(add != nullptr);

    dialog.show();
    QApplication::processEvents();

    address->setText(QStringLiteral("APP+0x10"));
    add->click();

    CHECK_FALSE(dialog.isVisible());
    REQUIRE(table.size() == 1);
    CHECK(table.entries().front().expression == "APP+0x10");
    CHECK(table.entries().front().address == 0x100010);
}

TEST_CASE("the add address dialog resolves a symbol name", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    slopkit::script::SymbolTable   symbols;
    REQUIRE(symbols.set("hp", 0x1337).has_value());
    AddAddressDialog dialog {table, worker, nullptr, symbols};

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* add     = button_labelled(dialog, QStringLiteral("Add"));
    REQUIRE(address != nullptr);
    REQUIRE(add != nullptr);

    dialog.show();
    QApplication::processEvents();

    address->setText(QStringLiteral("hp + 8"));
    add->click();

    CHECK_FALSE(dialog.isVisible());
    REQUIRE(table.size() == 1);
    CHECK(table.entries().front().expression == "hp + 8");
    CHECK(table.entries().front().address == 0x133F);
}

TEST_CASE("a pointer-chain expression resolves through the worker before the entry is added", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    attach_session(worker);
    store_pointer(*access.memory, 0x100000, 0x200000);

    AddAddressDialog dialog {table, worker};
    dialog.set_modules({module_image("app", 0x100000, 0x1000)});

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* add     = button_labelled(dialog, QStringLiteral("Add"));
    REQUIRE(address != nullptr);
    REQUIRE(add != nullptr);

    dialog.show();
    QApplication::processEvents();

    address->setText(QStringLiteral("app+0+8"));
    add->click();

    CHECK_FALSE(add->isEnabled());
    CHECK(table.size() == 0);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table.size() == 1;
                    }));
    CHECK(table.entries().front().expression == "app+0+8");
    CHECK(table.entries().front().address == 0x200008);
    CHECK(add->isEnabled());
}

TEST_CASE("an unresolvable expression is still added", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* add     = button_labelled(dialog, QStringLiteral("Add"));
    REQUIRE(address != nullptr);
    REQUIRE(add != nullptr);

    dialog.show();
    QApplication::processEvents();

    // `not-an-address` parses as a bare base token but does not resolve: it is a
    // resolve failure, so the row is added unresolved instead of refused.
    address->setText(QStringLiteral("not-an-address"));
    add->click();

    CHECK_FALSE(dialog.isVisible());
    REQUIRE(table.size() == 1);
    CHECK(table.entries().front().expression == "not-an-address");
    CHECK(table.entries().front().address == 0);
}

TEST_CASE("a pointer chain with no target is still added unresolved", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* add     = button_labelled(dialog, QStringLiteral("Add"));
    REQUIRE(address != nullptr);
    REQUIRE(add != nullptr);

    dialog.show();
    QApplication::processEvents();

    address->setText(QStringLiteral("[120000+10]"));
    add->click();

    CHECK_FALSE(add->isEnabled());
    CHECK(table.size() == 0);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table.size() == 1;
                    }));
    CHECK_FALSE(dialog.isVisible());
    CHECK(table.entries().front().expression == "[120000+10]");
    CHECK(table.entries().front().address == 0);
    CHECK(add->isEnabled());
}

TEST_CASE("a malformed address keeps the add address dialog open", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* add     = button_labelled(dialog, QStringLiteral("Add"));
    auto* status  = status_label_of(dialog);
    REQUIRE(address != nullptr);
    REQUIRE(add != nullptr);
    REQUIRE(status != nullptr);

    dialog.show();
    QApplication::processEvents();

    address->setText(QStringLiteral("app+"));
    add->click();

    CHECK(dialog.isVisible());
    CHECK(table.size() == 0);
    CHECK(status->text().contains(QStringLiteral("Address:")));

    address->clear();
    add->click();

    CHECK(dialog.isVisible());
    CHECK(table.size() == 0);
    CHECK(status->text().contains(QStringLiteral("Address:")));
}

TEST_CASE("the add address dialog previews the resolved address and value", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    attach_session(worker);
    (*access.memory)[0x100010] = {std::byte {42}, std::byte {0}, std::byte {0}, std::byte {0}};

    AddAddressDialog dialog {table, worker};
    dialog.set_modules({module_image("app", 0x100000, 0x1000)});

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* hex     = dialog.findChild<QCheckBox*>(QStringLiteral("hex_check"));
    auto* preview = dialog.findChild<QLabel*>(QStringLiteral("preview_label"));
    REQUIRE(address != nullptr);
    REQUIRE(hex != nullptr);
    REQUIRE(preview != nullptr);

    dialog.show();
    QApplication::processEvents();

    address->setText(QStringLiteral("APP+0x10"));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return preview->text().contains(QStringLiteral("42"));
                    }));
    CHECK(preview->text().contains(QStringLiteral("app+0x10")));

    hex->setChecked(true);
    CHECK(preview->text().contains(QStringLiteral("0000002A")));
}

TEST_CASE("the add address dialog previews an unresolved expression", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* add     = button_labelled(dialog, QStringLiteral("Add"));
    auto* preview = dialog.findChild<QLabel*>(QStringLiteral("preview_label"));
    REQUIRE(address != nullptr);
    REQUIRE(add != nullptr);
    REQUIRE(preview != nullptr);

    dialog.show();
    QApplication::processEvents();

    address->setText(QStringLiteral("not-an-address"));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return preview->text().contains(QStringLiteral("unknown module or literal"));
                    }));

    add->click();
    CHECK_FALSE(dialog.isVisible());
    REQUIRE(table.size() == 1);
    CHECK(table.entries().front().expression == "not-an-address");
    CHECK(table.entries().front().address == 0);
}

TEST_CASE("the add address dialog clears and re-resolves the preview", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};
    dialog.set_modules({module_image("app", 0x100000, 0x1000)});

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* preview = dialog.findChild<QLabel*>(QStringLiteral("preview_label"));
    REQUIRE(address != nullptr);
    REQUIRE(preview != nullptr);

    dialog.show();
    QApplication::processEvents();

    address->setText(QStringLiteral("APP+0x10"));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return preview->text().contains(QStringLiteral("app+0x10"));
                    }));

    dialog.hide();
    dialog.show();
    QApplication::processEvents();
    CHECK(preview->text().isEmpty());

    address->setText(QStringLiteral("game+0x10"));
    dialog.set_modules({module_image("game", 0x200000, 0x1000)});
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return preview->text().contains(QStringLiteral("game+0x10"));
                    }));
}

TEST_CASE("an invalid dynamic size keeps the add address dialog open", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* size    = edit_named(dialog, QStringLiteral("size_edit"));
    auto* type    = dialog.findChild<QComboBox*>(QStringLiteral("type_combo"));
    auto* add     = button_labelled(dialog, QStringLiteral("Add"));
    auto* status  = status_label_of(dialog);
    REQUIRE(address != nullptr);
    REQUIRE(size != nullptr);
    REQUIRE(type != nullptr);
    REQUIRE(add != nullptr);
    REQUIRE(status != nullptr);

    dialog.show();
    QApplication::processEvents();

    address->setText(QStringLiteral("0x1000"));
    type->setCurrentIndex(static_cast<int>(ValueType::string));
    size->setText(QStringLiteral("lots"));
    add->click();

    CHECK(dialog.isVisible());
    CHECK(table.size() == 0);
    CHECK(status->text().contains(QStringLiteral("Size must be")));
}

TEST_CASE("the add address dialog edits an existing entry in place", "[ui]")
{
    ensure_application();

    AddressTable                 table;
    slopkit::table::AddressEntry entry;
    entry.description = "Health";
    entry.address     = 0x100010;
    entry.expression  = "app+0x10";
    entry.type        = ValueType::int32;
    entry.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    entry.active      = true;
    table.add(entry);
    const std::uint64_t id = table.entries().front().id;

    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};
    dialog.set_modules({module_image("app", 0x100000, 0x1000)});

    int edits = 0;
    QObject::connect(&dialog,
                     &AddAddressDialog::entryEdited,
                     [&](std::size_t)
                     {
                         ++edits;
                     });

    auto* description = edit_named(dialog, QStringLiteral("description_edit"));
    auto* address     = edit_named(dialog, QStringLiteral("address_edit"));
    auto* size        = edit_named(dialog, QStringLiteral("size_edit"));
    auto* type        = dialog.findChild<QComboBox*>(QStringLiteral("type_combo"));
    auto* hex         = dialog.findChild<QCheckBox*>(QStringLiteral("hex_check"));
    REQUIRE(description != nullptr);
    REQUIRE(address != nullptr);
    REQUIRE(size != nullptr);
    REQUIRE(type != nullptr);
    REQUIRE(hex != nullptr);

    dialog.edit_entry(0);
    dialog.show();
    QApplication::processEvents();

    CHECK(dialog.windowTitle() == QStringLiteral("Edit Address"));
    auto* save = button_labelled(dialog, QStringLiteral("Save"));
    REQUIRE(save != nullptr);
    CHECK(description->text() == QStringLiteral("Health"));
    CHECK(address->text() == QStringLiteral("app+0x10"));
    CHECK(type->currentIndex() == static_cast<int>(ValueType::int32));
    CHECK(size->text() == QStringLiteral("4"));
    CHECK_FALSE(hex->isChecked());

    description->setText(QStringLiteral("Mana"));
    address->setText(QStringLiteral("0x5000"));
    type->setCurrentIndex(static_cast<int>(ValueType::string));
    size->setText(QStringLiteral("8"));
    hex->setChecked(true);
    save->click();

    CHECK_FALSE(dialog.isVisible());
    CHECK(edits == 1);
    REQUIRE(table.size() == 1);
    const auto& updated = table.entries().front();
    CHECK(updated.id == id);
    CHECK(updated.description == "Mana");
    CHECK(updated.address == 0x5000);
    CHECK(updated.expression == "0x5000");
    CHECK(updated.type == ValueType::string);
    CHECK(updated.hex);
    CHECK(updated.bytes.size() == 8);
    CHECK(updated.active);
}

TEST_CASE("a cancelled edit leaves the entry unchanged", "[ui]")
{
    ensure_application();

    AddressTable                 table;
    slopkit::table::AddressEntry entry;
    entry.description = "Health";
    entry.address     = 0x100010;
    entry.expression  = "app+0x10";
    entry.type        = ValueType::int32;
    entry.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(entry);

    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    int edits = 0;
    QObject::connect(&dialog,
                     &AddAddressDialog::entryEdited,
                     [&](std::size_t)
                     {
                         ++edits;
                     });

    auto* description = edit_named(dialog, QStringLiteral("description_edit"));
    auto* address     = edit_named(dialog, QStringLiteral("address_edit"));
    REQUIRE(description != nullptr);
    REQUIRE(address != nullptr);

    dialog.edit_entry(0);
    dialog.show();
    QApplication::processEvents();

    description->setText(QStringLiteral("Changed"));
    address->setText(QStringLiteral("0x9000"));
    dialog.close();

    CHECK(edits == 0);
    REQUIRE(table.size() == 1);
    const auto& kept = table.entries().front();
    CHECK(kept.description == "Health");
    CHECK(kept.address == 0x100010);
    CHECK(kept.expression == "app+0x10");
    CHECK(kept.bytes.size() == 4);
}

TEST_CASE("a malformed address keeps the edit dialog open", "[ui]")
{
    ensure_application();

    AddressTable                 table;
    slopkit::table::AddressEntry entry;
    entry.description = "Health";
    entry.address     = 0x100010;
    entry.expression  = "app+0x10";
    entry.type        = ValueType::int32;
    entry.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(entry);

    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* status  = status_label_of(dialog);
    REQUIRE(address != nullptr);
    REQUIRE(status != nullptr);

    dialog.edit_entry(0);
    dialog.show();
    QApplication::processEvents();

    auto* save = button_labelled(dialog, QStringLiteral("Save"));
    REQUIRE(save != nullptr);

    address->setText(QStringLiteral("app+"));
    save->click();

    CHECK(dialog.isVisible());
    CHECK(status->text().contains(QStringLiteral("Address:")));
    REQUIRE(table.size() == 1);
    CHECK(table.entries().front().address == 0x100010);
    CHECK(table.entries().front().expression == "app+0x10");
}

TEST_CASE("starting a new address after an edit resets to add mode", "[ui]")
{
    ensure_application();

    AddressTable                 table;
    slopkit::table::AddressEntry entry;
    entry.description = "Health";
    entry.address     = 0x100010;
    entry.expression  = "app+0x10";
    entry.type        = ValueType::int32;
    entry.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(entry);

    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    auto* description = edit_named(dialog, QStringLiteral("description_edit"));
    auto* address     = edit_named(dialog, QStringLiteral("address_edit"));
    REQUIRE(description != nullptr);
    REQUIRE(address != nullptr);

    dialog.edit_entry(0);
    dialog.show();
    QApplication::processEvents();
    CHECK(dialog.windowTitle() == QStringLiteral("Edit Address"));
    CHECK(button_labelled(dialog, QStringLiteral("Save")) != nullptr);

    dialog.begin_add();
    dialog.show();
    QApplication::processEvents();

    CHECK(dialog.windowTitle() == QStringLiteral("Add Address"));
    CHECK(button_labelled(dialog, QStringLiteral("Add")) != nullptr);
    CHECK(description->text() == QStringLiteral("New address"));
    CHECK(address->text().isEmpty());
}

TEST_CASE("Return submits the add address dialog", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    auto* description = edit_named(dialog, QStringLiteral("description_edit"));
    auto* address     = edit_named(dialog, QStringLiteral("address_edit"));
    auto* add         = button_labelled(dialog, QStringLiteral("Add"));
    REQUIRE(description != nullptr);
    REQUIRE(address != nullptr);
    REQUIRE(add != nullptr);

    dialog.show();
    QApplication::processEvents();
    REQUIRE(dialog.isVisible());
    CHECK(add->isDefault());

    description->setText(QStringLiteral("Health"));
    address->setText(QStringLiteral("0x4000"));

    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(address, &enter);
    QApplication::processEvents();

    CHECK_FALSE(dialog.isVisible());
    REQUIRE(table.size() == 1);
    CHECK(table.entries().front().description == "Health");
    CHECK(table.entries().front().address == 0x4000);
}

TEST_CASE("Escape closes the add address dialog without adding a row", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* close   = button_labelled(dialog, QStringLiteral("Close"));
    REQUIRE(address != nullptr);
    REQUIRE(close != nullptr);

    dialog.show();
    QApplication::processEvents();
    REQUIRE(dialog.isVisible());
    CHECK_FALSE(close->isDefault());

    address->setText(QStringLiteral("0x4000"));
    QKeyEvent escape {QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};
    QCoreApplication::sendEvent(address, &escape);
    QApplication::processEvents();

    CHECK_FALSE(dialog.isVisible());
    CHECK(table.size() == 0);
}