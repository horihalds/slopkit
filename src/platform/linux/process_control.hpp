#pragma once

#include <expected>

#include "platform/linux/memory.hpp"
#include "process/types.hpp"

namespace slopkit::platform
{

    // Stops every thread of `pid` with SIGSTOP; its memory stays readable.
    std::expected<void, MemoryError> suspend_process(process::ProcessId pid);

    // Resumes a process stopped by suspend_process with SIGCONT.
    std::expected<void, MemoryError> resume_process(process::ProcessId pid);

} // namespace slopkit::platform
