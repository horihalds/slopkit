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
#include <QTextCursor>
#include <QTextDocument>

#include "script/hook_script.hpp"
#include "support/reclass_helpers.hpp"
#include "support/ui_helpers.hpp"
#include "table/address_table.hpp"
#include "ui/components/script_completion.hpp"
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

    // Opens the completion list at the end of the editor's text, as
    // `Ctrl+Space` does.
    void open_completion(slopkit::ui::components::ScriptEditor& editor)
    {
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);

        QKeyEvent event(QEvent::KeyPress, Qt::Key_Space, Qt::ControlModifier);
        QCoreApplication::sendEvent(&editor, &event);
    }
}

TEST_CASE("the add-script dialog appends a script entry", "[ui]")
{
    application();

    AddressTable    table;
    AddScriptDialog dialog {table};
    dialog.show();

    REQUIRE(description_of(dialog) != nullptr);
    REQUIRE(editor_of(dialog) != nullptr);
    REQUIRE(commit_of(dialog) != nullptr);

    CHECK(description_of(dialog)->text() == QStringLiteral("New script"));
    CHECK(editor_of(dialog)->toPlainText().contains(QStringLiteral("function activate()")));
    CHECK(editor_of(dialog)->toPlainText().contains(QStringLiteral("function deactivate()")));
    CHECK(editor_of(dialog)->toPlainText().contains(QStringLiteral("-- function update()")));

    description_of(dialog)->clear();
    commit_of(dialog)->click();
    CHECK(table.empty());
    CHECK(dialog.isVisible());

    const std::string source = "local\ttab = {1, 2}\nprint(\"quoted\", 'single')\nreturn tab";
    description_of(dialog)->setText(QStringLiteral("helper"));
    editor_of(dialog)->setPlainText(QString::fromStdString(source));
    commit_of(dialog)->click();

    REQUIRE(table.size() == 1);
    CHECK(table.entries()[0].kind == EntryKind::script);
    CHECK(table.entries()[0].description == "helper");
    CHECK(table.entries()[0].script == source);
    CHECK_FALSE(dialog.isVisible());
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

    dialog.reject();
    CHECK(table.size() == 1);
    CHECK(table.entries()[0].script == "return 1");

    description_of(dialog)->setText(QStringLiteral("renamed"));
    editor_of(dialog)->setPlainText(QStringLiteral("return 2\n"));
    commit_of(dialog)->click();

    REQUIRE(table.size() == 1);
    CHECK(table.entries()[0].description == "renamed");
    CHECK(table.entries()[0].script == "return 2\n");

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

    verify_of(dialog)->click();
    CHECK(table.empty());
    CHECK(dialog.isVisible());
    CHECK(script_editor_of(dialog)->error_line() == 0);
    CHECK(status_of(dialog)->text() == QStringLiteral("Syntax OK."));

    script_editor_of(dialog)->setPlainText(QStringLiteral("local x = 1\nlocal y = )\n"));
    verify_of(dialog)->click();
    CHECK(table.empty());
    CHECK(dialog.isVisible());
    CHECK(script_editor_of(dialog)->error_line() == 2);
    CHECK(status_of(dialog)->text().contains(QStringLiteral("script:2:")));

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

    script_editor_of(dialog)->setPlainText(QStringLiteral("return 2"));
    verify_of(dialog)->click();
    CHECK(status_of(dialog)->text() == QStringLiteral("Syntax OK."));
    CHECK(table.entries()[row].script == "return 1");
    CHECK(table.entries()[row].description == "helper");
    CHECK(dialog.isVisible());

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

    CHECK(editor->sizeHint().width() >= ideal + editor->gutter_width());
    CHECK(editor->sizeHint().width() > 100 * column);
    CHECK(dialog.sizeHint().width() >= editor->sizeHint().width());
}

TEST_CASE("the add-script dialog offers the ReClass fields of a bound instance", "[ui]")
{
    application();

    AddressTable               table;
    slopkit::reclass::Project& project = table.structures();
    project.add_class("Player", "game.exe+1A2B40");
    project.classes().back().nodes.push_back(
        slopkit::test::node_of(project, slopkit::reclass::NodeKind::int32, "ammo"));
    project.classes().back().nodes.push_back(
        slopkit::test::node_of(project, slopkit::reclass::NodeKind::float32, "health"));

    AddScriptDialog dialog {table};
    dialog.show();

    auto* editor = script_editor_of(dialog);
    REQUIRE(editor != nullptr);
    auto* completion = editor->findChild<slopkit::ui::components::ScriptCompletionPopup*>();
    REQUIRE(completion != nullptr);

    editor->setPlainText(QStringLiteral("local p = structs.get(\"Player\"):at(0x1000)\np."));
    open_completion(*editor);
    REQUIRE(completion->is_open());
    REQUIRE(completion->selected_name().has_value());
    CHECK(*completion->selected_name() == QStringLiteral("ammo"));

    // The field's type travels with it, as the popup's detail line.
    auto* model = completion->findChild<slopkit::ui::components::ScriptCompletionModel*>();
    REQUIRE(model != nullptr);
    CHECK(model->data(model->index(0), slopkit::ui::components::ScriptCompletionModel::SignatureRole).toString()
          == QStringLiteral("i32"));

    // The fields come from the live project, so a rename in the ReClass view
    // shows up on the next ask.
    project.classes().back().nodes[0]->name = "rounds";
    open_completion(*editor);
    REQUIRE(completion->is_open());
    REQUIRE(completion->selected_name().has_value());
    CHECK(*completion->selected_name() == QStringLiteral("health"));
}
