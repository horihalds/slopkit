#include "platform/linux/process_control.hpp"

#include <cerrno>

#include <csignal>
#include <sys/types.h>

namespace slopkit::platform
{
    namespace
    {
        MemoryError map_errno(int error)
        {
            switch (error)
            {
            case ESRCH:
                return MemoryError::process_not_found;
            case EPERM:
                return MemoryError::permission_denied;
            case EINVAL:
                return MemoryError::invalid_argument;
            default:
                return MemoryError::io_error;
            }
        }

        std::expected<void, MemoryError> signal_process(process::ProcessId pid, int sig)
        {
            if (::kill(static_cast<pid_t>(pid), sig) != 0)
            {
                return std::unexpected(map_errno(errno));
            }
            return {};
        }
    } // namespace

    std::expected<void, MemoryError> suspend_process(process::ProcessId pid)
    {
        return signal_process(pid, SIGSTOP);
    }

    std::expected<void, MemoryError> resume_process(process::ProcessId pid)
    {
        return signal_process(pid, SIGCONT);
    }

} // namespace slopkit::platform
