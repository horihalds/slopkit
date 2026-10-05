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
#include "table/address_table.hpp"
#include "ui/components/widgets.hpp"
#include "ui/dialogs/add_address.hpp"

namespace
{
    // A QApplication may only exist once per process; Catch2 normally runs each
    // case in its own process, but the binary accepts several.
    void ensure_application()
    {
        if (QCoreApplication::instance() != nullptr)
        {
            return;
        }
        static int          argc      = 1;
        static char         program[] = "slopkit_tests";
        static char*        argv[]    = {program, nullptr};
        static QApplication instance(argc, argv);
    }

    using slopkit::scan::ValueType;
    using slopkit::table::AddressTable;
    using slopkit::ui::dialogs::AddAddressDialog;

    // Address-keyed bytes the fake target serves.
    using FakeMemory = std::map<std::uint64_t, std::vector<std::byte>>;

    class FakeBackend final : public slopkit::process::SessionBackend
    {
    public:
        std::shared_ptr<FakeMemory> memory = std::make_shared<FakeMemory>();

        [[nodiscard]] slopkit::process::ProcessId pid() const noexcept override
        {
            return 42;
        }

        [[nodiscard]] std::string_view plugin_id() const noexcept override
        {
            return "fake";
        }

        [[nodiscard]] slopkit::process::AccessMethod advertised_methods() const noexcept override
        {
            return slopkit::process::AccessMethod::procfs_mem;
        }

        [[nodiscard]] slopkit::process::AccessMethod last_method() const noexcept override
        {
            return slopkit::process::AccessMethod::procfs_mem;
        }

        std::expected<std::vector<std::byte>, slopkit::process::AccessError> read(std::uint64_t address,
                                                                                  std::size_t   size) override
        {
            std::vector<std::byte> bytes(size, std::byte {0});
            if (const auto entry = memory->find(address); entry != memory->end())
            {
                bytes = entry->second;
                bytes.resize(size);
            }
            return bytes;
        }

        std::expected<std::size_t, slopkit::process::AccessError> write(std::uint64_t,
                                                                        std::span<const std::byte>) override
        {
            return std::size_t {};
        }

        std::expected<std::vector<slopkit::process::ModuleInfo>, slopkit::process::AccessError> modules() override
        {
            return std::vector<slopkit::process::ModuleInfo> {};
        }

        std::expected<std::vector<slopkit::process::ThreadInfo>, slopkit::process::AccessError> threads() override
        {
            return std::vector<slopkit::process::ThreadInfo> {};
        }

        std::expected<std::vector<slopkit::process::RegionInfo>, slopkit::process::AccessError> regions() override
        {
            return std::vector<slopkit::process::RegionInfo> {};
        }
    };

    class FakeAccess final : public slopkit::process::ProcessAccess
    {
    public:
        std::shared_ptr<FakeMemory> memory = std::make_shared<FakeMemory>();

        std::expected<std::vector<slopkit::process::ProcessInfo>, slopkit::process::AccessError>
        list_processes() override
        {
            return std::vector<slopkit::process::ProcessInfo> {};
        }

        std::expected<slopkit::process::Session, slopkit::process::AccessError> attach(slopkit::process::ProcessId,
                                                                                       std::string_view) override
        {
            auto backend    = std::make_unique<FakeBackend>();
            backend->memory = memory;
            return slopkit::process::Session {std::move(backend)};
        }
    };

    bool pump(slopkit::process::AccessWorker& worker, const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            worker.drain();
            QApplication::processEvents();
            if (done())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    }

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
        REQUIRE(pump(worker,
                     [&]
                     {
                         return attached;
                     }));
    }

    slopkit::process::ModuleInfo module_image(std::string name, std::uint64_t base, std::uint64_t size)
    {
        slopkit::process::ModuleInfo module;
        module.kind = slopkit::process::ModuleKind::elf;
        module.name = std::move(name);
        module.base = base;
        module.size = size;
        return module;
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

    REQUIRE(pump(worker,
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
