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
        constexpr std::size_t kHeaderSize {32};

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
            const auto name_length  = read_u32(cursor);
            const auto item_count   = read_u32(cursor);
            const auto class_count  = read_u32(cursor);
            if (!cursor.ok)
            {
                return std::unexpected(std::string {"the trainer payload is truncated"});
            }
            if (version != kPayloadVersion)
            {
                return std::unexpected(std::format(
                    "unsupported trainer payload version {}; this trainer was generated by a different slopkit build "
                    "and must be regenerated",
                    version));
            }
            if (item_count > region.size() || class_count > region.size())
            {
                return std::unexpected(std::string {"the trainer payload is malformed"});
            }

            Payload payload;
            payload.target_os    = target_os;
            payload.pointer_size = pointer_size;
            payload.process_name = read_text(cursor, name_length);
            payload.items.reserve(item_count);

            // The number is the CLI's handle on a row, so a record whose number is
            // not its position within its own kind is refused instead of silently
            // addressing the wrong entry.
            std::uint32_t value_number  = 0;
            std::uint32_t script_number = 0;
            for (std::uint32_t index = 0; index < item_count; ++index)
            {
                const auto kind   = read_u8(cursor);
                const auto depth  = read_u8(cursor);
                const auto number = read_u32(cursor);
                if (!cursor.ok)
                {
                    return std::unexpected(std::string {"the trainer payload is malformed"});
                }

                PayloadItem item;
                item.depth  = depth;
                item.number = number;
                if (kind == static_cast<std::uint8_t>(ItemKind::value))
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

                    ++value_number;
                    if (number != value_number)
                    {
                        return std::unexpected(std::string {"the trainer payload is malformed"});
                    }
                    item.kind = ItemKind::value;
                    item.data = std::move(entry);
                }
                else if (kind == static_cast<std::uint8_t>(ItemKind::script))
                {
                    const auto description_length = read_u16(cursor);
                    const auto source_length      = read_u32(cursor);
                    if (!cursor.ok)
                    {
                        return std::unexpected(std::string {"the trainer payload is malformed"});
                    }

                    PayloadScript script;
                    script.description = read_text(cursor, description_length);
                    script.source      = read_text(cursor, source_length);
                    if (!cursor.ok)
                    {
                        return std::unexpected(std::string {"the trainer payload is malformed"});
                    }

                    ++script_number;
                    if (number != script_number)
                    {
                        return std::unexpected(std::string {"the trainer payload is malformed"});
                    }
                    item.kind = ItemKind::script;
                    item.data = std::move(script);
                }
                else
                {
                    return std::unexpected(std::string {"the trainer payload is malformed"});
                }
                payload.items.push_back(std::move(item));
            }

            payload.classes.reserve(class_count);
            for (std::uint32_t index = 0; index < class_count; ++index)
            {
                const auto name_length = read_u16(cursor);
                const auto text_length = read_u32(cursor);
                if (!cursor.ok)
                {
                    return std::unexpected(std::string {"the trainer payload is malformed"});
                }

                PayloadClass type;
                type.name = read_text(cursor, name_length);
                type.text = read_text(cursor, text_length);
                if (!cursor.ok)
                {
                    return std::unexpected(std::string {"the trainer payload is malformed"});
                }
                payload.classes.push_back(std::move(type));
            }

            return payload;
        }
    }

    std::string_view target_name(TargetOs target) noexcept
    {
        return target == TargetOs::windows ? "windows" : "linux";
    }

    std::size_t Payload::value_count() const
    {
        std::size_t count = 0;
        for (const auto& item : items)
        {
            count += item.kind == ItemKind::value ? 1 : 0;
        }
        return count;
    }

    std::size_t Payload::script_count() const
    {
        std::size_t count = 0;
        for (const auto& item : items)
        {
            count += item.kind == ItemKind::script ? 1 : 0;
        }
        return count;
    }

    std::vector<const PayloadEntry*> Payload::values() const
    {
        std::vector<const PayloadEntry*> records;
        records.reserve(value_count());
        for (const auto& item : items)
        {
            if (const auto* entry = std::get_if<PayloadEntry>(&item.data))
            {
                records.push_back(entry);
            }
        }
        return records;
    }

    std::vector<PayloadScript> Payload::script_records() const
    {
        std::vector<PayloadScript> records;
        records.reserve(script_count());
        for (const auto& item : items)
        {
            if (const auto* script = std::get_if<PayloadScript>(&item.data))
            {
                records.push_back(*script);
            }
        }
        return records;
    }

    std::vector<std::byte> encode_payload(const Payload& payload)
    {
        std::vector<std::byte> region;
        append_text(region, kPayloadMagic);
        append_u32(region, kPayloadVersion);
        append_u32(region, payload.target_os);
        append_u32(region, payload.pointer_size);
        append_u32(region, static_cast<std::uint32_t>(payload.process_name.size()));
        append_u32(region, static_cast<std::uint32_t>(payload.items.size()));
        append_u32(region, static_cast<std::uint32_t>(payload.classes.size()));
        append_text(region, payload.process_name);

        for (const auto& item : payload.items)
        {
            append_u8(region, static_cast<std::uint8_t>(item.kind));
            append_u8(region, item.depth);
            append_u32(region, item.number);
            if (item.kind == ItemKind::value)
            {
                const auto& entry = std::get<PayloadEntry>(item.data);
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
            else
            {
                const auto& script = std::get<PayloadScript>(item.data);
                append_u16(region, static_cast<std::uint16_t>(script.description.size()));
                append_u32(region, static_cast<std::uint32_t>(script.source.size()));
                append_text(region, script.description);
                append_text(region, script.source);
            }
        }

        for (const auto& type : payload.classes)
        {
            append_u16(region, static_cast<std::uint16_t>(type.name.size()));
            append_u32(region, static_cast<std::uint32_t>(type.text.size()));
            append_text(region, type.name);
            append_text(region, type.text);
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
