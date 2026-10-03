#include "platform/linux/module_entry.hpp"

#include <array>
#include <format>
#include <fstream>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::platform
{

    namespace
    {
        constexpr std::size_t kHeaderReadSize = 4096;

        std::uint8_t byte_at(std::span<const std::byte> bytes, std::size_t offset)
        {
            return std::to_integer<std::uint8_t>(bytes[offset]);
        }

        std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t offset, bool big_endian)
        {
            const auto first  = byte_at(bytes, offset);
            const auto second = byte_at(bytes, offset + 1);
            return big_endian ? static_cast<std::uint16_t>((first << 8) | second)
                              : static_cast<std::uint16_t>(first | (second << 8));
        }

        std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset, bool big_endian)
        {
            std::uint32_t value = 0;
            for (std::size_t i = 0; i < 4; ++i)
            {
                const auto index = big_endian ? 3 - i : i;
                value |= static_cast<std::uint32_t>(byte_at(bytes, offset + index)) << (8 * i);
            }
            return value;
        }

        std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset, bool big_endian)
        {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < 8; ++i)
            {
                const auto index = big_endian ? 7 - i : i;
                value |= static_cast<std::uint64_t>(byte_at(bytes, offset + index)) << (8 * i);
            }
            return value;
        }

        // A file's first page, read once and shared by the entry-point and
        // PE-kind readers.
        struct HeaderBytes
        {
            std::array<std::byte, kHeaderReadSize> data {};
            std::size_t                            size {};
        };

        std::optional<HeaderBytes> read_header(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                return std::nullopt;
            }

            HeaderBytes header;
            file.read(reinterpret_cast<char*>(header.data.data()), static_cast<std::streamsize>(header.data.size()));
            const auto count = file.gcount();
            if (count <= 0)
            {
                return std::nullopt;
            }
            header.size = static_cast<std::size_t>(count);
            return header;
        }

        std::optional<ImageEntry> parse_elf(std::span<const std::byte> bytes)
        {
            constexpr std::size_t   kTypeOffset   = 16;
            constexpr std::size_t   kEntryOffset  = 24;
            constexpr std::uint8_t  kElfClass32   = 1;
            constexpr std::uint8_t  kElfClass64   = 2;
            constexpr std::uint8_t  kLittleEndian = 1;
            constexpr std::uint8_t  kBigEndian    = 2;
            constexpr std::uint16_t kEtDyn        = 3;

            if (bytes.size() < kTypeOffset + 2)
            {
                return std::nullopt;
            }

            const auto elf_class = byte_at(bytes, 4);
            const auto encoding  = byte_at(bytes, 5);
            if (encoding != kLittleEndian && encoding != kBigEndian)
            {
                return std::nullopt;
            }
            const bool big_endian = encoding == kBigEndian;

            // ET_DYN entries are link-time addresses that need the load bias;
            // ET_EXEC entries are already absolute.
            ImageEntry entry;
            entry.relative = read_u16(bytes, kTypeOffset, big_endian) == kEtDyn;

            if (elf_class == kElfClass32)
            {
                if (bytes.size() < kEntryOffset + 4)
                {
                    return std::nullopt;
                }
                entry.address = read_u32(bytes, kEntryOffset, big_endian);
            }
            else if (elf_class == kElfClass64)
            {
                if (bytes.size() < kEntryOffset + 8)
                {
                    return std::nullopt;
                }
                entry.address = read_u64(bytes, kEntryOffset, big_endian);
            }
            else
            {
                return std::nullopt;
            }
            return entry;
        }

        std::optional<ImageEntry> parse_pe(std::span<const std::byte> bytes)
        {
            constexpr std::size_t kLfanewOffset   = 0x3c;
            constexpr std::size_t kSignatureSize  = 4;
            constexpr std::size_t kCoffHeaderSize = 20;
            constexpr std::size_t kEntryRvaOffset = 16;

            if (bytes.size() < kLfanewOffset + 4)
            {
                return std::nullopt;
            }
            const auto lfanew      = static_cast<std::size_t>(read_u32(bytes, kLfanewOffset, false));
            const auto header_size = kSignatureSize + kCoffHeaderSize + kEntryRvaOffset + 4;
            if (lfanew >= bytes.size() || bytes.size() - lfanew < header_size)
            {
                return std::nullopt;
            }
            if (byte_at(bytes, lfanew) != 'P' || byte_at(bytes, lfanew + 1) != 'E' || byte_at(bytes, lfanew + 2) != 0
                || byte_at(bytes, lfanew + 3) != 0)
            {
                return std::nullopt;
            }

            // A PE entry is an RVA from the module base, so it is always relative.
            const auto optional_header = lfanew + kSignatureSize + kCoffHeaderSize;
            ImageEntry entry;
            entry.address  = read_u32(bytes, optional_header + kEntryRvaOffset, false);
            entry.relative = true;
            return entry;
        }
    } // namespace

    std::optional<ImageEntry> parse_image_entry(std::span<const std::byte> bytes)
    {
        if (bytes.size() >= 4 && byte_at(bytes, 0) == 0x7f && byte_at(bytes, 1) == 'E' && byte_at(bytes, 2) == 'L'
            && byte_at(bytes, 3) == 'F')
        {
            return parse_elf(bytes);
        }
        if (bytes.size() >= 2 && byte_at(bytes, 0) == 'M' && byte_at(bytes, 1) == 'Z')
        {
            return parse_pe(bytes);
        }
        return std::nullopt;
    }

    std::optional<ImageEntry> read_image_entry(const std::filesystem::path& path)
    {
        const auto header = read_header(path);
        if (!header)
        {
            log::warning(log::category::process, std::format("cannot read the header of {}", path.string()));
            return std::nullopt;
        }
        auto entry = parse_image_entry(std::span<const std::byte>(header->data.data(), header->size));
        if (!entry)
        {
            log::warning(log::category::process, std::format("cannot parse the image header of {}", path.string()));
        }
        return entry;
    }

    std::optional<PeKind> parse_pe_kind(std::span<const std::byte> bytes)
    {
        constexpr std::size_t   kLfanewOffset       = 0x3c;
        constexpr std::size_t   kSignatureSize      = 4;
        constexpr std::size_t   kCoffHeaderSize     = 20;
        constexpr std::size_t   kCharacteristicsOff = 18;
        constexpr std::uint16_t kImageFileDll       = 0x2000;

        if (bytes.size() < 2 || byte_at(bytes, 0) != 'M' || byte_at(bytes, 1) != 'Z')
        {
            return std::nullopt;
        }
        if (bytes.size() < kLfanewOffset + 4)
        {
            return std::nullopt;
        }
        const auto lfanew = static_cast<std::size_t>(read_u32(bytes, kLfanewOffset, false));
        if (lfanew >= bytes.size() || bytes.size() - lfanew < kSignatureSize + kCoffHeaderSize)
        {
            return std::nullopt;
        }
        if (byte_at(bytes, lfanew) != 'P' || byte_at(bytes, lfanew + 1) != 'E' || byte_at(bytes, lfanew + 2) != 0
            || byte_at(bytes, lfanew + 3) != 0)
        {
            return std::nullopt;
        }

        const auto characteristics = read_u16(bytes, lfanew + kSignatureSize + kCharacteristicsOff, false);
        return (characteristics & kImageFileDll) != 0 ? PeKind::dll : PeKind::executable;
    }

    std::optional<PeKind> read_pe_kind(const std::filesystem::path& path)
    {
        const auto header = read_header(path);
        if (!header)
        {
            log::warning(log::category::process, std::format("cannot read the PE header of {}", path.string()));
            return std::nullopt;
        }
        auto kind = parse_pe_kind(std::span<const std::byte>(header->data.data(), header->size));
        if (!kind)
        {
            log::warning(log::category::process, std::format("cannot parse the PE header of {}", path.string()));
        }
        return kind;
    }

} // namespace slopkit::platform
