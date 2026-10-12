#pragma once

#include <cstddef>
#include <string>

#include "structs/definition.hpp"

namespace slopkit::structs
{
    // Renders the definitions as compilable C++ declarations: every root struct
    // is emitted in dependency order, gaps become explicit `pad_0x…` fields, and
    // each definition carries its size as a trailing comment. A struct whose
    // fields overlap is flagged with a comment rather than silently rendered.
    [[nodiscard]] std::string render_cpp(const Structs& structs, std::size_t pointer_size);

}
