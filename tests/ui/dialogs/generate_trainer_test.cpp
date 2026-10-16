#include <catch2/catch.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <QCheckBox>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>
#include <QString>

#include "support/ui_helpers.hpp"
#include "table/address_table.hpp"
#include "trainer/generate.hpp"
#include "ui/dialogs/generate_trainer.hpp"

namespace
{
    using slopkit::scan::ValueType;
    using slopkit::table::AddressEntry;
    using slopkit::table::AddressTable;
    using slopkit::table::EntryKind;
    using slopkit::trainer::Request;
    using slopkit::ui::dialogs::GenerateTrainerDialog;

    AddressTable practice_table()
    {
        AddressEntry health;
        health.kind        = EntryKind::value;
        health.description = "Health";
        health.expression  = "practice+1f2a";
        health.type        = ValueType::int32;
        health.bytes       = std::vector<std::byte>(4, std::byte {42});

        AddressEntry score;
        score.kind        = EntryKind::value;
        score.description = "Score";
        score.expression  = "practice+1f2e";
        score.type        = ValueType::int32;
        score.bytes       = std::vector<std::byte>(4, std::byte {1});

        AddressTable table;
        table.add(health);
        table.add(score);
        static_cast<void>(table.add_script("No reload", "return true"));
        return table;
    }

    QString scratch_directory(std::string_view name)
    {
        const QString   directory = QFileInfo(scratch_settings_file(name)).absolutePath() + QStringLiteral("/")
                                  + QString::fromStdString(std::string {name});
        std::error_code error;
        std::filesystem::remove_all(directory.toStdString(), error);
        std::filesystem::create_directories(directory.toStdString(), error);
        REQUIRE_FALSE(error);
        return directory;
    }

    Request request_for(const AddressTable& table)
    {
        Request request;
        request.table            = &table;
        request.process_name     = "practice";
        request.pointer_size     = 8;
        request.linux_target     = true;
        request.windows_target   = false;
        request.output_directory = scratch_directory("generate_trainer_dialog").toStdString();
        request.base_name        = "practice";
        return request;
    }
}

TEST_CASE("the dialog summarises what will be embedded", "[ui][trainer]")
{
    application();

    const AddressTable    table = practice_table();
    GenerateTrainerDialog dialog {request_for(table)};
    dialog.show();

    CHECK(dialog.summary_text() == QStringLiteral("2 value entries, 1 script entries"));
    CHECK(dialog.linux_target());
    CHECK_FALSE(dialog.windows_target());
    CHECK(dialog.can_generate());

    // The summary line carries the recorded pointer size and target name too.
    auto* summary = dialog.findChild<QLabel*>(QStringLiteral("generate_trainer_summary"));
    REQUIRE(summary != nullptr);
    CHECK(summary->text().contains(QStringLiteral("2 value entries, 1 script entries")));
    CHECK(summary->text().contains(QStringLiteral("8-byte")));
    CHECK(summary->text().contains(QStringLiteral("practice")));

    auto* generate = dialog.findChild<QPushButton*>(QStringLiteral("generate_trainer_generate"));
    REQUIRE(generate != nullptr);
    CHECK(generate->isEnabled());
}

TEST_CASE("the Generate button needs a target, a folder and a name", "[ui][trainer]")
{
    application();

    const AddressTable    table = practice_table();
    GenerateTrainerDialog dialog {request_for(table)};
    dialog.show();

    auto* generate = dialog.findChild<QPushButton*>(QStringLiteral("generate_trainer_generate"));
    REQUIRE(generate != nullptr);

    dialog.set_linux_target(false);
    CHECK_FALSE(dialog.can_generate());
    CHECK_FALSE(generate->isEnabled());

    dialog.set_windows_target(true);
    CHECK(dialog.can_generate());
    CHECK(generate->isEnabled());

    dialog.set_output_directory(QString());
    CHECK_FALSE(dialog.can_generate());
    CHECK_FALSE(generate->isEnabled());

    dialog.set_output_directory(scratch_directory("generate_trainer_dialog_folder"));
    auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("generate_trainer_name"));
    REQUIRE(name != nullptr);
    name->setText(QString());
    CHECK_FALSE(dialog.can_generate());
    CHECK_FALSE(generate->isEnabled());
}

TEST_CASE("generating writes the trainer files the dialog names", "[ui][trainer]")
{
    application();

    const AddressTable          table     = practice_table();
    const std::filesystem::path directory = scratch_directory("generate_trainer_dialog_names").toStdString();

    auto request             = request_for(table);
    request.output_directory = directory;

    GenerateTrainerDialog dialog {request};
    dialog.show();

    QString reported;
    QObject::connect(&dialog,
                     &GenerateTrainerDialog::trainersGenerated,
                     [&reported](const QString& chosen)
                     {
                         reported = chosen;
                     });

    dialog.generate();

    const auto produced = directory / "practice.trainer";
    CHECK(reported == QString::fromStdString(directory.string()));
    CHECK(std::filesystem::is_regular_file(produced));
    CHECK(dialog.result_text().contains(QStringLiteral("practice.trainer")));

    auto* status = dialog.findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("generate_trainer_status"));
    REQUIRE(status != nullptr);
    CHECK(status->text().contains(QStringLiteral("Wrote 1 trainer")));
}

TEST_CASE("a missing template is reported per target in the dialog", "[ui][trainer]")
{
    application();

    const AddressTable          table     = practice_table();
    const std::filesystem::path directory = scratch_directory("generate_trainer_dialog_partial").toStdString();

    auto request             = request_for(table);
    request.output_directory = directory;

    GenerateTrainerDialog dialog {request};
    dialog.show();

    dialog.set_windows_target(true);
    dialog.generate();

    const bool windows_template =
        std::filesystem::is_regular_file(std::filesystem::path {SLOPKIT_TRAINER_DIR} / "slopkit-trainer.exe");

    CHECK(std::filesystem::is_regular_file(directory / "practice.trainer"));
    CHECK(std::filesystem::is_regular_file(directory / "practice.trainer.exe") == windows_template);
    if (!windows_template)
    {
        CHECK(dialog.result_text().contains(QStringLiteral("windows")));
        CHECK(dialog.result_text().contains(QStringLiteral("practice.trainer")));
        CHECK_FALSE(dialog.result_text().contains(QStringLiteral("practice.trainer.exe")));
    }
}
