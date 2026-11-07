#include <QComboBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVariant>

#include "reclass/project.hpp"
#include "support/ui_helpers.hpp"
#include "ui/dialogs/enums.hpp"

namespace
{
    using slopkit::reclass::Project;
    using slopkit::ui::dialogs::EnumChooserDialog;
    using slopkit::ui::dialogs::EnumEditorDialog;
}

TEST_CASE("the enum editor creates an enum with its values", "[enums][ui]")
{
    application();

    Project          project;
    EnumEditorDialog editor {project, QString {}};

    editor.name_edit()->setText(QStringLiteral("Colour"));
    editor.add_value_row(QStringLiteral("red"), QStringLiteral("1"), QStringLiteral("warm"));
    editor.add_value_row(QStringLiteral("green"), QStringLiteral("0x10"));

    REQUIRE(editor.apply());
    REQUIRE(project.enums().size() == 1);
    CHECK(project.enums()[0].name == "Colour");
    CHECK(project.enums()[0].size == 4);
    REQUIRE(project.enums()[0].values.size() == 2);
    CHECK(project.enums()[0].values[0].name == "red");
    CHECK(project.enums()[0].values[0].value == 1);
    CHECK(project.enums()[0].values[0].comment == "warm");
    CHECK(project.enums()[0].values[1].value == 16);
    CHECK(editor.applied_name() == QStringLiteral("Colour"));
}

TEST_CASE("the enum editor refuses an empty name and a duplicate", "[enums][ui]")
{
    application();

    Project project;
    project.add_enum("Colour", 4);

    QString          message;
    EnumEditorDialog editor {project, QString {}};
    QObject::connect(&editor,
                     &EnumEditorDialog::statusChanged,
                     [&](const QString& text, bool)
                     {
                         message = text;
                     });

    CHECK_FALSE(editor.apply());
    CHECK(message == QStringLiteral("An enum name is required."));
    CHECK(project.enums().size() == 1);

    editor.name_edit()->setText(QStringLiteral("Colour"));
    CHECK_FALSE(editor.apply());
    CHECK(message == QStringLiteral("An enum named 'Colour' already exists."));
    CHECK(project.enums().size() == 1);
}

TEST_CASE("the enum editor refuses a value wider than the enum", "[enums][ui]")
{
    application();

    Project project;
    project.add_enum("Small", 1);

    EnumEditorDialog editor {project, QStringLiteral("Small")};
    editor.add_value_row(QStringLiteral("too_big"), QStringLiteral("0x100"));

    CHECK_FALSE(editor.apply());
    REQUIRE(project.enums().size() == 1);
    CHECK(project.enums()[0].values.empty());
}

TEST_CASE("the enum editor edits an existing enum in place", "[enums][ui]")
{
    application();

    Project project;
    project.add_enum("Colour", 4);
    project.enums()[0].values.push_back({.name = "red", .value = 1, .comment = {}});

    EnumEditorDialog editor {project, QStringLiteral("Colour")};

    // The editor loads what the project holds.
    CHECK(editor.name_edit()->text() == QStringLiteral("Colour"));
    REQUIRE(editor.values_table()->rowCount() == 1);
    CHECK(editor.values_table()->item(0, 0)->text() == QStringLiteral("red"));

    editor.size_combo()->setCurrentIndex(editor.size_combo()->findData(QVariant::fromValue<qulonglong>(2)));
    editor.values_table()->item(0, 0)->setText(QStringLiteral("crimson"));

    REQUIRE(editor.apply());
    REQUIRE(project.enums().size() == 1);
    CHECK(project.enums()[0].size == 2);
    CHECK(project.enums()[0].values[0].name == "crimson");
    CHECK(editor.applied_name() == QStringLiteral("Colour"));
}

TEST_CASE("the enum chooser lists the project's enums", "[enums][ui]")
{
    application();

    Project project;
    project.add_enum("Colour", 4);
    project.add_enum("Flags", 1);

    EnumChooserDialog chooser {project, QStringLiteral("Flags")};
    REQUIRE(chooser.combo()->count() == 3);
    CHECK(chooser.combo()->itemText(0) == QStringLiteral("(no enum)"));
    CHECK(chooser.chosen_name() == QStringLiteral("Flags"));

    chooser.combo()->setCurrentIndex(0);
    CHECK(chooser.chosen_name().isEmpty());
}
