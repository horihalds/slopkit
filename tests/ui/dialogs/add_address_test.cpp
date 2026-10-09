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
} // namespace

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

    // A freshly opened dialog shows the defaults.
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

    // The entry is not added until the resolve completes.
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

TEST_CASE("an invalid address keeps the add address dialog open", "[ui]")
{
    ensure_application();

    AddressTable                   table;
    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    AddAddressDialog               dialog {table, worker};

    auto* address = edit_named(dialog, QStringLiteral("address_edit"));
    auto* add     = button_labelled(dialog, QStringLiteral("Add"));
    auto* status  = dialog.findChild<slopkit::ui::widgets::StatusLabel*>();
    REQUIRE(address != nullptr);
    REQUIRE(add != nullptr);
    REQUIRE(status != nullptr);

    dialog.show();
    QApplication::processEvents();

    address->setText(QStringLiteral("not-an-address"));
    add->click();

    CHECK(dialog.isVisible());
    CHECK(table.size() == 0);
    CHECK(status->text().contains(QStringLiteral("Address:")));
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
    auto* status  = dialog.findChild<slopkit::ui::widgets::StatusLabel*>();
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
