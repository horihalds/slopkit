#pragma once

#include <cstdint>
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
        // Plugin that owns this entry by default (the most specific claimant).
        std::string              plugin_id;
        // Every plugin that claims the process, default first.
        std::vector<std::string> claimants;
    };

    struct ModuleInfo
    {
        std::uint64_t base {};
        std::uint64_t size {};
        std::uint64_t offset {};
        ModuleKind    kind {ModuleKind::anonymous};
        std::string   name;
        std::string   path;
    };

    struct ThreadInfo
    {
        std::uint32_t tid {};
        std::string   name;
    };

    // One mapped memory region of a target, mirroring slopkit_region_info in the
    // C ABI. `shared` false with a non-empty path marks a copy-on-write mapping.
    struct RegionInfo
    {
        std::uint64_t start {};
        std::uint64_t end {};
        std::uint64_t offset {};
        bool          readable {};
        bool          writable {};
        bool          executable {};
        bool          shared {};
        std::string   path; // Empty for anonymous mappings.
    };

    // Bit flags describing how a plugin can reach a target, mirroring
    // slopkit_access_method in the C ABI.
    enum class AccessMethod : std::uint32_t
    {
        none       = 0,
        procfs_mem = 1u << 0,
        process_vm = 1u << 1,
        ptrace     = 1u << 2,
        wineserver = 1u << 3,
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

} // namespace slopkit::process
