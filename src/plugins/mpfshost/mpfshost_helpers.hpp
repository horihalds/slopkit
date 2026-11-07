#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace slopkit::plugins::mpfshost
{
    struct GuestTarget
    {
        std::string   target;
        std::uint32_t pid {};
        std::string   name;
        bool          wow64 {};
    };

    class TargetTable
    {
    public:
        [[nodiscard]] std::uint32_t              remember(GuestTarget process);
        [[nodiscard]] std::optional<GuestTarget> lookup(std::uint32_t synthetic_pid) const;

    private:
        mutable std::mutex                             mutex_;
        std::unordered_map<std::uint32_t, GuestTarget> by_pid_;
        std::unordered_map<std::string, std::uint32_t> by_key_;
        std::uint32_t                                  next_ {0x80000000u};
    };

    struct RegionProtection
    {
        bool readable {};
        bool writable {};
        bool executable {};
    };

    [[nodiscard]] RegionProtection classify_pte_flags(std::uint64_t flags) noexcept;
    [[nodiscard]] bool             is_main_module(std::string_view module_name, std::string_view process_name) noexcept;
    [[nodiscard]] std::int32_t     status_for_client_error(std::string_view error) noexcept;
    [[nodiscard]] std::string      socket_path();
}
