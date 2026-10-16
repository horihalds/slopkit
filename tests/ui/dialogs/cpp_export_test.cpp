#include <catch2/catch.hpp>

#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>

#include "support/ui_helpers.hpp"
#include "ui/dialogs/cpp_export.hpp"

TEST_CASE("the export dialog previews the header it was handed", "[ui]")
{
    application();

    using slopkit::ui::dialogs::CppExportDialog;

    const QString text = QStringLiteral("#ifndef SLOPKIT_RECLASS_HEADER\n#define SLOPKIT_RECLASS_HEADER\n");

    CppExportDialog dialog {text};
    dialog.show();

    auto* view = dialog.findChild<QPlainTextEdit*>(QStringLiteral("cpp_export_view"));
    REQUIRE(view != nullptr);
    CHECK(view->toPlainText() == text);
    CHECK(view->isReadOnly());

    auto* status = dialog.findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("cpp_export_status"));
    REQUIRE(status != nullptr);

    CHECK(dialog.findChild<QPushButton*>(QStringLiteral("cpp_export_copy")) != nullptr);
    CHECK(dialog.findChild<QPushButton*>(QStringLiteral("cpp_export_save")) != nullptr);
    CHECK(dialog.findChild<QPushButton*>(QStringLiteral("cpp_export_close")) != nullptr);
}

TEST_CASE("Copy puts the header on the clipboard", "[ui]")
{
    application();

    using slopkit::ui::dialogs::CppExportDialog;

    const QString text = QStringLiteral("struct Holder { int32_t field_0; };\n");

    CppExportDialog dialog {text};
    dialog.show();

    auto* copy = dialog.findChild<QPushButton*>(QStringLiteral("cpp_export_copy"));
    REQUIRE(copy != nullptr);
    copy->click();

    CHECK(QGuiApplication::clipboard()->text() == text);
}

TEST_CASE("saving the header writes the previewed bytes", "[ui]")
{
    application();

    using slopkit::ui::dialogs::CppExportDialog;

    const QString text = QStringLiteral("#ifndef SLOPKIT_RECLASS_HEADER\n#endif  // SLOPKIT_RECLASS_HEADER\n");
    const QString path = scratch_settings_file("cpp_export_saved.h");

    CppExportDialog dialog {text};
    QString         saved_path;
    QObject::connect(&dialog,
                     &CppExportDialog::headerSaved,
                     [&saved_path](const QString& saved)
                     {
                         saved_path = saved;
                     });

    dialog.save_to(path);

    CHECK(saved_path == path);
    auto* status = dialog.findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("cpp_export_status"));
    REQUIRE(status != nullptr);
    CHECK(status->text().contains(QStringLiteral("Saved")));

    QFile written {path};
    REQUIRE(written.open(QIODevice::ReadOnly | QIODevice::Text));
    CHECK(QString::fromUtf8(written.readAll()) == text);
}

TEST_CASE("a path that cannot be written reports an error and writes nothing", "[ui]")
{
    application();

    using slopkit::ui::dialogs::CppExportDialog;

    const QString path    = scratch_settings_file("cpp_export_missing_dir_header.h");
    const QString blocked = QFileInfo(path).absolutePath() + QStringLiteral("/no_such_directory/header.h");

    CppExportDialog dialog {QStringLiteral("struct Holder {};\n")};
    bool            reported = false;
    QObject::connect(&dialog,
                     &CppExportDialog::headerSaved,
                     [&reported](const QString&)
                     {
                         reported = true;
                     });

    dialog.save_to(blocked);

    CHECK_FALSE(reported);
    CHECK_FALSE(QFile::exists(blocked));
    auto* status = dialog.findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("cpp_export_status"));
    REQUIRE(status != nullptr);
    CHECK(status->text().contains(QStringLiteral("Could not write")));
}

TEST_CASE("the export dialog remembers the directory it was given", "[ui]")
{
    application();

    using slopkit::ui::dialogs::CppExportDialog;

    const QString   directory = QFileInfo(scratch_settings_file("cpp_export_dir.h")).absolutePath();
    CppExportDialog dialog {QStringLiteral("struct Holder {};\n")};

    dialog.set_dialog_directory(directory);

    CHECK(dialog.dialog_directory() == directory);
}
