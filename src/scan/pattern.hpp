#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::scan
{
    class BytePattern
    {
    public:
        [[nodiscard]] static std::expected<BytePattern, std::string> parse(std::string_view text);

        [[nodiscard]] std::size_t size() const noexcept;

        [[nodiscard]] std::optional<std::size_t> find(std::span<const std::byte> bytes, std::size_t from = 0) const;

    private:
        std::vector<std::byte> needle_;
        std::vector<bool>      mask_;
        std::size_t            anchor_ {};
    };

}
