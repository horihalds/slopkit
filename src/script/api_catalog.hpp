#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace slopkit::script
{
    enum class ApiKind
    {
        global_function,
        table,
        table_function,
        statement,
    };

    struct ApiEntry
    {
        std::string_view name;
        std::string_view signature;
        std::string_view summary;
        ApiKind          kind;
    };

    [[nodiscard]] std::span<const ApiEntry> api_catalog();

    [[nodiscard]] std::span<const std::string_view> lua_standard_globals();

    [[nodiscard]] std::span<const std::string_view> lua_library_members();

    [[nodiscard]] const ApiEntry* find(std::string_view name);

    [[nodiscard]] std::vector<const ApiEntry*> members_of(std::string_view table);

    [[nodiscard]] std::vector<const ApiEntry*> matches(std::string_view receiver, std::string_view prefix);

    [[nodiscard]] std::size_t parameter_index(std::string_view signature, std::size_t typed);
}
