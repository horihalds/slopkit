#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

#include "ui/components/script_context.hpp"

namespace
{
    using slopkit::ui::components::script_context::document_names;
    using slopkit::ui::components::script_context::enclosing_call;
    using slopkit::ui::components::script_context::expression_instance_class;
    using slopkit::ui::components::script_context::identifier_at;
    using slopkit::ui::components::script_context::instance_class;
    using slopkit::ui::components::script_context::is_code_at;
    using slopkit::ui::components::script_context::matching_bracket;
}

TEST_CASE("identifier_at finds the word the caret touches", "[ui]")
{
    application();

    const QString text = QStringLiteral("aobscan");

    for (const int position : {0, 3, 7})
    {
        const auto identifier = identifier_at(text, position);
        REQUIRE(identifier.has_value());
        CHECK(identifier->name == QStringLiteral("aobscan"));
        CHECK(identifier->begin == 0);
        CHECK(identifier->end == 7);
        CHECK(identifier->receiver.isEmpty());
    }

    const QString dotted = QStringLiteral("mem.re");
    const auto    after  = identifier_at(dotted, 6);
    REQUIRE(after.has_value());
    CHECK(after->receiver == QStringLiteral("mem"));
    CHECK(after->name == QStringLiteral("re"));
    CHECK(after->begin == 4);
    CHECK(after->end == 6);

    const auto empty_prefix = identifier_at(QStringLiteral("mem."), 4);
    REQUIRE(empty_prefix.has_value());
    CHECK(empty_prefix->receiver == QStringLiteral("mem"));
    CHECK(empty_prefix->name.isEmpty());

    // A dot after a call hangs off the whole call expression.
    const QString call  = QStringLiteral("structs.get(\"Player\"):at(0x1000).he");
    const auto    field = identifier_at(call, call.size());
    REQUIRE(field.has_value());
    CHECK(field->receiver == QStringLiteral("structs.get(\"Player\"):at(0x1000)"));
    CHECK(field->name == QStringLiteral("he"));

    const auto bare_call = identifier_at(QStringLiteral("f()."), 4);
    REQUIRE(bare_call.has_value());
    CHECK(bare_call->receiver == QStringLiteral("f()"));
    CHECK(bare_call->name.isEmpty());

    CHECK_FALSE(identifier_at(QStringLiteral("a  b"), 2).has_value());
    CHECK_FALSE(identifier_at(QString {}, 0).has_value());
}

TEST_CASE("identifier_at ignores comments and strings", "[ui]")
{
    application();

    CHECK_FALSE(is_code_at(QStringLiteral("-- aob"), 5));
    CHECK_FALSE(identifier_at(QStringLiteral("-- aob"), 6).has_value());
    CHECK_FALSE(identifier_at(QStringLiteral("x = \"aob\""), 8).has_value());
    CHECK_FALSE(identifier_at(QStringLiteral("x = [[aob]]"), 7).has_value());

    CHECK(is_code_at(QStringLiteral("-- c\nmem"), 6));
    const auto identifier = identifier_at(QStringLiteral("-- c\nmem"), 7);
    REQUIRE(identifier.has_value());
    CHECK(identifier->name == QStringLiteral("mem"));
}

TEST_CASE("enclosing_call reports the innermost call and the active parameter", "[ui]")
{
    application();

    const auto second = enclosing_call(QStringLiteral("mem.read(0x1000, "), 17);
    REQUIRE(second.has_value());
    CHECK(second->receiver == QStringLiteral("mem"));
    CHECK(second->name == QStringLiteral("read"));
    CHECK(second->open_paren == 8);
    CHECK(second->active_parameter == 1);

    const auto first = enclosing_call(QStringLiteral("mem.read("), 9);
    REQUIRE(first.has_value());
    CHECK(first->name == QStringLiteral("read"));
    CHECK(first->active_parameter == 0);

    const QString nested_text = QStringLiteral("hook.install({ pattern = ");
    const auto    nested      = enclosing_call(nested_text, nested_text.size());
    REQUIRE(nested.has_value());
    CHECK(nested->receiver == QStringLiteral("hook"));
    CHECK(nested->name == QStringLiteral("install"));
    CHECK(nested->active_parameter == 0);

    const auto inner = enclosing_call(QStringLiteral("mem.read(alloc("), 15);
    REQUIRE(inner.has_value());
    CHECK(inner->name == QStringLiteral("alloc"));

    const auto unfinished = enclosing_call(QStringLiteral("mem.read(0x1000"), 14);
    REQUIRE(unfinished.has_value());
    CHECK(unfinished->name == QStringLiteral("read"));

    CHECK_FALSE(enclosing_call(QStringLiteral("mem.read(0x1000)"), 16).has_value());
    CHECK_FALSE(enclosing_call(QStringLiteral("local x = 1"), 11).has_value());
}

