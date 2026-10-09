#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::script
{

    // The value type token vocabulary a script names in `mem.read`/`mem.write`:
    // `u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 ptr`. The widths mirror
    // `scan::value_size`; `ptr` is the host pointer width.

    // Width in bytes of a token, or `std::nullopt` for an unknown token.
    [[nodiscard]] std::optional<std::size_t> type_width(std::string_view token);

    // True for the integer tokens (`u8 i8 u16 i16 u32 i32 u64 i64 ptr`), false
    // for the floating ones. `mem.read` uses it to hand Lua an integer rather
    // than a float, so a value prints and renders without a trailing `.0`.
    [[nodiscard]] bool is_integer_token(std::string_view token);

    // Interprets exactly `type_width(token)` bytes from the target as the token's
    // type and returns the numeric value. Unknown tokens and a wrong byte count
    // are rejected.
    [[nodiscard]] std::expected<double, std::string> decode_number(std::string_view           token,
                                                                   std::span<const std::byte> bytes);

    // Encodes a Lua number as the token's type, little-endian. Unknown tokens
    // are rejected; integers truncate toward zero.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> encode_number(std::string_view token,
                                                                                   double           value);

} // namespace slopkit::script
