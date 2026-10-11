#pragma once

#include <expected>

#include "platform/linux/memory.hpp"
#include "process/types.hpp"

namespace slopkit::platform
{
    std::expected<void, MemoryError> suspend_process(process::ProcessId pid);

    std::expected<void, MemoryError> resume_process(process::ProcessId pid);

}
