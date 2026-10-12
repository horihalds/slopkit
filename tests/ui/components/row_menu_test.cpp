#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

#include <vector>

#include <QKeySequence>

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

    QMenu* submenu(QMenu& menu, const QString& title)
    {
        for (QAction* candidate : menu.actions())
        {
            if (candidate->menu() != nullptr && candidate->menu()->title() == title)
            {
                return candidate->menu();
            }
        }
        return nullptr;
    }
}

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

TEST_CASE("a submenu carries the parent's hover explanations", "[ui]")
{
    application();

    QMenu  menu;
    QMenu* submenu = slopkit::ui::widgets::explained_submenu(menu, QStringLiteral("Copy"));
    REQUIRE(submenu != nullptr);
    CHECK(submenu->title() == QStringLiteral("Copy"));
    CHECK(submenu->toolTipsVisible());
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

    QMenu* watch = submenu(menu, QStringLiteral("Watch accesses"));
    REQUIRE(watch != nullptr);
    CHECK(watch->toolTipsVisible());

    QAction* writes   = action(*watch, QStringLiteral("Find out what writes this address"));
    QAction* accesses = action(*watch, QStringLiteral("Find out what accesses this address"));
    REQUIRE(writes != nullptr);
    REQUIRE(accesses != nullptr);
    CHECK_FALSE(writes->isEnabled());
    CHECK_FALSE(accesses->isEnabled());
    CHECK(writes->toolTip() == QStringLiteral("Attach to a target first."));

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

    QMenu* watch = submenu(menu, QStringLiteral("Watch accesses"));
    REQUIRE(watch != nullptr);

    QAction* writes   = action(*watch, QStringLiteral("Find out what writes this address"));
    QAction* accesses = action(*watch, QStringLiteral("Find out what accesses this address"));
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

TEST_CASE("a row-menu entry can show another action's key as a hint", "[ui]")
{
    application();

    QMenu    menu;
    QAction* source = menu.addAction(QStringLiteral("Edit"));
    source->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));

    QAction* item = slopkit::ui::widgets::described_action(
        menu, QStringLiteral("Change value…"), QStringLiteral("Writes the typed value."));
    slopkit::ui::widgets::show_shortcut_hint(*item, *source);

    // The key is copied and pinned as a widget-local hint that never claims it.
    CHECK(item->shortcut() == QKeySequence(QStringLiteral("Ctrl+E")));
    CHECK(item->shortcutContext() == Qt::WidgetShortcut);
    CHECK(item->text() == QStringLiteral("Change value…"));
    CHECK(item->toolTip() == QStringLiteral("Writes the typed value."));
    CHECK(item->isEnabled());
    CHECK(source->shortcut() == QKeySequence(QStringLiteral("Ctrl+E")));
}
