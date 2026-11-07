#pragma once

#include <string>

#include "reclass/project.hpp"

namespace slopkit::reclass
{
    // The C++ header for every class and enum in `project`; pure, reads the
    // project only.
    [[nodiscard]] std::string cpp_header(const Project& project);

}
