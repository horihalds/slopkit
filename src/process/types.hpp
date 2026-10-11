#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::process
{
    using ProcessId = std::uint32_t;

    enum class ModuleKind
    {
        elf,
        pe,
        anonymous,
    };

    struct ProcessInfo
    {
        ProcessId                pid {};
        std::string              name;
        std::string              exe_path;
        std::string              plugin_id;
        std::vector<std::string> claimants;
    };

    struct ModuleInfo
    {
        std::uint64_t base {};
        std::uint64_t size {};
        std::uint64_t offset {};
        std::uint64_t entry {};
        ModuleKind    kind {ModuleKind::anonymous};
        bool          is_main {};
        std::string   name;
        std::string   path;
    };

    [[nodiscard]] const ModuleInfo* main_module(std::span<const ModuleInfo> modules) noexcept;

    [[nodiscard]] const ProcessInfo*
    match_process_by_name(std::span<const ProcessInfo> processes, std::string_view name, bool match_exe_path) noexcept;

    [[nodiscard]] const ProcessInfo* match_process_by_exe_path(std::span<const ProcessInfo> processes,
                                                               std::string_view             exe_path) noexcept;

    [[nodiscard]] constexpr bool is_file_backed(const ModuleInfo& module) noexcept
    {
        return module.kind != ModuleKind::anonymous && module.size != 0;
    }

    struct ThreadInfo
    {
        std::uint32_t tid {};
        std::string   name;
    };

    struct RegionInfo
    {
        std::uint64_t start {};
        std::uint64_t end {};
        std::uint64_t offset {};
        bool          readable {};
        bool          writable {};
        bool          executable {};
        bool          shared {};
        std::string   path;
    };

    enum class AccessMethod : std::uint32_t
    {
        none       = 0,
        procfs_mem = 1u << 0,
        process_vm = 1u << 1,
        ptrace     = 1u << 2,
        wineserver = 1u << 3,
        agent      = 1u << 4,
    };

    constexpr AccessMethod operator|(AccessMethod lhs, AccessMethod rhs) noexcept
    {
        return static_cast<AccessMethod>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
    }

    constexpr AccessMethod operator&(AccessMethod lhs, AccessMethod rhs) noexcept
    {
        return static_cast<AccessMethod>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
    }

    constexpr AccessMethod& operator|=(AccessMethod& lhs, AccessMethod rhs) noexcept
    {
        lhs = lhs | rhs;
        return lhs;
    }

    constexpr bool has_flag(AccessMethod value, AccessMethod flag) noexcept
    {
        return (value & flag) != AccessMethod::none;
    }

    enum class AccessError
    {
        permission_denied,
        not_found,
        unsupported,
        io_error,
        invalid_abi,
        invalid_argument,
        internal,
    };

    std::string_view describe(AccessError error) noexcept;
    std::string_view describe(ModuleKind kind) noexcept;
    std::string      describe(AccessMethod methods);

}
