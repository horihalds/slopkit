#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "ui/components/code_patch.hpp"

namespace
{
    using slopkit::ui::components::CodePatch;
    using slopkit::ui::components::CodePatchTable;
    using slopkit::ui::components::nop_bytes;

    std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values)
    {
        std::vector<std::byte> bytes;
        bytes.reserve(values.size());
        for (const unsigned value : values)
        {
            bytes.push_back(static_cast<std::byte>(value));
        }
        return bytes;
    }
} // namespace

TEST_CASE("nop_bytes fills one 0x90 per byte", "[ui]")
{
    const std::vector<std::byte> five = nop_bytes(5);
    REQUIRE(five.size() == 5);
    for (const std::byte value : five)
    {
        CHECK(value == std::byte {0x90});
    }
    CHECK(nop_bytes(0).empty());
}

TEST_CASE("the code patch table records, looks up and forgets NOP patches", "[ui]")
{
    CodePatchTable table;
    CHECK(table.empty());
    CHECK(table.size() == 0);

    const CodePatch& patch = table.apply_nop(0x2001, 3, bytes_of({0x48, 0x89, 0xE5}), QStringLiteral("MOV RBP, RSP"));
    CHECK(patch.begin == 0x2001);
    CHECK(patch.length == 3);
    CHECK(patch.end() == 0x2004);
    CHECK(patch.original_bytes == bytes_of({0x48, 0x89, 0xE5}));
    CHECK(patch.original_text == QStringLiteral("MOV RBP, RSP"));
    CHECK(table.size() == 1);
    CHECK_FALSE(table.empty());

    // The region covers every byte of the replaced instruction, nothing before
    // it and nothing past its end.
    REQUIRE(table.covering(0x2001) != nullptr);
    REQUIRE(table.covering(0x2003) != nullptr);
    CHECK(table.covering(0x2001)->begin == 0x2001);
    CHECK(table.covering(0x2003)->begin == 0x2001);
    CHECK(table.covering(0x2000) == nullptr);
    CHECK(table.covering(0x2004) == nullptr);

    // A second apply at the same begin replaces the record.
    table.apply_nop(0x2001, 1, bytes_of({0x90}), QStringLiteral("NOP"));
    CHECK(table.size() == 1);
    REQUIRE(table.covering(0x2001) != nullptr);
    CHECK(table.covering(0x2001)->length == 1);
    CHECK(table.covering(0x2002) == nullptr);

    // Restore erases; an unknown address reports false.
    CHECK(table.restore(0x2001));
    CHECK(table.empty());
    CHECK_FALSE(table.restore(0x2001));
}

TEST_CASE("the code patch table drops every record when the target changes", "[ui]")
{
    CodePatchTable table;
    table.note_target(7);
    table.apply_nop(0x2001, 1, bytes_of({0x55}), QStringLiteral("PUSH RBP"));
    CHECK(table.size() == 1);

    // The same target keeps the records.
    table.note_target(7);
    CHECK(table.size() == 1);

    // Another process clears them, and so does a detach (pid 0).
    table.note_target(8);
    CHECK(table.empty());
    table.apply_nop(0x2001, 1, bytes_of({0x55}), QStringLiteral("PUSH RBP"));
    table.note_target(0);
    CHECK(table.empty());
}
