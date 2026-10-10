#include <catch2/catch.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextDocument>

#include "script/hook_script.hpp"
#include "support/ui_helpers.hpp"
#include "table/address_table.hpp"
#include "ui/components/script_editor.hpp"
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

    QPushButton* verify_of(AddScriptDialog& dialog)
    {
        return dialog.findChild<QPushButton*>(QStringLiteral("verify_button"));
    }

    slopkit::ui::components::ScriptEditor* script_editor_of(AddScriptDialog& dialog)
    {
        return dialog.findChild<slopkit::ui::components::ScriptEditor*>(QStringLiteral("script_edit"));
    }

    slopkit::ui::widgets::StatusLabel* status_of(AddScriptDialog& dialog)
    {
        return dialog.findChild<slopkit::ui::widgets::StatusLabel*>();
    }

    QPushButton* close_of(AddScriptDialog& dialog)
    {
        for (QPushButton* button : dialog.findChildren<QPushButton*>())
        {
            if (button->text() == QStringLiteral("Close"))
            {
                return button;
            }
        }
        return nullptr;
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

    // The add mode suggests a description and starts from the hook skeleton.
    CHECK(description_of(dialog)->text() == QStringLiteral("New script"));
    CHECK(editor_of(dialog)->toPlainText().contains(QStringLiteral("function activate()")));
    CHECK(editor_of(dialog)->toPlainText().contains(QStringLiteral("function deactivate()")));
    CHECK(editor_of(dialog)->toPlainText().contains(QStringLiteral("-- function update()")));

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
    CHECK(editor_of(dialog)->toPlainText().contains(QStringLiteral("function activate()")));

    description_of(dialog)->setText(QStringLiteral("helper"));
    editor_of(dialog)->setPlainText(QStringLiteral("return 1"));
    commit_of(dialog)->click();

    REQUIRE(table.size() == 2);
    CHECK(table.entries()[1].kind == EntryKind::script);
    CHECK(table.entries()[1].script == "return 1");
}

TEST_CASE("committing the untouched add form stores the hook skeleton", "[ui]")
{
    application();

    AddressTable    table;
    AddScriptDialog dialog {table};
    dialog.show();

    // The skeleton names all three hooks and shows the refusal form; the
    // optional `update` hook is seeded commented out.
    const QString skeleton = editor_of(dialog)->toPlainText();
    CHECK(skeleton.contains(QStringLiteral("function activate()")));
    CHECK(skeleton.contains(QStringLiteral("function deactivate()")));
    CHECK(skeleton.contains(QStringLiteral("-- function update()")));
    CHECK(skeleton.contains(QStringLiteral("return false, \"why\"")));

    commit_of(dialog)->click();

    REQUIRE(table.size() == 1);
    CHECK(table.entries()[0].kind == EntryKind::script);
    CHECK(table.entries()[0].script == skeleton.toStdString());
}

TEST_CASE("the verify button checks the source without changing the entry", "[ui]")
{
    application();

    AddressTable    table;
    AddScriptDialog dialog {table};
    dialog.show();

    REQUIRE(verify_of(dialog) != nullptr);
    REQUIRE(script_editor_of(dialog) != nullptr);
    REQUIRE(status_of(dialog) != nullptr);

    // The untouched skeleton compiles; nothing is appended and nothing closes.
    verify_of(dialog)->click();
    CHECK(table.empty());
    CHECK(dialog.isVisible());
    CHECK(script_editor_of(dialog)->error_line() == 0);
    CHECK(status_of(dialog)->text() == QStringLiteral("Syntax OK."));

    // Broken source reports the compiler's message and marks the offending line.
    script_editor_of(dialog)->setPlainText(QStringLiteral("local x = 1\nlocal y = )\n"));
    verify_of(dialog)->click();
    CHECK(table.empty());
    CHECK(dialog.isVisible());
    CHECK(script_editor_of(dialog)->error_line() == 2);
    CHECK(status_of(dialog)->text().contains(QStringLiteral("script:2:")));

    // Editing the source clears the verdict and the mark.
    script_editor_of(dialog)->setPlainText(QStringLiteral("return 1"));
    CHECK(script_editor_of(dialog)->error_line() == 0);
    CHECK(status_of(dialog)->text().isEmpty());
}

