#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace slopkit::platform
{
    struct ImageEntry
    {
        std::uint64_t address {0};
        bool          relative {false};
    };

    [[nodiscard]] std::optional<ImageEntry> parse_image_entry(std::span<const std::byte> bytes);

    [[nodiscard]] std::optional<ImageEntry> read_image_entry(const std::filesystem::path& path);

    enum class PeKind
    {
        dll,
        executable,
    };

    [[nodiscard]] std::optional<PeKind> parse_pe_kind(std::span<const std::byte> bytes);

    [[nodiscard]] std::optional<PeKind> read_pe_kind(const std::filesystem::path& path);

    [[nodiscard]] std::optional<std::uint16_t> parse_pe_machine(std::span<const std::byte> bytes);

    [[nodiscard]] std::optional<std::uint16_t> read_pe_machine(const std::filesystem::path& path);

}
