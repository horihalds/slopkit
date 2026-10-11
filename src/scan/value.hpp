#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "scan/types.hpp"

namespace slopkit::scan
{
    using ScanValue = std::variant<std::int64_t, double, std::string, std::vector<std::byte>>;

    struct ValueError
    {
        std::string message;
    };

    [[nodiscard]] std::expected<ScanValue, ValueError> parse_value(ValueType type, std::string_view text, bool hex);

    [[nodiscard]] std::expected<std::uint64_t, ValueError> parse_address(std::string_view text);

    [[nodiscard]] std::expected<std::uint64_t, ValueError> parse_alignment(std::string_view text);

    [[nodiscard]] std::string format_value(ValueType type, std::span<const std::byte> bytes, bool hex);

    [[nodiscard]] std::optional<std::string>
    convert_value_base(ValueType type, std::string_view text, bool from_hex, bool to_hex);

    [[nodiscard]] std::vector<std::byte> encode_value(ValueType type, const ScanValue& value);

    [[nodiscard]] bool matches(ScanType                   scan_type,
                               ValueType                  value_type,
                               std::span<const std::byte> candidate,
                               const ScanValue&           value,
                               const ScanValue&           value_upper,
                               bool                       hex);

    [[nodiscard]] bool matches_refinement(ScanType                   scan_type,
                                          ValueType                  value_type,
                                          std::span<const std::byte> previous,
                                          std::span<const std::byte> current);

    [[nodiscard]] std::size_t effective_size(ValueType type, const ScanValue& value) noexcept;

}
