#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the table settings dialog edits the table settings in place", "[ui]")
{
    application();

    slopkit::table::AddressTable              table;
    slopkit::process::AttachedTarget          target;
    slopkit::ui::dialogs::TableSettingsDialog dialog {table, target};

    auto* target_panel = dialog.findChild<slopkit::ui::widgets::Panel*>();
    REQUIRE(target_panel != nullptr);
    CHECK(panel_has_title(target_panel, QStringLiteral("Target process")));

    auto* target_edit   = dialog.findChild<QLineEdit*>(QStringLiteral("target_edit"));
    auto* exe_path_edit = dialog.findChild<QLineEdit*>(QStringLiteral("exe_path_edit"));
    REQUIRE(target_edit != nullptr);
    REQUIRE(exe_path_edit != nullptr);
    auto* use_attached = dialog.findChild<QPushButton*>(QStringLiteral("use_attached_button"));
    REQUIRE(use_attached != nullptr);
    CHECK_FALSE(use_attached->isEnabled());
    bool reports_detached = false;
    for (auto* label : dialog.findChildren<QLabel*>())
    {
        if (label->text() == QStringLiteral("No process is attached."))
        {
            reports_detached = true;
        }
    }
    CHECK(reports_detached);
    auto check_boxes = dialog.findChildren<QCheckBox*>();
    REQUIRE(check_boxes.size() == 2);
    QCheckBox* auto_attach    = nullptr;
    QCheckBox* match_exe_path = nullptr;
    for (QCheckBox* box : check_boxes)
    {
        if (box->text().contains(QStringLiteral("Auto attach")))
        {
            auto_attach = box;
        }
        else if (box->text().contains(QStringLiteral("Match by")))
        {
            match_exe_path = box;
        }
    }
    REQUIRE(auto_attach != nullptr);
    REQUIRE(match_exe_path != nullptr);

    dialog.show();
    QCoreApplication::processEvents();
    CHECK(target_edit->text().isEmpty());
    CHECK(exe_path_edit->text().isEmpty());
    CHECK_FALSE(auto_attach->isChecked());

    // Enabling auto attach with an empty name is refused and reverted.
    auto_attach->setChecked(true);
    CHECK_FALSE(auto_attach->isChecked());
    CHECK_FALSE(table.settings().auto_attach);

    // A path alone does not satisfy the name the toggle still selects.
    exe_path_edit->setText(QStringLiteral("/usr/bin/game"));
    emit exe_path_edit->editingFinished();
    CHECK(table.settings().exe_path == "/usr/bin/game");
    auto_attach->setChecked(true);
    CHECK_FALSE(auto_attach->isChecked());

    // Editing the target name writes straight through to the table.
    target_edit->setText(QStringLiteral("game"));
    emit target_edit->editingFinished();
    CHECK(table.settings().target_process == "game");

    // With a name set the toggles stick.
    auto_attach->setChecked(true);
    CHECK(table.settings().auto_attach);
    match_exe_path->setChecked(true);
    CHECK(table.settings().match_exe_path);

    // Ticking the path toggle with an empty path while auto attach is on is
    // refused and reverted.
    match_exe_path->setChecked(false);
    CHECK_FALSE(table.settings().match_exe_path);
    exe_path_edit->setText(QString());
    emit exe_path_edit->editingFinished();
    CHECK(table.settings().exe_path.empty());
    match_exe_path->setChecked(true);
    CHECK_FALSE(match_exe_path->isChecked());
    CHECK_FALSE(table.settings().match_exe_path);
}

TEST_CASE("the table settings dialog fills from the attached process", "[ui]")
{
    application();

    slopkit::table::AddressTable              table;
    slopkit::process::AttachedTarget          target = fake_target();
    slopkit::ui::dialogs::TableSettingsDialog dialog {table, target};

    // Both cards are present.
    bool has_target_panel   = false;
    bool has_attached_panel = false;
    for (auto* panel : dialog.findChildren<slopkit::ui::widgets::Panel*>())
    {
        if (panel_has_title(panel, QStringLiteral("Target process")))
        {
            has_target_panel = true;
        }
        if (panel_has_title(panel, QStringLiteral("Attached process")))
        {
            has_attached_panel = true;
        }
    }
    CHECK(has_target_panel);
    CHECK(has_attached_panel);

    auto* use_attached = dialog.findChild<QPushButton*>(QStringLiteral("use_attached_button"));
    REQUIRE(use_attached != nullptr);
    CHECK(use_attached->isEnabled());

    // The card shows the attached identity and its executable path.
    bool shows_name = false;
    bool shows_path = false;
    for (auto* label : dialog.findChildren<QLabel*>())
    {
        if (label->text().contains(QStringLiteral("fake (42)")))
        {
            shows_name = true;
        }
        if (label->text() == QStringLiteral("/usr/bin/fake"))
        {
            shows_path = true;
        }
    }
    CHECK(shows_name);
    CHECK(shows_path);

    // Pressing the button fills both fields and writes through, leaving the
    // checkboxes exactly as the user set them.
    use_attached->click();
    CHECK(table.settings().target_process == "fake");
    CHECK(table.settings().exe_path == "/usr/bin/fake");
    CHECK_FALSE(table.settings().auto_attach);
    CHECK_FALSE(table.settings().match_exe_path);

    // Detaching flips the card and disables the button.
    target.clear();
    dialog.refresh_target();
    CHECK_FALSE(use_attached->isEnabled());
}

TEST_CASE("the table settings dialog fills only the name when the path is unknown", "[ui]")
{
    application();

    slopkit::table::AddressTable     table;
    slopkit::process::AttachedTarget target = fake_target();
    target.exe_path.clear();
    slopkit::ui::dialogs::TableSettingsDialog dialog {table, target};

    bool reports_unknown = false;
    for (auto* label : dialog.findChildren<QLabel*>())
    {
        if (label->text().contains(QStringLiteral("executable path unknown")))
        {
            reports_unknown = true;
        }
    }
    CHECK(reports_unknown);

    auto* exe_path_edit = dialog.findChild<QLineEdit*>(QStringLiteral("exe_path_edit"));
    auto* use_attached  = dialog.findChild<QPushButton*>(QStringLiteral("use_attached_button"));
    REQUIRE(exe_path_edit != nullptr);
    REQUIRE(use_attached != nullptr);

    // Only the name is filled; an existing path is left alone.
    exe_path_edit->setText(QStringLiteral("/keep/me"));
    emit exe_path_edit->editingFinished();
    use_attached->click();
    CHECK(table.settings().target_process == "fake");
    CHECK(table.settings().exe_path == "/keep/me");
}
