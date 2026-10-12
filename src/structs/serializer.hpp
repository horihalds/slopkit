#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "structs/definition.hpp"

namespace slopkit::structs
{
    // Serialises one root struct and its subtree as the body of a `.skt`
    // `structs/<stem>.txt` member.
    [[nodiscard]] std::string to_text(const Structs& structs, std::uint64_t root);

    // Appends the root struct described by `text` to `structs` and returns its
    // id. Node ids are regenerated, so only the shape is preserved.
    [[nodiscard]] std::expected<std::uint64_t, std::string> from_text(std::string_view text, Structs& structs);

}
