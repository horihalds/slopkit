#pragma once

#include <string_view>

#include <QColor>

#include "reclass/node.hpp"

namespace slopkit::ui
{
    // The file stem of the kind's glyph in assets/icons/datatypes/, empty when
    // the folder has no glyph for it.
    [[nodiscard]] std::string_view datatype_icon_name(reclass::NodeKind kind) noexcept;

    // The colour a datatype is spelled in: its glyph and its Type-column name.
    [[nodiscard]] QColor datatype_colour(reclass::NodeKind kind);
}
