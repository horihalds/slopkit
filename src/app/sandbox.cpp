#include "app/sandbox.hpp"

#include <array>
#include <cstring>
#include <format>

#include <spawn.h>

#include "app/cli.hpp"

extern char** environ;

namespace slopkit::app
{
    namespace
    {
        std::filesystem::path sandbox_candidate(const std::filesystem::path& directory)
        {
            const std::filesystem::path base = directory.empty() ? executable_directory() : directory;
            return base / "slopkit-sandbox";
        }
    } // namespace

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

} // namespace slopkit::app
