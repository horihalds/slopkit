#pragma once

#include <cstddef>
#include <cstdint>

#include "plugins/support/session.hpp"

namespace slopkit::plugins::support
{
    // True when [address, address + size) is mapped and readable in the
    // session's target. Reads in chunks of at most 64 KiB through the session's
    // cached MemAccess and stops at the first chunk that fails or comes back
    // short; every failure is simply "not valid". Never stops the target.
    [[nodiscard]] bool range_is_readable(Session& session, std::uint64_t address, std::size_t size);

} // namespace slopkit::plugins::support