TEST_CASE("verifying in edit mode leaves the entry unchanged", "[ui]")
{
    application();

    AddressTable      table;
    const std::size_t row = table.add_script("helper", "return 1");

    AddScriptDialog dialog {table};
    dialog.edit_entry(row);
    dialog.show();

    // A valid edit verifies but is not written.
    script_editor_of(dialog)->setPlainText(QStringLiteral("return 2"));
    verify_of(dialog)->click();
    CHECK(status_of(dialog)->text() == QStringLiteral("Syntax OK."));
    CHECK(table.entries()[row].script == "return 1");
    CHECK(table.entries()[row].description == "helper");
    CHECK(dialog.isVisible());

    // A rejected verdict writes nothing either.
    script_editor_of(dialog)->setPlainText(QStringLiteral("return ("));
    verify_of(dialog)->click();
    CHECK(table.entries()[row].script == "return 1");
    CHECK(table.entries()[row].description == "helper");
    CHECK(dialog.isVisible());
    CHECK(script_editor_of(dialog)->error_line() > 0);
}

TEST_CASE("a prefilled add dialog keeps the generated text editable", "[ui]")
{
    application();

    AddressTable    table;
    AddScriptDialog dialog {table};

    const QString description = QStringLiteral("Hook game.exe+1A2B40");
    const QString source      = QStringLiteral("function activate()\n    return true\nend\n");
    dialog.reset_for_add(description, source);

    CHECK(description_of(dialog)->text() == description);
    CHECK(editor_of(dialog)->toPlainText() == source);
    CHECK(dialog.windowTitle() == QStringLiteral("Add Script"));
    CHECK(commit_of(dialog)->text() == QStringLiteral("Add"));

    // Nothing reaches the table before Add.
    CHECK(table.empty());
    dialog.show();
    commit_of(dialog)->click();

    REQUIRE(table.size() == 1);
    CHECK(table.entries()[0].kind == EntryKind::script);
    CHECK(table.entries()[0].description == description.toStdString());
    CHECK(table.entries()[0].script == source.toStdString());
}

TEST_CASE("Escape leaves the add-script dialog open", "[ui]")
{
    application();

    AddressTable    table;
    AddScriptDialog dialog {table};
    dialog.show();

    bool rejected = false;
    QObject::connect(&dialog,
                     &QDialog::rejected,
                     &dialog,
                     [&rejected]
                     {
                         rejected = true;
                     });

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&dialog, &escape);

    CHECK(dialog.isVisible());
    CHECK_FALSE(rejected);

    // The Close button is still the way out.
    REQUIRE(close_of(dialog) != nullptr);
    close_of(dialog)->click();
    CHECK_FALSE(dialog.isVisible());
    CHECK(rejected);
}

TEST_CASE("the add-script dialog opens wide enough for a hook script", "[ui]")
{
    application();

    AddressTable    table;
    AddScriptDialog dialog {table};

    // A hook script as the generator renders it; 32 original bytes make the
    // `original = string.char(...)` line the document's longest at ~106 columns.
    slopkit::script::HookTarget target;
    target.address            = 0x7F3A1B2C;
    target.description        = "game.exe+1A2B40";
    target.module_name        = "game.exe";
    target.module_rva_text    = "1A2B40";
    target.pattern            = "48 8B 05 ?? ?? ?? ?? 48 89 03";
    target.instruction_length = 3;
    for (int index = 0; index < 32; ++index)
    {
        target.original.push_back(static_cast<std::byte>(index));
    }
    slopkit::script::HookInstruction covered;
    covered.text         = "MOV RAX, [RBX+1C]";
    covered.rewritten    = {"MOV RAX, qword ptr [RBX+0x%X]"};
    covered.address_args = {0x1C};
    target.covered.push_back(covered);
    target.trampoline_lines = {"MOV RAX, qword ptr [RBX+0x%X]"};

    const QString source = QString::fromStdString(slopkit::script::render(target));
    dialog.reset_for_add(QStringLiteral("Hook game.exe+1A2B40"), source);
    dialog.show();

    auto* editor = script_editor_of(dialog);
    REQUIRE(editor != nullptr);

    const int column = static_cast<int>(std::ceil(QFontMetricsF(editor->font()).horizontalAdvance(QLatin1Char(' '))));
    const int ideal  = static_cast<int>(std::ceil(editor->document()->idealWidth()));

    // The default width shows the hook script's longest line without scrolling
    // and is wider than the 100-column floor a short script gets.
    CHECK(editor->sizeHint().width() >= ideal + editor->gutter_width());
    CHECK(editor->sizeHint().width() > 100 * column);
    CHECK(dialog.sizeHint().width() >= editor->sizeHint().width());
}
