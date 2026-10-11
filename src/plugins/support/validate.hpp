#pragma once

#include <cstddef>
#include <cstdint>

#include "plugins/support/session.hpp"

namespace slopkit::plugins::support
{
    [[nodiscard]] bool range_is_readable(Session& session, std::uint64_t address, std::size_t size);

}
