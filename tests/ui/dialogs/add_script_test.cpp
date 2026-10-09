#include <catch2/catch.hpp>

#include <cstddef>
#include <string>

#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>

#include "support/ui_helpers.hpp"
#include "table/address_table.hpp"
#include "ui/dialogs/add_script.hpp"

namespace
{
    using slopkit::table::AddressEntry;
    using slopkit::table::AddressTable;
    using slopkit::table::EntryKind;
    using slopkit::ui::dialogs::AddScriptDialog;

    QLineEdit* description_of(AddScriptDialog& dialog)
    {
        return dialog.findChild<QLineEdit*>(QStringLiteral("description_edit"));
    }

    QPlainTextEdit* editor_of(AddScriptDialog& dialog)
    {
        return dialog.findChild<QPlainTextEdit*>(QStringLiteral("script_edit"));
    }

    QPushButton* commit_of(AddScriptDialog& dialog)
    {
        return dialog.findChild<QPushButton*>(QStringLiteral("commit_button"));
    }
} // namespace

TEST_CASE("the add-script dialog appends a script entry", "[ui]")
{
    application();

    AddressTable    table;
    AddScriptDialog dialog {table};
    dialog.show();

    REQUIRE(description_of(dialog) != nullptr);
    REQUIRE(editor_of(dialog) != nullptr);
    REQUIRE(commit_of(dialog) != nullptr);

    // The add mode suggests a description and starts from an empty script.
    CHECK(description_of(dialog)->text() == QStringLiteral("New script"));
    CHECK(editor_of(dialog)->toPlainText().isEmpty());

    // An empty description is refused and appends nothing.
    description_of(dialog)->clear();
    commit_of(dialog)->click();
    CHECK(table.empty());
    CHECK(dialog.isVisible());

    // Committing stores the text verbatim: newlines, tabs and quotes included.
    const std::string source = "local\ttab = {1, 2}\nprint(\"quoted\", 'single')\nreturn tab";
    description_of(dialog)->setText(QStringLiteral("helper"));
    editor_of(dialog)->setPlainText(QString::fromStdString(source));
    commit_of(dialog)->click();

    REQUIRE(table.size() == 1);
    CHECK(table.entries()[0].kind == EntryKind::script);
    CHECK(table.entries()[0].description == "helper");
    CHECK(table.entries()[0].script == source);
    CHECK_FALSE(dialog.isVisible()); // a confirmed dialog closes
}

TEST_CASE("the edit-script dialog replaces the description and the source", "[ui]")
{
    application();

    AddressTable      table;
    const std::size_t row = table.add_script("helper", "return 1");

    AddScriptDialog dialog {table};
    dialog.edit_entry(row);

    CHECK(description_of(dialog)->text() == QStringLiteral("helper"));
    CHECK(editor_of(dialog)->toPlainText() == QStringLiteral("return 1"));

    // Closing the dialog leaves the entry untouched.
    dialog.reject();
    CHECK(table.size() == 1);
    CHECK(table.entries()[0].script == "return 1");

    // Committing replaces both fields and does not append a second entry.
    description_of(dialog)->setText(QStringLiteral("renamed"));
    editor_of(dialog)->setPlainText(QStringLiteral("return 2\n"));
    commit_of(dialog)->click();

    REQUIRE(table.size() == 1);
    CHECK(table.entries()[0].description == "renamed");
    CHECK(table.entries()[0].script == "return 2\n");

    // An empty description is refused in edit mode too.
    dialog.edit_entry(0);
    description_of(dialog)->setText(QStringLiteral(" "));
    commit_of(dialog)->click();
    CHECK(table.entries()[0].description == "renamed");
}

TEST_CASE("the edit-script dialog falls back to add mode for a value row", "[ui]")
{
    application();

    AddressTable table;
    AddressEntry value;
    value.description = "health";
    table.add(value);

    AddScriptDialog dialog {table};
    dialog.edit_entry(0);

    CHECK(description_of(dialog)->text() == QStringLiteral("New script"));
    CHECK(editor_of(dialog)->toPlainText().isEmpty());

    description_of(dialog)->setText(QStringLiteral("helper"));
    editor_of(dialog)->setPlainText(QStringLiteral("return 1"));
    commit_of(dialog)->click();

    REQUIRE(table.size() == 2);
    CHECK(table.entries()[1].kind == EntryKind::script);
    CHECK(table.entries()[1].script == "return 1");
}