TEST_CASE("document_names lists the definitions in the text", "[ui]")
{
    application();

    const QString text  = QStringLiteral("function activate()\n"
                                         "    local function helper(a, b)\n"
                                         "    local kHook\n"
                                         "end\n");
    const auto    names = document_names(text);
    REQUIRE(names.size() == 3);
    CHECK(names[0].name == QStringLiteral("activate"));
    CHECK(names[0].signature == QStringLiteral("activate()"));
    CHECK(names[0].function);
    CHECK(names[1].name == QStringLiteral("helper"));
    CHECK(names[1].signature == QStringLiteral("helper(a, b)"));
    CHECK(names[1].function);
    CHECK(names[2].name == QStringLiteral("kHook"));
    CHECK_FALSE(names[2].function);
}

TEST_CASE("document_names ignores names inside comments and strings", "[ui]")
{
    application();

    CHECK(document_names(QStringLiteral("-- function commented()\n")).empty());
    const auto names = document_names(QStringLiteral("local s = \"function fake()\"\n"));
    REQUIRE(names.size() == 1);
    CHECK(names[0].name == QStringLiteral("s"));
}

TEST_CASE("matching_bracket pairs code brackets only", "[ui]")
{
    application();

    CHECK(matching_bracket(QStringLiteral("f(x)"), 1) == 3);
    CHECK(matching_bracket(QStringLiteral("f(x)"), 3) == 1);
    CHECK(matching_bracket(QStringLiteral("f[1]"), 1) == 3);
    CHECK(matching_bracket(QStringLiteral("f(x"), 1) == -1);
    CHECK(matching_bracket(QStringLiteral("fx"), 1) == -1);
    CHECK(matching_bracket(QStringLiteral("f(\")\")"), 1) == 5);
}

TEST_CASE("instance_class reads the class a variable is statically bound to", "[ui]")
{
    application();

    // The completion asks with the caret's position; a whole-text ask is what
    // the end-of-document caret sees.
    const auto bound = [](const QString& text, const QString& variable)
    {
        return instance_class(text, text.size(), variable);
    };

    const QString direct = QStringLiteral("local p = structs.get(\"Player\"):at(0x1000)\n");
    CHECK(bound(direct, QStringLiteral("p")) == QStringLiteral("Player"));
    CHECK_FALSE(bound(direct, QStringLiteral("q")).has_value());

    const QString handle = QStringLiteral("local Player = structs.get(\"Player\")\n"
                                          "local p = Player:at(game.exe + 1A2B40)\n");
    CHECK(bound(handle, QStringLiteral("p")) == QStringLiteral("Player"));

    const QString alias = QStringLiteral("local p = structs.get(\"Player\"):at(0)\n"
                                         "local q = p\n"
                                         "local r = q\n");
    CHECK(bound(alias, QStringLiteral("r")) == QStringLiteral("Player"));

    // A handle on its own is not an instance, and a reassignment wins.
    CHECK_FALSE(
        bound(QStringLiteral("local Player = structs.get(\"Player\")\n"), QStringLiteral("Player")).has_value());
    CHECK_FALSE(
        bound(QStringLiteral("local p = structs.get(\"Player\"):at(0)\np = something_else()\n"), QStringLiteral("p"))
            .has_value());

    // A binding written after the caret does not decide it.
    CHECK_FALSE(instance_class(direct, 5, QStringLiteral("p")).has_value());

    // A comment, a string and a comparison bind nothing.
    CHECK_FALSE(bound(QStringLiteral("-- local p = structs.get(\"Player\"):at(0)\n"), QStringLiteral("p")).has_value());
    CHECK_FALSE(
        bound(QStringLiteral("local s = \"p = structs.get('Player'):at(0)\"\n"), QStringLiteral("p")).has_value());
    CHECK_FALSE(bound(QStringLiteral("p == structs.get(\"Player\"):at(0)\n"), QStringLiteral("p")).has_value());

    // A handle and a field path name no instance.
    CHECK_FALSE(bound(QStringLiteral("local p = structs.get(\"Player\")\n"), QStringLiteral("p")).has_value());
    CHECK_FALSE(bound(direct, QStringLiteral("p.weapon")).has_value());
}

TEST_CASE("expression_instance_class reads the class an inline instance is built from", "[ui]")
{
    application();

    const auto before = [](const QString& text)
    {
        return expression_instance_class(text, text.lastIndexOf(QLatin1Char('.')));
    };

    const QString inline_call = QStringLiteral("structs.get(\"Player\"):at(0x1000).health");
    CHECK(before(inline_call) == QStringLiteral("Player"));

    // A nested call in the argument list keeps the chain intact.
    CHECK(before(QStringLiteral("structs.get(\"Player\"):at(expression(\"game.exe+1A2B40\")).health"))
          == QStringLiteral("Player"));

    // A handle variable's own call, and a variable holding an inline instance.
    CHECK(before(QStringLiteral("local Player = structs.get(\"Player\")\nPlayer:at(0x1000).health"))
          == QStringLiteral("Player"));
    CHECK(before(QStringLiteral("local p = structs.get(\"Player\"):at(0)\np.")) == QStringLiteral("Player"));

    // No class ends there, and a dot in a string is not code.
    CHECK_FALSE(before(QStringLiteral("f().health")).has_value());
    CHECK_FALSE(before(QStringLiteral("mem.read.health")).has_value());
    CHECK_FALSE(expression_instance_class(QStringLiteral("local s = \"a.b\""), 12).has_value());
}
