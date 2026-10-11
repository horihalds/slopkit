#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "debug/backend.hpp"

namespace slopkit::debug
{
    enum class Kind
    {
        software,
        hardware_execute,
        hardware_write,
        hardware_read_write,
    };

    [[nodiscard]] constexpr bool is_hardware(Kind kind) noexcept
    {
        return kind != Kind::software;
    }

    [[nodiscard]] constexpr HardwareKind hardware_kind(Kind kind) noexcept
    {
        switch (kind)
        {
        case Kind::hardware_write:
            return HardwareKind::write;
        case Kind::hardware_read_write:
            return HardwareKind::read_write;
        case Kind::hardware_execute:
        case Kind::software:
        default:
            return HardwareKind::execute;
        }
    }

    struct Breakpoint
    {
        std::uint64_t id {};
        Kind          kind {Kind::software};
        std::size_t   size {1};
        std::string   expression;
        std::uint64_t address {};
        bool          enabled {true};
        bool          armed {false};
        std::uint32_t slot {};
        std::uint64_t hits {};
        bool          hidden {false};
    };

    class BreakpointTable
    {
    public:
        static constexpr std::uint32_t hardware_slots = 4;
        static constexpr std::uint32_t software_slots = 64;

        [[nodiscard]] static std::size_t normalize_size(Kind kind, std::size_t size) noexcept;
        [[nodiscard]] static bool        valid_size(Kind kind, std::size_t size) noexcept;

        std::expected<std::uint64_t, std::string>
        add(std::string expression, std::uint64_t address, Kind kind, std::size_t size);

        bool remove(std::uint64_t id);
        void clear();

        [[nodiscard]] Breakpoint*                 find(std::uint64_t id);
        [[nodiscard]] const Breakpoint*           find(std::uint64_t id) const;
        [[nodiscard]] const Breakpoint*           software_at(std::uint64_t address) const;
        [[nodiscard]] const Breakpoint*           hardware_in_slot(std::uint32_t slot) const;
        [[nodiscard]] std::span<const Breakpoint> entries() const noexcept;
        [[nodiscard]] bool                        empty() const noexcept;

        const Breakpoint* count_hit(std::uint64_t id);

    private:
        [[nodiscard]] std::optional<std::uint32_t> free_software_slot() const;
        [[nodiscard]] std::optional<std::uint32_t> free_hardware_slot() const;
        [[nodiscard]] bool                         covered(std::uint64_t address, Kind kind) const;

        std::vector<Breakpoint> entries_;
        std::uint64_t           next_id_ {1};
    };

}
