#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "process/types.hpp"

namespace slopkit::scan
{

    // How the bytes at a candidate address are interpreted.
    enum class ValueType
    {
        byte,
        int16,
        int32,
        int64,
        float32,
        float64,
        string,
        byte_array,
        all,
    };

    // How a candidate is compared with the requested value.
    enum class ScanType
    {
        exact_value,
        bigger_than,
        smaller_than,
        value_between,
        increased,
        decreased,
        changed,
        unchanged,
        unknown_initial_value,
    };

    enum class ScanState
    {
        idle,
        running,
        done,
        failed,
        cancelled,
    };

    // The UI labels, in enum declaration order, so an enum value can index a
    // combo box's item array directly.
    inline constexpr const char* kValueTypeNames[] = {
        "Byte",
        "2 Bytes",
        "4 Bytes",
        "8 Bytes",
        "Float",
        "Double",
        "String",
        "Array of byte",
        "All",
    };

    inline constexpr const char* kScanTypeNames[] = {
        "Exact Value",
        "Bigger than",
        "Smaller than",
        "Value between",
        "Increased",
        "Decreased",
        "Changed",
        "Unchanged",
        "Unknown initial value",
    };

    // Highest canonical user-space address on x86-64. The `All memory` scan
    // range ends here; the target's mapped regions decide where a scan actually
    // stops, so this ceiling never turns into reads of unmapped memory.
    inline constexpr std::uint64_t kMaxUserAddress = 0x7FFFFFFFFFFF;

    // The region filter and fast-scan alignment applied to a scan. When none of
    // the attribute flags is set every readable region is scanned; setting any
    // of them keeps regions matching at least one checked attribute.
    struct RegionFilter
    {
        bool          writable {true};
        bool          executable {false};
        bool          copy_on_write {false};
        std::uint64_t start {};
        std::uint64_t stop {};
        std::uint64_t alignment {1};
    };

    // Width in bytes of a fixed-width value type, or 0 for the dynamically
    // sized string, byte-array and all types.
    [[nodiscard]] constexpr std::size_t value_size(ValueType type) noexcept
    {
        switch (type)
        {
        case ValueType::byte:
            return 1;
        case ValueType::int16:
            return 2;
        case ValueType::int32:
        case ValueType::float32:
            return 4;
        case ValueType::int64:
        case ValueType::float64:
            return 8;
        case ValueType::string:
        case ValueType::byte_array:
        case ValueType::all:
            return 0;
        }
        return 0;
    }

    [[nodiscard]] std::string_view describe(ValueType type) noexcept;
    [[nodiscard]] std::string_view describe(ScanType type) noexcept;
    [[nodiscard]] std::string_view describe(ScanState state) noexcept;

    // True for the refinement scan types that re-read existing candidates.
    [[nodiscard]] constexpr bool is_refinement(ScanType type) noexcept
    {
        return type == ScanType::increased || type == ScanType::decreased || type == ScanType::changed
            || type == ScanType::unchanged;
    }

    // True for the scan types that need a comparison value from the user.
    [[nodiscard]] constexpr bool needs_value(ScanType type) noexcept
    {
        return type == ScanType::exact_value || type == ScanType::bigger_than || type == ScanType::smaller_than;
    }

    // True for the numeric value types, i.e. those ordered by value.
    [[nodiscard]] constexpr bool is_numeric(ValueType type) noexcept
    {
        return type != ValueType::string && type != ValueType::byte_array;
    }

    // A private file mapping (shared == false with a path) is copy-on-write.
    [[nodiscard]] constexpr bool is_copy_on_write(const process::RegionInfo& region) noexcept
    {
        return !region.shared && !region.path.empty();
    }

} // namespace slopkit::scan
