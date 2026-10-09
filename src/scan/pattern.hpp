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

    // A byte pattern with `?` wildcards, compiled from text like "de ad ? ef".
    // The text is a run of tokens, whitespace between them optional, where each
    // token is exactly two hex digits or a one/two-character `?` wildcard.
    class BytePattern
    {
    public:
        // An empty text, a malformed token or a pattern with no concrete byte is
        // an error naming the offending token.
        [[nodiscard]] static std::expected<BytePattern, std::string> parse(std::string_view text);

        // Number of bytes the pattern covers.
        [[nodiscard]] std::size_t size() const noexcept;

        // The first offset >= `from` where the whole pattern matches, or nullopt.
        [[nodiscard]] std::optional<std::size_t> find(std::span<const std::byte> bytes, std::size_t from = 0) const;

    private:
        std::vector<std::byte> needle_;    // wildcard positions hold 0x00
        std::vector<bool>      mask_;      // true = this byte is compared
        std::size_t            anchor_ {}; // index of the first compared byte
    };

} // namespace slopkit::scan
