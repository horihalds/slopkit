#include <catch2/catch.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

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

    AddressTable     table;
    AddAddressDialog dialog {table};

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

    AddressTable     table;
    AddAddressDialog dialog {table};

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
    CHECK(entry.type == ValueType::int32);
}

TEST_CASE("an invalid address keeps the add address dialog open", "[ui]")
{
    ensure_application();

    AddressTable     table;
    AddAddressDialog dialog {table};

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

    AddressTable     table;
    AddAddressDialog dialog {table};

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
