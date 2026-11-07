#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "process/types.hpp"

namespace slopkit::scan
{
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
    };

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

    inline constexpr const char* kValueTypeNames[] = {
        "Byte",
        "2 Bytes",
        "4 Bytes",
        "8 Bytes",
        "Float",
        "Double",
        "String",
        "Array of byte",
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

    inline constexpr std::uint64_t kMaxUserAddress = 0x7FFFFFFFFFFF;

    struct RegionFilter
    {
        bool          writable {true};
        bool          executable {false};
        bool          copy_on_write {false};
        std::uint64_t start {};
        std::uint64_t stop {};
        std::uint64_t alignment {1};
    };

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
            return 0;
        }
        return 0;
    }

    [[nodiscard]] std::string_view describe(ValueType type) noexcept;
    [[nodiscard]] std::string_view describe(ScanType type) noexcept;
    [[nodiscard]] std::string_view describe(ScanState state) noexcept;

    [[nodiscard]] constexpr bool is_refinement(ScanType type) noexcept
    {
        return type == ScanType::increased || type == ScanType::decreased || type == ScanType::changed
            || type == ScanType::unchanged;
    }

    [[nodiscard]] constexpr bool needs_value(ScanType type) noexcept
    {
        return type == ScanType::exact_value || type == ScanType::bigger_than || type == ScanType::smaller_than;
    }

    [[nodiscard]] constexpr bool is_numeric(ValueType type) noexcept
    {
        return type != ValueType::string && type != ValueType::byte_array;
    }

    [[nodiscard]] constexpr bool is_copy_on_write(const process::RegionInfo& region) noexcept
    {
        return !region.shared && !region.path.empty();
    }

}
