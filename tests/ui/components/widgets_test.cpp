#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the action icons are non-null multi-size icons", "[ui]")
{
    application();

    constexpr std::array icons {slopkit::ui::widgets::ActionIcon::cancel,
                                slopkit::ui::widgets::ActionIcon::checkmark,
                                slopkit::ui::widgets::ActionIcon::chip,
                                slopkit::ui::widgets::ActionIcon::diskette,
                                slopkit::ui::widgets::ActionIcon::error,
                                slopkit::ui::widgets::ActionIcon::file,
                                slopkit::ui::widgets::ActionIcon::folder,
                                slopkit::ui::widgets::ActionIcon::home,
                                slopkit::ui::widgets::ActionIcon::info,
                                slopkit::ui::widgets::ActionIcon::ok,
                                slopkit::ui::widgets::ActionIcon::search,
                                slopkit::ui::widgets::ActionIcon::settings,
                                slopkit::ui::widgets::ActionIcon::target,
                                slopkit::ui::widgets::ActionIcon::warning};

    for (const auto which : icons)
    {
        const QIcon icon = slopkit::ui::widgets::action_icon(which);
        CHECK_FALSE(icon.isNull());
        CHECK(icon.availableSizes().contains(QSize(16, 16)));
    }
}
