#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::script
{
    [[nodiscard]] std::optional<std::size_t> type_width(std::string_view token);

    [[nodiscard]] bool is_integer_token(std::string_view token);

    [[nodiscard]] std::expected<double, std::string> decode_number(std::string_view           token,
                                                                   std::span<const std::byte> bytes);

    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> encode_number(std::string_view token,
                                                                                   double           value);

    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> encode_integer(std::string_view token,
                                                                                    std::uint64_t    value);

}
