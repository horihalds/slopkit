#include <catch2/catch.hpp>

#include <cstdint>

#include <QComboBox>
#include <QCoreApplication>
#include <QLineEdit>
#include <QTableView>

#include "support/ui_helpers.hpp"
#include "ui/components/referenced_strings.hpp"
#include "ui/dialogs/referenced_strings.hpp"
#include "ui/models/referenced_string_model.hpp"

namespace
{
    slopkit::ui::components::ReferencedString
    entry(std::uint64_t instruction, std::uint64_t string_address, const char* text, bool wide = false)
    {
        return slopkit::ui::components::ReferencedString {
            .instruction = instruction, .string_address = string_address, .text = text, .wide = wide};
    }
}

TEST_CASE("the referenced-strings window lists every module and filters case-insensitively", "[ui]")
{
    application();

    slopkit::ui::components::ReferencedStringTable table;
    table.set_module("alpha", {entry(0x1000, 0x3010, "Hello"), entry(0x1010, 0x3020, "world", true)});
    table.set_module("beta", {entry(0x2000, 0x4010, "HELLO there")});

    slopkit::ui::dialogs::ReferencedStringsDialog dialog(table);
    dialog.show();
    QCoreApplication::processEvents();

    REQUIRE(dialog.module_combo()->count() == 3);
    CHECK(dialog.module_combo()->itemText(0) == QStringLiteral("All analyzed modules"));
    CHECK(dialog.module_combo()->itemText(1) == QStringLiteral("alpha"));
    CHECK(dialog.module_combo()->itemText(2) == QStringLiteral("beta"));

    CHECK(dialog.row_count() == 3);
    CHECK(dialog.status_text().contains(QStringLiteral("of 3")));

    dialog.set_filter(QStringLiteral("hello"));
    CHECK(dialog.row_count() == 2);

    dialog.set_filter({});
    CHECK(dialog.row_count() == 3);

    dialog.select_module(QStringLiteral("beta"));
    CHECK(dialog.row_count() == 1);
    CHECK(dialog.cell_text(0, slopkit::ui::models::ReferencedStringModel::string) == QStringLiteral("HELLO there"));
    CHECK(dialog.cell_text(0, slopkit::ui::models::ReferencedStringModel::module) == QStringLiteral("beta"));
    CHECK(dialog.status_text().contains(QStringLiteral("of 1")));

    std::uint64_t navigated = 0;
    QObject::connect(&dialog,
                     &slopkit::ui::dialogs::ReferencedStringsDialog::instructionActivated,
                     &dialog,
                     [&navigated](std::uint64_t address)
                     {
                         navigated = address;
                     });
    dialog.table()->doubleClicked(dialog.table()->model()->index(0, 0));
    CHECK(navigated == 0x2000);

    dialog.hide();
}

TEST_CASE("the referenced-strings window shows an empty state before any analysis", "[ui]")
{
    application();

    slopkit::ui::components::ReferencedStringTable table;
    slopkit::ui::dialogs::ReferencedStringsDialog  dialog(table);
    dialog.show();
    QCoreApplication::processEvents();

    CHECK(dialog.row_count() == 0);
    CHECK(dialog.empty_state()->isVisible());
    CHECK_FALSE(dialog.table()->isVisible());

    table.set_module("alpha", {entry(0x1000, 0x3010, "hello")});
    dialog.refresh();
    CHECK(dialog.row_count() == 1);
    CHECK_FALSE(dialog.empty_state()->isVisible());
    CHECK(dialog.table()->isVisible());

    dialog.hide();
}
