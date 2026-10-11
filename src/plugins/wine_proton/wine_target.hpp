#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "process/types.hpp"

namespace slopkit::plugins::wine_proton
{

    // Everything the Linux side needs to reach one Wine/Proton target with the
    // resident agent: the wine binary and the environment to run the injector
    // with (the target's own, so Proton's wine and WINEPREFIX and never the
    // system ones), the guest PE's pointer size, and the target image as the
    // Windows path the injector matches on.
    struct WineTarget
    {
        process::ProcessId       pid {0};
        std::filesystem::path    exe_path;
        std::filesystem::path    wine_binary;
        std::uint32_t            pointer_size {0};
        std::vector<std::string> environment;
        std::string              image_path;
    };

    // Derives the target description from /proc/<pid>. A failure names what
    // could not be found, so the plugin can report it. The environment is the
    // target's own, with the entries that only carry process state stripped (see
    // injector_environment).
    [[nodiscard]] std::expected<WineTarget, std::string> derive_wine_target(process::ProcessId pid);

    // The target's environment without the entries that hand off an open file
    // descriptor rather than a value. Wine passes every process it starts an
    // already-connected wineserver socket as `WINESERVERSOCKET=<fd>`; that
    // descriptor belongs to the target's own table, so the injector this tool
    // forks does not have it and Wine's loader aborts with "Bad server socket
    // <fd> : Bad file descriptor" instead of loading the agent. Dropping the
    // entry lets the injector connect to the prefix's wineserver itself.
    [[nodiscard]] std::vector<std::string> injector_environment(const std::vector<std::string>& environment);

    // The Z: form of an absolute Linux path, the way Wine's default drive
    // mapping presents the host filesystem to the target.
    [[nodiscard]] std::string linux_path_to_windows(const std::string& path);

    // True when a directory created on the host at `path` is also reachable
    // through the target's own root, which is the view the target's loader and
    // its `Z:` drive resolve through. A Proton game runs inside a
    // pressure-vessel container that replaces `/run/user/<uid>` with a private
    // directory, so a path staged there is invisible to it even though this
    // process sees it.
    [[nodiscard]] bool target_can_reach(process::ProcessId pid, const std::filesystem::path& path);

    // The directory the agent binaries are staged in for one session with `pid`:
    // the first staging base that the target can actually reach, created if
    // necessary. Fails with the reason when none of them works.
    [[nodiscard]] std::expected<std::filesystem::path, std::string> make_staging_directory(process::ProcessId pid);

} // namespace slopkit::plugins::wine_proton
