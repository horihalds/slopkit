#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the log dialog shows live records and filters them", "[ui]")
{
    application();

    using slopkit::log::Level;

    slopkit::log::Logger::instance().clear_history();
    slopkit::log::Logger::instance().set_minimum_level(Level::debug);

    slopkit::ui::dialogs::LogDialog dialog;
    dialog.show();

    auto* view = dialog.findChild<QPlainTextEdit*>();
    REQUIRE(view != nullptr);
    auto* combo = dialog.findChild<QComboBox*>();
    REQUIRE(combo != nullptr);
    auto* search = dialog.findChild<QLineEdit*>();
    REQUIRE(search != nullptr);
    auto* status = dialog.findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("log_status"));
    REQUIRE(status != nullptr);

    slopkit::log::warning(slopkit::log::category::scan, "something happened");
    QCoreApplication::processEvents();
    CHECK(view->toPlainText().contains(QStringLiteral("warning")));
    CHECK(view->toPlainText().contains(QStringLiteral("[scan] something happened")));

    combo->setCurrentIndex(4);
    CHECK_FALSE(view->toPlainText().contains(QStringLiteral("something happened")));
    combo->setCurrentIndex(3);
    CHECK(view->toPlainText().contains(QStringLiteral("something happened")));

    search->setText(QStringLiteral("SCAN"));
    CHECK(view->toPlainText().contains(QStringLiteral("something happened")));
    search->setText(QStringLiteral("nope"));
    CHECK(view->toPlainText().isEmpty());

    search->clear();
    auto* clear = button_labelled(dialog, QStringLiteral("Clear"));
    REQUIRE(clear != nullptr);
    clear->click();
    CHECK(view->toPlainText().isEmpty());
    CHECK(slopkit::log::Logger::instance().history().empty());
    CHECK(status->text().contains(QStringLiteral("0 record(s)")));

    slopkit::log::Logger::instance().set_minimum_level(Level::info);
}

TEST_CASE("the log dialog seeds itself from the retained history", "[ui]")
{
    application();

    using slopkit::log::Level;

    slopkit::log::Logger::instance().clear_history();
    slopkit::log::Logger::instance().set_minimum_level(Level::info);

    slopkit::log::info(slopkit::log::category::plugin, "logged before the window opened");

    slopkit::ui::dialogs::LogDialog dialog;
    auto*                           view = dialog.findChild<QPlainTextEdit*>();
    REQUIRE(view != nullptr);

    CHECK(view->toPlainText().isEmpty());

    // A record queued between construction and the first show must not be
    // duplicated by the history seeding.
    slopkit::log::info(slopkit::log::category::ui, "queued before the first show");
    dialog.show();

    const QString text = view->toPlainText();
    CHECK(text.contains(QStringLiteral("[plugin] logged before the window opened")));
    CHECK(text.count(QStringLiteral("queued before the first show")) == 1);

    slopkit::log::Logger::instance().set_minimum_level(Level::info);
}

TEST_CASE("a UI action records under the ui category", "[ui]")
{
    application();

    LevelGuard level;
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    slopkit::table::AddressTable     table;
    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};
    panel.toggle_active_selected();

    bool saw_ui = false;
    for (const auto& record : records)
    {
        if (std::string_view {record.category} == slopkit::log::category::ui
            && record.level == slopkit::log::Level::warning
            && record.message.find("active toggle requested with no selection") != std::string::npos)
        {
            saw_ui = true;
        }
    }
    CHECK(saw_ui);
}
