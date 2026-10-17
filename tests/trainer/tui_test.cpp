#include <catch2/catch.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include "trainer/runtime/tui.hpp"

namespace
{
    using slopkit::trainer::ScriptState;
    using slopkit::trainer::tui::Action;
    using slopkit::trainer::tui::Model;
    using slopkit::trainer::tui::Row;

    std::vector<ScriptState> scripts(std::initializer_list<bool> active)
    {
        std::vector<ScriptState> states(active.size());
        std::size_t              position = 0;
        for (const bool flag : active)
        {
            states[position++].active = flag;
        }
        return states;
    }
}

TEST_CASE("the list is the value entries and then the scripts", "[trainer][tui]")
{
    const auto states = scripts({false, true});
    Model      model {2, states};

    REQUIRE(model.row_count() == 4);
    CHECK(model.row(0).kind == Row::Kind::value);
    CHECK(model.row(0).number == 1);
    CHECK(model.row(1).kind == Row::Kind::value);
    CHECK(model.row(1).number == 2);
    CHECK(model.row(2).kind == Row::Kind::script);
    CHECK(model.row(2).number == 1);
    CHECK(model.row(3).kind == Row::Kind::script);
    CHECK(model.row(3).number == 2);
}

TEST_CASE("a click toggles the row it lands on", "[trainer][tui]")
{
    const auto states = scripts({false, true});
    Model      model {1, states};

    const auto value = model.toggle_row(0);
    CHECK(value.kind == Action::Kind::freeze);
    CHECK(value.index == 1);

    // An idle script activates, an active one deactivates: the two spaces never
    // share a number.
    const auto idle = model.toggle_row(1);
    CHECK(idle.kind == Action::Kind::activate_script);
    CHECK(idle.index == 1);

    const auto active = model.toggle_row(2);
    CHECK(active.kind == Action::Kind::deactivate_script);
    CHECK(active.index == 2);

    CHECK(model.toggle_row(3).kind == Action::Kind::none);
}

TEST_CASE("a failed script is retried by the same click", "[trainer][tui]")
{
    auto states      = scripts({false});
    states[0].failed = true;

    Model model {0, states};
    CHECK(model.toggle_row(0).kind == Action::Kind::activate_script);
}

TEST_CASE("the input line runs the console's own grammar", "[trainer][tui]")
{
    const auto states = scripts({false});
    Model      model {2, states};

    const auto freeze = model.command("f2");
    CHECK(freeze.kind == Action::Kind::freeze);
    CHECK(freeze.index == 2);

    const auto set = model.command("s 1 42");
    CHECK(set.kind == Action::Kind::set_value);
    CHECK(set.index == 1);
    CHECK(set.value == "42");

    const auto activate = model.command("a1");
    CHECK(activate.kind == Action::Kind::activate_script);
    CHECK(activate.index == 1);

    const auto deactivate = model.command("d 2");
    CHECK(deactivate.kind == Action::Kind::deactivate_script);
    CHECK(deactivate.index == 2);

    CHECK(model.command("r").kind == Action::Kind::refresh);
    CHECK(model.command("q").kind == Action::Kind::quit);

    // `l` is a redraw and `?` opens the help: neither reaches the session.
    CHECK(model.command("l").kind == Action::Kind::none);
    model.set_help_shown(false);
    CHECK(model.command("?").kind == Action::Kind::none);
    CHECK(model.help_shown());
}

TEST_CASE("an unreadable line is answered on the status line", "[trainer][tui]")
{
    Model model {1, {}};

    const auto action = model.command("nonsense");
    CHECK(action.kind == Action::Kind::none);
    CHECK(model.message().find("nonsense") != std::string::npos);
    CHECK(model.message().find("?") != std::string::npos);
}

TEST_CASE("entering a line runs it and clears the input line", "[trainer][tui]")
{
    Model model {2, {}};

    CHECK(model.submit().kind == Action::Kind::none);

    model.append_input("f");
    model.append_input("1");
    CHECK(model.input() == "f1");

    const auto action = model.submit();
    CHECK(action.kind == Action::Kind::freeze);
    CHECK(action.index == 1);
    CHECK(model.input().empty());

    model.append_input("s 1 42");
    model.backspace_input();
    CHECK(model.input() == "s 1 4");
    model.backspace_input();
    model.backspace_input();
    CHECK(model.input() == "s 1");
    model.clear_input();
    CHECK(model.input().empty());
}

TEST_CASE("the selection stays inside the list and inside the window", "[trainer][tui]")
{
    Model model {4, scripts({false})};

    model.set_visible_rows(2);
    CHECK(model.selected() == 0);
    CHECK(model.scroll() == 0);

    model.move(1);
    CHECK(model.selected() == 1);
    CHECK(model.scroll() == 0);

    model.move(1);
    CHECK(model.selected() == 2);
    CHECK(model.scroll() == 1);

    model.select_last();
    CHECK(model.selected() == 4);
    CHECK(model.scroll() == 3);

    model.move(10);
    CHECK(model.selected() == 4);

    model.select_first();
    CHECK(model.selected() == 0);
    CHECK(model.scroll() == 0);

    model.move(-5);
    CHECK(model.selected() == 0);
}

TEST_CASE("a click offset resolves only inside the visible window", "[trainer][tui]")
{
    Model model {4, scripts({false})};
    model.set_visible_rows(2);

    CHECK(model.row_at_offset(0) == 0);
    CHECK(model.row_at_offset(1) == 1);
    CHECK(model.row_at_offset(2) == std::nullopt);

    model.select_last();
    CHECK(model.scroll() == 3);
    CHECK(model.row_at_offset(0) == 3);
    CHECK(model.row_at_offset(1) == 4);
    CHECK(model.row_at_offset(2) == std::nullopt);
}

TEST_CASE("an empty window has nothing to click", "[trainer][tui]")
{
    Model model {4, scripts({false})};

    model.set_visible_rows(0);
    CHECK(model.scroll() == 0);
    CHECK(model.row_at_offset(0) == std::nullopt);
}

TEST_CASE("scripts appearing or going away clamp the selection", "[trainer][tui]")
{
    Model model {0, scripts({false, false})};
    model.select_last();
    CHECK(model.selected() == 1);

    const auto fewer = scripts({false});
    model.set_scripts(fewer);
    CHECK(model.row_count() == 1);
    CHECK(model.selected() == 0);

    const auto none = scripts({});
    model.set_scripts(none);
    CHECK(model.row_count() == 0);
    CHECK(model.selected() == 0);
    CHECK(model.toggle_row(0).kind == Action::Kind::none);

    model.move(1);
    CHECK(model.selected() == 0);
}
