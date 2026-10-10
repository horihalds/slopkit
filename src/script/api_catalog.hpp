#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

// The scriptable names the engine binds, with the signature and one-line summary
// the script editor shows while completing and hinting. The table is the single
// source of truth: the highlighter, the completion model and the argument hints
// all read it, and `tests/script/api_catalog_test.cpp` proves it against a live
// engine. It stays free of Qt and Lua headers so both the script module and the
// UI can use it.
namespace slopkit::script
{
    enum class ApiKind
    {
        global_function, // read_u8, aobscan, ...
        table,           // mem, hook
        table_function,  // mem.read, hook.install
        statement,       // a reserved global that is not a plain function
    };

    struct ApiEntry
    {
        std::string_view name;      // "mem.read" - a dot marks a table member
        std::string_view signature; // "mem.read(address, token)"
        std::string_view summary;   // one line, shown under the name
        ApiKind          kind;
    };

    // Every global the engine binds, and every member of the tables it binds.
    [[nodiscard]] std::span<const ApiEntry> api_catalog();

    // The Lua standard library the engine opens: the base globals (minus `print`,
    // which the engine rebinds) and the `string`/`table`/`math` table names.
    [[nodiscard]] std::span<const std::string_view> lua_standard_globals();

    // The `string`/`table`/`math` members, as dotted names ("string.format").
    [[nodiscard]] std::span<const std::string_view> lua_library_members();

    // The catalogue entry with exactly this name, or nullptr.
    [[nodiscard]] const ApiEntry* find(std::string_view name);

    // Every catalogue entry that is a member of `table` ("mem" -> mem.read, ...).
    [[nodiscard]] std::vector<const ApiEntry*> members_of(std::string_view table);

    // The catalogue entries a receiver and a prefix select, alphabetically:
    // with an empty receiver, the globals whose name starts with `prefix`; with a
    // receiver ("mem"), that table's members whose member part starts with
    // `prefix`. Matching is case-insensitive.
    [[nodiscard]] std::vector<const ApiEntry*> matches(std::string_view receiver, std::string_view prefix);

    // The index of the parameter a signature should emphasise when a call has
    // `typed` parameters behind the caret, clamped to the signature's own count.
    [[nodiscard]] std::size_t parameter_index(std::string_view signature, std::size_t typed);
} // namespace slopkit::script
