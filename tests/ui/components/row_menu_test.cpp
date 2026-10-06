#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

#include <vector>

#include "ui/components/row_menu.hpp"

namespace
{
    using slopkit::ui::widgets::WatchCommand;

    QAction* action(QMenu& menu, const QString& text)
    {
        for (QAction* candidate : menu.actions())
        {
            if (candidate->text() == text)
            {
                return candidate;
            }
        }
        return nullptr;
    }
} // namespace

TEST_CASE("the row-menu factories set the enabled flag and the explanation", "[ui]")
{
    application();

    QMenu menu;
    slopkit::ui::widgets::show_explanations(menu);
    CHECK(menu.toolTipsVisible());

    QAction* described =
        slopkit::ui::widgets::described_action(menu, QStringLiteral("Do the thing"), QStringLiteral("Does the thing."));
    REQUIRE(described != nullptr);
    CHECK(described->text() == QStringLiteral("Do the thing"));
    CHECK(described->isEnabled());
    CHECK(described->toolTip() == QStringLiteral("Does the thing."));

    QAction* disabled =
        slopkit::ui::widgets::disabled_action(menu, QStringLiteral("Do the other"), QStringLiteral("Not yet."));
    REQUIRE(disabled != nullptr);
    CHECK_FALSE(disabled->isEnabled());
    CHECK(disabled->toolTip() == QStringLiteral("Not yet."));
}

TEST_CASE("the shared watch commands are disabled without a target", "[ui]")
{
    application();

    QMenu menu;
    int   armed = 0;
    slopkit::ui::widgets::add_watch_commands(menu,
                                             false,
                                             [&armed](WatchCommand)
                                             {
                                                 ++armed;
                                             });

    QAction* writes   = action(menu, QStringLiteral("Find out what writes this address"));
    QAction* accesses = action(menu, QStringLiteral("Find out what accesses this address"));
    REQUIRE(writes != nullptr);
    REQUIRE(accesses != nullptr);
    CHECK_FALSE(writes->isEnabled());
    CHECK_FALSE(accesses->isEnabled());
    CHECK(writes->toolTip() == QStringLiteral("Attach to a target first."));

    // A disabled entry cannot be triggered, so no command is armed.
    writes->trigger();
    accesses->trigger();
    CHECK(armed == 0);
}

TEST_CASE("the shared watch commands report the picked command with a target", "[ui]")
{
    application();

    QMenu                     menu;
    std::vector<WatchCommand> armed;
    slopkit::ui::widgets::add_watch_commands(menu,
                                             true,
                                             [&armed](WatchCommand command)
                                             {
                                                 armed.push_back(command);
                                             });

    QAction* writes   = action(menu, QStringLiteral("Find out what writes this address"));
    QAction* accesses = action(menu, QStringLiteral("Find out what accesses this address"));
    REQUIRE(writes != nullptr);
    REQUIRE(accesses != nullptr);
    CHECK(writes->isEnabled());
    CHECK(accesses->isEnabled());
    CHECK_FALSE(writes->toolTip().isEmpty());

    writes->trigger();
    accesses->trigger();
    REQUIRE(armed.size() == 2);
    CHECK(armed[0] == WatchCommand::writes);
    CHECK(armed[1] == WatchCommand::accesses);
}
