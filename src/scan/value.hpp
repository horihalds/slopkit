#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "scan/types.hpp"

namespace slopkit::scan
{

    // A parsed scan value. The active alternative follows the ValueType it was
    // parsed for: an integer, a real, a string or a byte sequence.
    using ScanValue = std::variant<std::int64_t, double, std::string, std::vector<std::byte>>;

    struct ValueError
    {
        std::string message;
    };

    // Parses `text` for `type`. `hex` selects base 16 for the integer and
    // byte-array types and is rejected for the real types.
    [[nodiscard]] std::expected<ScanValue, ValueError> parse_value(ValueType type, std::string_view text, bool hex);

    // Parses a decimal or `0x`-prefixed unsigned address.
    [[nodiscard]] std::expected<std::uint64_t, ValueError> parse_address(std::string_view text);

    // Parses a fast-scan alignment: a positive decimal or `0x` value.
    [[nodiscard]] std::expected<std::uint64_t, ValueError> parse_alignment(std::string_view text);

    // Renders `bytes` as the given type.
    [[nodiscard]] std::string format_value(ValueType type, std::span<const std::byte> bytes, bool hex);

    // Encodes a parsed value as its little-endian byte representation, using the
    // width of `type`; fixed-width types are truncated or zero-extended to fit.
    [[nodiscard]] std::vector<std::byte> encode_value(ValueType type, const ScanValue& value);

    // True when the candidate bytes satisfy the requested comparison.
    [[nodiscard]] bool matches(ScanType                   scan_type,
                               ValueType                  value_type,
                               std::span<const std::byte> candidate,
                               const ScanValue&           value,
                               const ScanValue&           value_upper,
                               bool                       hex);

    // True when `current` is increased/decreased/changed/unchanged relative to
    // `previous` for a refinement scan type.
    [[nodiscard]] bool matches_refinement(ScanType                   scan_type,
                                          ValueType                  value_type,
                                          std::span<const std::byte> previous,
                                          std::span<const std::byte> current);

    // The width the engine scans with. Fixed types use their size, string and
    // byte-array use the parsed value's length, and `all` is scanned as 4 bytes.
    [[nodiscard]] std::size_t effective_size(ValueType type, const ScanValue& value) noexcept;

} // namespace slopkit::scan
