#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "platform/linux/debug_session.hpp"
#include "platform/linux/procfs.hpp"
#include "plugin/plugin_api.h"
#include "plugins/support/allocate.hpp"
#include "plugins/support/memory.hpp"
#include "process/types.hpp"

namespace slopkit::plugins::support
{
    struct PluginProfile
    {
        const slopkit_plugin_info* (*info)() noexcept;
        std::int32_t (*precedence)() noexcept;
        std::uint32_t (*access_methods)() noexcept;
        bool (*claim)(process::ProcessId pid, std::string& out_name);
        void (*shape_modules)(std::vector<process::ModuleInfo>& modules, process::ProcessId pid);
        std::expected<std::vector<process::ModuleInfo>, AccessFault> (*modules)(Session& session);
        platform::ForeignSignalPolicy signal_policy;
        std::expected<std::uint64_t, AllocationError> (*allocate)(Session&      session,
                                                                  std::uint64_t size,
                                                                  std::uint64_t near_address);
        std::expected<void, AllocationError> (*free)(Session& session, std::uint64_t address);
        void (*close_session)(Session& session);
        std::expected<std::vector<platform::MappedRegion>, AccessFault> (*regions)(Session& session);
        std::expected<bool, AccessFault> (*range_is_readable)(Session&      session,
                                                              std::uint64_t address,
                                                              std::size_t   size);
        std::expected<MemoryResult, AccessFault> (*read)(Session&             session,
                                                         std::uint64_t        address,
                                                         std::span<std::byte> buffer);
        std::expected<MemoryResult, AccessFault> (*write)(Session&                   session,
                                                          std::uint64_t              address,
                                                          std::span<const std::byte> data);
        std::vector<process::ProcessId> (*debug_excluded_threads)(Session& session);
    };

    [[nodiscard]] const slopkit_plugin_vtable* install(const slopkit_host_services* host,
                                                       const PluginProfile*         profile) noexcept;

    template<const PluginProfile& Profile>
    [[nodiscard]] const slopkit_plugin_vtable* entry(const slopkit_host_services* host) noexcept
    {
        return install(host, &Profile);
    }

}
