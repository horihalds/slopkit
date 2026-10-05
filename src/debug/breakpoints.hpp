#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace slopkit::debug
{

    // A breakpoint kind as the user picks it. Software traps are int3 bytes;
    // the hardware kinds use the four ordinary x86-64 debug slots.
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

    // One session breakpoint. `slot` is a software slot id or a DR slot 0-3.
    struct Breakpoint
    {
        std::uint64_t id {};
        Kind          kind {Kind::software};
        std::size_t   size {1};
        std::string   expression;
        std::uint64_t address {};
        bool          enabled {true};
        // Set once the plugin has actually armed the breakpoint; a failed arming
        // leaves the entry in the list but unarmed.
        bool          armed {false};
        std::uint32_t slot {};
        std::uint64_t hits {};
    };

    // The session-only breakpoint list: slot allocation, duplicate refusal and
    // hit counting. Persistence is deliberately out of scope.
    class BreakpointTable
    {
    public:
        static constexpr std::uint32_t hardware_slots = 4;
        static constexpr std::uint32_t software_slots = 64;

        // Softwares are always one byte; an execute breakpoint is length 1; a
        // data breakpoint is 1, 2, 4 or 8 bytes.
        [[nodiscard]] static std::size_t normalize_size(Kind kind, std::size_t size) noexcept;
        [[nodiscard]] static bool        valid_size(Kind kind, std::size_t size) noexcept;

        // Adds a breakpoint, allocating a slot. Fails with a readable reason when
        // the address is already covered or no slot is free.
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

        // Counts one hit and returns the affected breakpoint, if it still exists.
        const Breakpoint* count_hit(std::uint64_t id);

    private:
        [[nodiscard]] std::optional<std::uint32_t> free_software_slot() const;
        [[nodiscard]] std::optional<std::uint32_t> free_hardware_slot() const;
        [[nodiscard]] bool                         covered(std::uint64_t address, Kind kind) const;

        std::vector<Breakpoint> entries_;
        std::uint64_t           next_id_ {1};
    };

} // namespace slopkit::debug
