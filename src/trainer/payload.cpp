#include "trainer/payload.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slopkit::trainer
{
    namespace
    {
        constexpr std::size_t kHeaderSize {28};

        void append_u8(std::vector<std::byte>& out, std::uint8_t value)
        {
            out.push_back(static_cast<std::byte>(value));
        }

        void append_u16(std::vector<std::byte>& out, std::uint16_t value)
        {
            append_u8(out, static_cast<std::uint8_t>(value & 0xFFu));
            append_u8(out, static_cast<std::uint8_t>((value >> 8) & 0xFFu));
        }

        void append_u32(std::vector<std::byte>& out, std::uint32_t value)
        {
            append_u16(out, static_cast<std::uint16_t>(value & 0xFFFFu));
            append_u16(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFFu));
        }

        void append_u64(std::vector<std::byte>& out, std::uint64_t value)
        {
            append_u32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
            append_u32(out, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFu));
        }

        void append_text(std::vector<std::byte>& out, std::string_view text)
        {
            const auto* data = reinterpret_cast<const std::byte*>(text.data());
            out.insert(out.end(), data, data + text.size());
        }

        std::uint8_t flags_of(const PayloadEntry& entry)
        {
            std::uint8_t flags = 0;
            if (entry.frozen)
            {
                flags |= kEntryFrozen;
            }
            if (entry.hex)
            {
                flags |= kEntryHex;
            }
            return flags;
        }

        // A read past the end of the payload leaves the cursor damaged, so every
        // accessor stays in range and the caller checks `ok` once per record.
        struct Cursor
        {
            std::span<const std::byte> bytes;
            std::size_t                offset {0};
            bool                       ok {true};

            [[nodiscard]] bool need(std::size_t count)
            {
                if (offset + count > bytes.size())
                {
                    ok = false;
                    return false;
                }
                return true;
            }
        };

        std::uint8_t read_u8(Cursor& cursor)
        {
            if (!cursor.need(1))
            {
                return 0;
            }
            return std::to_integer<std::uint8_t>(cursor.bytes[cursor.offset++]);
        }

        std::uint16_t read_u16(Cursor& cursor)
        {
            const auto low  = read_u8(cursor);
            const auto high = read_u8(cursor);
            return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high) << 8);
        }

        std::uint32_t read_u32(Cursor& cursor)
        {
            const auto low  = read_u16(cursor);
            const auto high = read_u16(cursor);
            return static_cast<std::uint32_t>(low) | static_cast<std::uint32_t>(high) << 16;
        }

        std::uint64_t read_u64(Cursor& cursor)
        {
            const auto low  = read_u32(cursor);
            const auto high = read_u32(cursor);
            return static_cast<std::uint64_t>(low) | static_cast<std::uint64_t>(high) << 32;
        }

        std::string read_text(Cursor& cursor, std::size_t count)
        {
            if (!cursor.need(count))
            {
                return {};
            }
            const auto* data = reinterpret_cast<const char*>(cursor.bytes.data() + cursor.offset);
            std::string text {data, count};
            cursor.offset += count;
            return text;
        }

        std::span<const std::byte> read_block(Cursor& cursor, std::size_t count)
        {
            if (!cursor.need(count))
            {
                return {};
            }
            const auto block = cursor.bytes.subspan(cursor.offset, count);
            cursor.offset += count;
            return block;
        }

        bool magic_matches(std::string_view text, std::string_view magic)
        {
            return text == magic;
        }

        std::expected<Payload, std::string> parse_payload(std::span<const std::byte> region)
        {
            Cursor cursor {region};

            const std::string magic = read_text(cursor, kPayloadMagic.size());
            if (!cursor.ok || !magic_matches(magic, kPayloadMagic))
            {
                return std::unexpected(std::string {"no trainer payload in this file"});
            }

            const auto version      = read_u32(cursor);
            const auto target_os    = read_u32(cursor);
            const auto pointer_size = read_u32(cursor);
            const auto entry_count  = read_u32(cursor);
            const auto name_length  = read_u32(cursor);
            if (!cursor.ok)
            {
                return std::unexpected(std::string {"the trainer payload is truncated"});
            }
            if (version != kPayloadVersion)
            {
                return std::unexpected(std::format("unsupported trainer payload version {}", version));
            }
            if (entry_count > region.size())
            {
                return std::unexpected(std::string {"the trainer payload is malformed"});
            }

            Payload payload;
            payload.target_os    = target_os;
            payload.pointer_size = pointer_size;
            payload.process_name = read_text(cursor, name_length);
            payload.entries.reserve(entry_count);

            for (std::uint32_t index = 0; index < entry_count; ++index)
            {
                const auto description_length = read_u16(cursor);
                const auto type               = read_u8(cursor);
                const auto flags              = read_u8(cursor);
                const auto bytes_length       = read_u32(cursor);
                const auto expression_length  = read_u16(cursor);
                const auto last_address       = read_u64(cursor);
                if (!cursor.ok)
                {
                    return std::unexpected(std::string {"the trainer payload is malformed"});
                }

                PayloadEntry entry;
                entry.type         = static_cast<scan::ValueType>(type);
                entry.frozen       = (flags & kEntryFrozen) != 0;
                entry.hex          = (flags & kEntryHex) != 0;
                entry.last_address = last_address;
                entry.description  = read_text(cursor, description_length);
                const auto bytes   = read_block(cursor, bytes_length);
                entry.bytes.assign(bytes.begin(), bytes.end());
                entry.expression = read_text(cursor, expression_length);
                if (!cursor.ok)
                {
                    return std::unexpected(std::string {"the trainer payload is malformed"});
                }
                payload.entries.push_back(std::move(entry));
            }

            return payload;
        }
    }

    std::string_view target_name(TargetOs target) noexcept
    {
        return target == TargetOs::windows ? "windows" : "linux";
    }

    std::vector<std::byte> encode_payload(const Payload& payload)
    {
        std::vector<std::byte> region;
        append_text(region, kPayloadMagic);
        append_u32(region, kPayloadVersion);
        append_u32(region, payload.target_os);
        append_u32(region, payload.pointer_size);
        append_u32(region, static_cast<std::uint32_t>(payload.entries.size()));
        append_u32(region, static_cast<std::uint32_t>(payload.process_name.size()));
        append_text(region, payload.process_name);

        for (const auto& entry : payload.entries)
        {
            append_u16(region, static_cast<std::uint16_t>(entry.description.size()));
            append_u8(region, static_cast<std::uint8_t>(entry.type));
            append_u8(region, flags_of(entry));
            append_u32(region, static_cast<std::uint32_t>(entry.bytes.size()));
            append_u16(region, static_cast<std::uint16_t>(entry.expression.size()));
            append_u64(region, entry.last_address);
            append_text(region, entry.description);
            region.insert(region.end(), entry.bytes.begin(), entry.bytes.end());
            append_text(region, entry.expression);
        }

        const auto size = static_cast<std::uint64_t>(region.size());
        append_text(region, kFingerMagic);
        append_u64(region, size);
        return region;
    }

    std::expected<Payload, std::string> decode_payload(std::span<const std::byte> image)
    {
        if (image.size() < kFingerSize + kHeaderSize)
        {
            return std::unexpected(std::string {"the file is too short to carry a trainer payload"});
        }

        const auto finger      = image.last(kFingerSize);
        const auto finger_mark = std::string_view {reinterpret_cast<const char*>(finger.data()), kFingerMagic.size()};
        if (!magic_matches(finger_mark, kFingerMagic))
        {
            return std::unexpected(std::string {"no trainer payload in this file (the fingerprint is missing)"});
        }

        Cursor     cursor {finger.subspan(kFingerMagic.size())};
        const auto size     = read_u64(cursor);
        const auto capacity = static_cast<std::uint64_t>(image.size() - kFingerSize);
        if (size < kHeaderSize || size > capacity)
        {
            return std::unexpected(std::string {"the trainer payload is truncated"});
        }

        const auto offset = static_cast<std::size_t>(capacity - size);
        return parse_payload(image.subspan(offset, static_cast<std::size_t>(size)));
    }

}
