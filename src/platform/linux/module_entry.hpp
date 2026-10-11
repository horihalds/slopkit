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

    // Whether a PE image is a DLL or a standalone executable (the COFF
    // IMAGE_FILE_DLL characteristic).
    enum class PeKind
    {
        dll,
        executable,
    };

    // Parses the PE kind from a mapped header. std::nullopt for a truncated or
    // non-PE buffer.
    [[nodiscard]] std::optional<PeKind> parse_pe_kind(std::span<const std::byte> bytes);

    // Reads the first page of a file and delegates to parse_pe_kind.
    [[nodiscard]] std::optional<PeKind> read_pe_kind(const std::filesystem::path& path);

    // The COFF Machine field of a PE image (0x8664 for x86_64, 0x014c for i386,
    // ...), or std::nullopt for a truncated or non-PE buffer. The wine-proton
    // plugin reads it to pick the agent build matching the target's bitness.
    [[nodiscard]] std::optional<std::uint16_t> parse_pe_machine(std::span<const std::byte> bytes);

    // Reads the first page of a file and delegates to parse_pe_machine.
    [[nodiscard]] std::optional<std::uint16_t> read_pe_machine(const std::filesystem::path& path);

} // namespace slopkit::platform
