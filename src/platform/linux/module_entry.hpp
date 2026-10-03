#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace slopkit::platform
{

    // The entry point declared by an executable image header. `relative` marks
    // an entry that is an offset from the module base (PIE and PE images)
    // rather than an absolute virtual address (non-PIE ELF executables).
    struct ImageEntry
    {
        std::uint64_t address {0};
        bool          relative {false};
    };

    // Parses an ELF (e_type/e_entry) or PE (e_lfanew -> AddressOfEntryPoint)
    // header. Returns std::nullopt for anything else or truncated input.
    [[nodiscard]] std::optional<ImageEntry> parse_image_entry(std::span<const std::byte> bytes);

    // Reads the first page of a file and delegates to parse_image_entry.
    [[nodiscard]] std::optional<ImageEntry> read_image_entry(const std::filesystem::path& path);

} // namespace slopkit::platform
