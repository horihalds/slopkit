#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the settings dialog offers the address display choice", "[ui]")
{
    application();

    SECTION("offers the address display choice")
    {

        slopkit::plugin::PluginHost          host;
        slopkit::scan::ScanEngine            engine;
        slopkit::ui::SettingsController      controller {scratch_settings_file("settings_dialog.ini")};
        slopkit::ui::dialogs::SettingsDialog settings {host, engine, controller};

        auto* categories = settings.findChild<QListWidget*>();
        REQUIRE(categories != nullptr);
        REQUIRE(categories->count() == 7);
        CHECK(categories->item(0)->text() == QStringLiteral("Appearance"));
        CHECK(categories->item(1)->text() == QStringLiteral("Addresses"));
        CHECK(categories->item(2)->text() == QStringLiteral("Live update"));
        CHECK(categories->item(3)->text() == QStringLiteral("Tables"));
        CHECK(categories->item(4)->text() == QStringLiteral("Scanning"));

        auto* module_relative = radio_labelled(settings, QStringLiteral("Module + RVA"));
        REQUIRE(module_relative != nullptr);
        auto* absolute = radio_labelled(settings, QStringLiteral("Absolute address"));
        REQUIRE(absolute != nullptr);
        CHECK(module_relative->isChecked());
        CHECK_FALSE(absolute->isChecked());

        int                      changes = 0;
        slopkit::ui::AddressMode last    = slopkit::ui::AddressMode::module_relative;
        QObject::connect(&controller,
                         &slopkit::ui::SettingsController::addressModeChanged,
                         &controller,
                         [&](slopkit::ui::AddressMode mode)
                         {
                             ++changes;
                             last = mode;
                         });

        absolute->click();
        CHECK(changes == 1);
        CHECK(last == slopkit::ui::AddressMode::absolute);
        CHECK(absolute->isChecked());
        CHECK(controller.values().address_mode == slopkit::ui::AddressMode::absolute);

        // Re-selecting the same value is a no-op: no signal and no radio change.
        controller.set_address_mode(slopkit::ui::AddressMode::absolute);
        CHECK(changes == 1);

        // A real programmatic change keeps the radios in step.
        controller.set_address_mode(slopkit::ui::AddressMode::module_relative);
        CHECK(module_relative->isChecked());
        CHECK(changes == 2);

        // Help > About still reaches the About page after the extra category.
        settings.select_about();
        REQUIRE(categories->currentItem() != nullptr);
        CHECK(categories->currentRow() == categories->count() - 1);
        CHECK(categories->currentItem()->text() == QStringLiteral("About"));
    }

    SECTION("the Tables setting toggles the auto-load switch")
    {

        slopkit::plugin::PluginHost          host;
        slopkit::scan::ScanEngine            engine;
        slopkit::ui::SettingsController      controller {scratch_settings_file("tables_dialog.ini")};
        slopkit::ui::dialogs::SettingsDialog settings {host, engine, controller};

        auto* box = settings.findChild<QCheckBox*>(QStringLiteral("auto_load_last_table"));
        REQUIRE(box != nullptr);
        CHECK_FALSE(box->isChecked());

        bool saw_last_table = false;
        for (auto* label : settings.findChildren<QLabel*>())
        {
            if (label->text().contains(QStringLiteral("Last table:")))
            {
                saw_last_table = true;
            }
        }
        CHECK(saw_last_table);

        int  changes = 0;
        bool last    = false;
        QObject::connect(&controller,
                         &slopkit::ui::SettingsController::autoLoadLastTableChanged,
                         &controller,
                         [&](bool enabled)
                         {
                             ++changes;
                             last = enabled;
                         });

        box->click();
        CHECK(changes == 1);
        CHECK(last);
        CHECK(controller.values().auto_load_last_table);

        // Re-selecting the same value is a no-op.
        controller.set_auto_load_last_table(true);
        CHECK(changes == 1);

        // A real programmatic change ticks the box without re-emitting.
        controller.set_auto_load_last_table(false);
        CHECK_FALSE(box->isChecked());
        CHECK(changes == 2);
    }

    SECTION("the Live update setting drives the interval control")
    {

        slopkit::plugin::PluginHost          host;
        slopkit::scan::ScanEngine            engine;
        slopkit::ui::SettingsController      controller {scratch_settings_file("live_dialog.ini")};
        slopkit::ui::dialogs::SettingsDialog settings {host, engine, controller};

        auto* check    = settings.findChild<QCheckBox*>(QStringLiteral("live_update_enabled"));
        auto* interval = settings.findChild<QSpinBox*>(QStringLiteral("live_update_interval"));
        REQUIRE(check != nullptr);
        REQUIRE(interval != nullptr);
        CHECK(check->isChecked());
        CHECK(interval->isEnabled());
        CHECK(interval->value() == 250);
        CHECK(interval->minimum() == 50);
        CHECK(interval->maximum() == 5000);

        int live_changes     = 0;
        int interval_changes = 0;
        QObject::connect(&controller,
                         &slopkit::ui::SettingsController::liveUpdateChanged,
                         &controller,
                         [&](bool)
                         {
                             ++live_changes;
                         });
        QObject::connect(&controller,
                         &slopkit::ui::SettingsController::liveUpdateIntervalChanged,
                         &controller,
                         [&](int)
                         {
                             ++interval_changes;
                         });

        // Switching off disables the interval and persists the choice.
        check->click();
        CHECK(live_changes == 1);
        CHECK_FALSE(controller.values().live_update_enabled);
        CHECK_FALSE(interval->isEnabled());

        // A new interval persists immediately.
        interval->setValue(1000);
        CHECK(interval_changes == 1);
        CHECK(controller.values().live_update_interval_ms == 1000);

        // A real programmatic change keeps the page in step.
        controller.set_live_update_enabled(true);
        CHECK(check->isChecked());
        CHECK(interval->isEnabled());
        controller.set_live_update_interval_ms(500);
        CHECK(interval->value() == 500);
    }

    SECTION("the Addresses setting switches the viewer live")
    {

        FakeAccess access;

        slopkit::process::ModuleInfo image;
        image.base     = 0x1000;
        image.size     = 0x800;
        image.entry    = 0x1040;
        image.kind     = slopkit::process::ModuleKind::elf;
        image.name     = "low";
        image.path     = "/opt/low";
        access.modules = {image};

        slopkit::plugin::PluginHost      host;
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target = fake_target();
        slopkit::ui::SettingsController  controller {scratch_settings_file("viewer_live.ini")};
        slopkit::ui::MainWindow          window {worker, target, host, controller};

        attach_app_session(worker);

        auto* scanner = window.findChild<slopkit::ui::panels::ScannerPanel*>();
        REQUIRE(scanner != nullptr);
        REQUIRE(pump_ui(worker,
                        [scanner]
                        {
                            return scanner->main_module_address() != 0;
                        }));

        auto* viewer = window.findChild<slopkit::ui::dialogs::MemoryViewerDialog*>();
        REQUIRE(viewer != nullptr);
        auto* document = viewer->findChild<slopkit::ui::components::MemoryViewDocument*>();
        REQUIRE(document != nullptr);

        viewer->set_address(0x1040);
        CHECK(document->display_text(0x1040) == QStringLiteral("low+40"));

        auto* settings = window.findChild<slopkit::ui::dialogs::SettingsDialog*>();
        REQUIRE(settings != nullptr);
        auto* module_relative = radio_labelled(*settings, QStringLiteral("Module + RVA"));
        REQUIRE(module_relative != nullptr);
        auto* absolute = radio_labelled(*settings, QStringLiteral("Absolute address"));
        REQUIRE(absolute != nullptr);
        CHECK(module_relative->isChecked());

        // Clicking the setting re-renders the viewer immediately, with no reload.
        absolute->click();
        CHECK(document->display_text(0x1040) == QStringLiteral("0x1040"));

        module_relative->click();
        CHECK(document->display_text(0x1040) == QStringLiteral("low+40"));
    }
}
