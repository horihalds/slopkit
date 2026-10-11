#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "process/types.hpp"

namespace slopkit::plugins::wine_proton
{
    struct WineTarget
    {
        process::ProcessId       pid {0};
        std::filesystem::path    exe_path;
        std::filesystem::path    wine_binary;
        std::uint32_t            pointer_size {0};
        std::vector<std::string> environment;
        std::string              image_path;
    };

    // The guest executable's file name, extension included (`game.exe`), or
    // nothing when the process maps no executable PE image.
    [[nodiscard]] std::optional<std::string> guest_image_name(process::ProcessId pid);

    // The picker re-lists every two seconds, so a claimed pid's guest-image walk
    // is remembered instead of repeated per refresh; a name is therefore up to
    // the TTL stale, including after a pid is reused.
    class NameCache
    {
    public:
        // Entries kept at most; the soonest to expire is dropped first, so a long
        // session naming many short-lived wine processes cannot grow the map
        // without bound.
        static constexpr std::size_t kCapacity = 512;

        explicit NameCache(std::chrono::steady_clock::duration ttl = std::chrono::seconds(5));

        [[nodiscard]] std::optional<std::string> lookup(process::ProcessId                    pid,
                                                        std::chrono::steady_clock::time_point now);
        void remember(process::ProcessId pid, std::string name, std::chrono::steady_clock::time_point now);
        void forget(process::ProcessId pid);

    private:
        struct Entry
        {
            std::string                           name;
            std::chrono::steady_clock::time_point expires;
        };

        void sweep(std::chrono::steady_clock::time_point now);

        std::mutex                          mutex_;
        std::chrono::steady_clock::duration ttl_;
        std::map<process::ProcessId, Entry> entries_;
    };

    [[nodiscard]] std::expected<WineTarget, std::string> derive_wine_target(process::ProcessId pid);

    [[nodiscard]] std::vector<std::string> injector_environment(const std::vector<std::string>& environment);

    [[nodiscard]] std::string linux_path_to_windows(const std::string& path);

    [[nodiscard]] bool target_can_reach(process::ProcessId pid, const std::filesystem::path& path);

    [[nodiscard]] std::expected<std::filesystem::path, std::string> make_staging_directory(process::ProcessId pid);

}
