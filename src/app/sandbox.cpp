#include "app/sandbox.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <format>
#include <thread>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#include "app/cli.hpp"

extern char** environ;

namespace slopkit::app
{
    namespace
    {
        std::filesystem::path sandbox_candidate(const std::filesystem::path& directory)
        {
            const std::filesystem::path base = directory.empty() ? executable_directory() : directory;
            return base / kSandboxProcessName;
        }

        bool equals_ignore_case(std::string_view lhs, std::string_view rhs) noexcept
        {
            if (lhs.size() != rhs.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i)
            {
                const auto a = static_cast<unsigned char>(lhs[i]);
                const auto b = static_cast<unsigned char>(rhs[i]);
                if (std::tolower(a) != std::tolower(b))
                {
                    return false;
                }
            }
            return true;
        }
    }

    bool is_sandbox_identifier(std::string_view identifier) noexcept
    {
        const std::filesystem::path path {identifier};
        return equals_ignore_case(path.filename().string(), kSandboxProcessName);
    }

    std::optional<std::filesystem::path> sandbox_binary_path(const std::filesystem::path& directory)
    {
        const std::filesystem::path candidate = sandbox_candidate(directory);
        std::error_code             error;
        if (std::filesystem::is_regular_file(candidate, error))
        {
            return candidate;
        }
        return std::nullopt;
    }

    std::expected<std::int64_t, std::string> launch_sandbox(const std::filesystem::path& directory)
    {
        const std::filesystem::path candidate = sandbox_candidate(directory);
        if (!sandbox_binary_path(directory).has_value())
        {
            return std::unexpected(
                std::format("slopkit-sandbox was not found next to slopkit ({})", candidate.string()));
        }

        std::array<char*, 2> argv {const_cast<char*>(candidate.c_str()), nullptr};

        posix_spawnattr_t attributes {};
        if (::posix_spawnattr_init(&attributes) != 0)
        {
            return std::unexpected(std::format("slopkit-sandbox could not be started ({})", candidate.string()));
        }
        // An own session keeps the target alive after slopkit and after a terminal
        // hangup; the parent link stays, because the kernel only lets slopkit read
        // a target it is an ancestor of (Yama ptrace_scope=1).
        ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);

        pid_t     pid  = 0;
        const int code = ::posix_spawn(&pid, candidate.c_str(), nullptr, &attributes, argv.data(), environ);
        ::posix_spawnattr_destroy(&attributes);
        if (code != 0)
        {
            return std::unexpected(
                std::format("slopkit-sandbox could not be started ({}): {}", candidate.string(), std::strerror(code)));
        }
        return static_cast<std::int64_t>(pid);
    }

    std::expected<void, std::string> terminate_sandbox(std::int64_t pid)
    {
        if (pid <= 0)
        {
            return std::unexpected(std::format("invalid practice target pid {}", pid));
        }

        const auto child = static_cast<pid_t>(pid);
        if (::kill(child, SIGTERM) != 0 && errno != ESRCH)
        {
            return std::unexpected(
                std::format("practice target (pid {}) could not be stopped: {}", pid, std::strerror(errno)));
        }

        // The sandbox is a GUI process, so give it a short window to close itself
        // before forcing the issue, and reap the child either way.
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            int         status = 0;
            const pid_t reaped = ::waitpid(child, &status, WNOHANG);
            if (reaped == child || (reaped < 0 && (errno == ECHILD || errno == ESRCH)))
            {
                return {};
            }
            if (reaped < 0 && errno != EINTR)
            {
                return std::unexpected(
                    std::format("practice target (pid {}) could not be reaped: {}", pid, std::strerror(errno)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (::kill(child, SIGKILL) != 0 && errno != ESRCH)
        {
            return std::unexpected(
                std::format("practice target (pid {}) could not be killed: {}", pid, std::strerror(errno)));
        }
        while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR)
        {
        }
        return {};
    }

}
