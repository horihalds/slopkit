#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the companion bar carries a button for every companion window", "[ui]")
{
    application();

    slopkit::ui::panels::CompanionBar bar;

    const QList<QPushButton*> buttons = bar.findChildren<QPushButton*>();
    REQUIRE(buttons.size() == 6);
    CHECK(buttons[0]->text() == QStringLiteral("Memory View"));
    CHECK(buttons[1]->text() == QStringLiteral("ReClass"));
    CHECK(buttons[2]->text() == QStringLiteral("Breakpoints"));
    CHECK(buttons[3]->text() == QStringLiteral("Access Watch"));
    CHECK(buttons[4]->text() == QStringLiteral("Log"));
    CHECK(buttons[5]->text() == QStringLiteral("Settings"));
    for (QPushButton* button : buttons)
    {
        CHECK_FALSE(button->toolTip().isEmpty());
    }

    // The labels are the ones the View menu entries use, so the bar needs the
    // window's whole width and never sits in a narrower pane.
    int labels_width = 0;
    for (QPushButton* button : buttons)
    {
        labels_width += button->sizeHint().width();
    }
    bar.resize(labels_width + 32, 40);
    bar.show();
    QCoreApplication::processEvents();
    for (QPushButton* button : buttons)
    {
        CHECK(button->width() >= button->sizeHint().width());
    }

    // Memory View keeps its target gate; the five companion buttons mirror the
    // always-enabled View menu entries.
    CHECK_FALSE(buttons[0]->isEnabled());
    for (int index = 1; index < buttons.size(); ++index)
    {
        CHECK(buttons[index]->isEnabled());
    }
    bar.set_target_attached(true);
    for (QPushButton* button : buttons)
    {
        CHECK(button->isEnabled());
    }

    CHECK(bar.tab_order_first() == buttons[0]);
    CHECK(bar.tab_order_last() == buttons[5]);
    for (int index = 0; index + 1 < buttons.size(); ++index)
    {
        check_tab_order(bar, buttons[index], buttons[index + 1]);
    }

    int memory_views = 0;
    int reclass      = 0;
    int breakpoints  = 0;
    int access_watch = 0;
    int logs         = 0;
    int settings     = 0;
    QObject::connect(&bar,
                     &slopkit::ui::panels::CompanionBar::memoryViewRequested,
                     &bar,
                     [&]
                     {
                         ++memory_views;
                     });
    QObject::connect(&bar,
                     &slopkit::ui::panels::CompanionBar::reclassRequested,
                     &bar,
                     [&]
                     {
                         ++reclass;
                     });
    QObject::connect(&bar,
                     &slopkit::ui::panels::CompanionBar::breakpointsRequested,
                     &bar,
                     [&]
                     {
                         ++breakpoints;
                     });
    QObject::connect(&bar,
                     &slopkit::ui::panels::CompanionBar::accessWatchWindowRequested,
                     &bar,
                     [&]
                     {
                         ++access_watch;
                     });
    QObject::connect(&bar,
                     &slopkit::ui::panels::CompanionBar::logRequested,
                     &bar,
                     [&]
                     {
                         ++logs;
                     });
    QObject::connect(&bar,
                     &slopkit::ui::panels::CompanionBar::settingsRequested,
                     &bar,
                     [&]
                     {
                         ++settings;
                     });

    for (QPushButton* button : buttons)
    {
        button->click();
    }

    // Each click emits exactly its own signal and no other.
    CHECK(memory_views == 1);
    CHECK(reclass == 1);
    CHECK(breakpoints == 1);
    CHECK(access_watch == 1);
    CHECK(logs == 1);
    CHECK(settings == 1);
}
