#pragma once

#include <filesystem>

namespace slopkit::trainer
{
    // Where `--verbose` writes: the trainer's own file name with `.log` appended,
    // beside the executable, so the trainer stays one movable file plus an
    // optional sidecar log.
    [[nodiscard]] std::filesystem::path verbose_log_path(const std::filesystem::path& executable);

    // Routes slopkit's logger at `debug` into that file and returns it. A trainer
    // installs no sink of its own otherwise, so nothing is written without this.
    [[nodiscard]] std::filesystem::path enable_verbose_logging(const std::filesystem::path& executable);
}
