#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

#include "ui/components/script_context.hpp"

namespace
{
    using slopkit::ui::components::script_context::document_names;
    using slopkit::ui::components::script_context::enclosing_call;
    using slopkit::ui::components::script_context::identifier_at;
    using slopkit::ui::components::script_context::is_code_at;
    using slopkit::ui::components::script_context::matching_bracket;
} // namespace

TEST_CASE("identifier_at finds the word the caret touches", "[ui]")
{
    application();

    const QString text = QStringLiteral("aobscan");

    // The start, middle and end of a word all name the whole word.
    for (const int position : {0, 3, 7})
    {
        const auto identifier = identifier_at(text, position);
        REQUIRE(identifier.has_value());
        CHECK(identifier->name == QStringLiteral("aobscan"));
        CHECK(identifier->begin == 0);
        CHECK(identifier->end == 7);
        CHECK(identifier->receiver.isEmpty());
    }

    // After a dot the receiver is split out.
    const QString dotted = QStringLiteral("mem.re");
    const auto    after  = identifier_at(dotted, 6);
    REQUIRE(after.has_value());
    CHECK(after->receiver == QStringLiteral("mem"));
    CHECK(after->name == QStringLiteral("re"));
    CHECK(after->begin == 4);
    CHECK(after->end == 6);

    // A caret right after the dot yields the receiver and an empty name.
    const auto empty_prefix = identifier_at(QStringLiteral("mem."), 4);
    REQUIRE(empty_prefix.has_value());
    CHECK(empty_prefix->receiver == QStringLiteral("mem"));
    CHECK(empty_prefix->name.isEmpty());

    // Whitespace between words and the empty document give nothing.
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

    // Code after a comment on the next line is code again.
    CHECK(is_code_at(QStringLiteral("-- c\nmem"), 6));
    const auto identifier = identifier_at(QStringLiteral("-- c\nmem"), 7);
    REQUIRE(identifier.has_value());
    CHECK(identifier->name == QStringLiteral("mem"));
}

TEST_CASE("enclosing_call reports the innermost call and the active parameter", "[ui]")
{
    application();

    // `mem.read(0x1000, |` - the second argument.
    const auto second = enclosing_call(QStringLiteral("mem.read(0x1000, "), 17);
    REQUIRE(second.has_value());
    CHECK(second->receiver == QStringLiteral("mem"));
    CHECK(second->name == QStringLiteral("read"));
    CHECK(second->open_paren == 8);
    CHECK(second->active_parameter == 1);

    // `mem.read(|` - the first argument.
    const auto first = enclosing_call(QStringLiteral("mem.read("), 9);
    REQUIRE(first.has_value());
    CHECK(first->name == QStringLiteral("read"));
    CHECK(first->active_parameter == 0);

    // A nested table argument does not add parameters of its own.
    const QString nested_text = QStringLiteral("hook.install({ pattern = ");
    const auto    nested      = enclosing_call(nested_text, nested_text.size());
    REQUIRE(nested.has_value());
    CHECK(nested->receiver == QStringLiteral("hook"));
    CHECK(nested->name == QStringLiteral("install"));
    CHECK(nested->active_parameter == 0);

    // The innermost of two open calls wins.
    const auto inner = enclosing_call(QStringLiteral("mem.read(alloc("), 15);
    REQUIRE(inner.has_value());
    CHECK(inner->name == QStringLiteral("alloc"));

    // An unfinished call still yields a hint.
    const auto unfinished = enclosing_call(QStringLiteral("mem.read(0x1000"), 14);
    REQUIRE(unfinished.has_value());
    CHECK(unfinished->name == QStringLiteral("read"));

    // A closed call and plain code do not.
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
    // A bracket inside a string is not code and does not match.
    CHECK(matching_bracket(QStringLiteral("f(\")\")"), 1) == 5);
}
