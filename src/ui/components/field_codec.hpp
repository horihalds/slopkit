#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <vector>

#include "structs/definition.hpp"
#include "structs/types.hpp"
#include "ui/address_format.hpp"
#include "ui/text.hpp"

#include <QString>
#include <QStringList>

namespace slopkit::ui::components
{
    // Little-endian decode/encode for a single struct field, shared by the
    // document (live reads and value writes) and the table model's formatting.
    namespace field_codec
    {
        [[nodiscard]] inline std::uint64_t read_le(std::span<const std::byte> bytes, std::size_t width) noexcept
        {
            std::uint64_t value = 0;
            for (std::size_t index = 0; index < width && index < bytes.size(); ++index)
            {
                value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[index])) << (8 * index);
            }
            return value;
        }

        [[nodiscard]] inline std::int64_t sign_extend(std::uint64_t raw, std::size_t width) noexcept
        {
            if (width >= 8)
            {
                return static_cast<std::int64_t>(raw);
            }
            const auto          bits = static_cast<unsigned>(width * 8);
            const std::uint64_t sign = std::uint64_t {1} << (bits - 1);
            if ((raw & sign) == 0)
            {
                return static_cast<std::int64_t>(raw);
            }
            return static_cast<std::int64_t>(raw | ~((std::uint64_t {1} << bits) - 1));
        }

        [[nodiscard]] inline std::vector<std::byte> le_bytes(std::uint64_t value, std::size_t width)
        {
            std::vector<std::byte> out(width, std::byte {0});
            for (std::size_t index = 0; index < width; ++index)
            {
                out[index] = static_cast<std::byte>((value >> (8 * index)) & 0xFF);
            }
            return out;
        }

        [[nodiscard]] inline QString hex_text(std::uint64_t value, std::size_t width)
        {
            return QStringLiteral("0x")
                 + QString::number(value, 16).rightJustified(static_cast<int>(width) * 2, QLatin1Char('0')).toUpper();
        }

        [[nodiscard]] inline std::optional<std::uint64_t> parse_integer(const QString& text, bool hex)
        {
            const QString trimmed = text.trimmed();
            if (trimmed.isEmpty())
            {
                return std::nullopt;
            }
            bool ok = false;
            if (trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            {
                const qulonglong value = trimmed.mid(2).toULongLong(&ok, 16);
                return ok ? std::optional<std::uint64_t> {value} : std::nullopt;
            }
            if (trimmed.startsWith(QLatin1Char('-')))
            {
                const qlonglong value = trimmed.toLongLong(&ok, 10);
                return ok ? std::optional<std::uint64_t> {static_cast<std::uint64_t>(value)} : std::nullopt;
            }
            const qulonglong value = trimmed.toULongLong(&ok, hex ? 16 : 10);
            return ok ? std::optional<std::uint64_t> {value} : std::nullopt;
        }

        [[nodiscard]] inline QString decode(const structs::StructNode& node,
                                            std::span<const std::byte> bytes,
                                            std::size_t                pointer_size,
                                            ui::AddressMode            address_mode,
                                            const ui::ModuleSpans&     module_spans)
        {
            const structs::FieldType& type = node.type;
            switch (type.kind)
            {
            case structs::FieldKind::integer:
            {
                const std::uint64_t raw = read_le(bytes, type.width);
                if (type.is_signed)
                {
                    return QString::number(sign_extend(raw, type.width));
                }
                return node.hex ? hex_text(raw, type.width) : QString::number(raw);
            }
            case structs::FieldKind::floating:
            {
                if (type.width == 4 && bytes.size() >= 4)
                {
                    float value = 0;
                    std::memcpy(&value, bytes.data(), 4);
                    return to_qstring(std::format("{}", value));
                }
                if (type.width == 8 && bytes.size() >= 8)
                {
                    double value = 0;
                    std::memcpy(&value, bytes.data(), 8);
                    return to_qstring(std::format("{}", value));
                }
                return QStringLiteral("?");
            }
            case structs::FieldKind::pointer:
            {
                const std::uint64_t value = read_le(bytes, pointer_size);
                return value == 0 ? QStringLiteral("0x0") : format_cell_address(address_mode, module_spans, value);
            }
            case structs::FieldKind::array:
            {
                QStringList parts;
                for (std::size_t index = 0; index < type.count; ++index)
                {
                    const auto          start = std::min(index * type.width, bytes.size());
                    const std::uint64_t raw   = read_le(bytes.subspan(start), type.width);
                    if (type.is_signed)
                    {
                        parts << QString::number(sign_extend(raw, type.width));
                    }
                    else
                    {
                        parts << (node.hex ? hex_text(raw, type.width) : QString::number(raw));
                    }
                }
                return QStringLiteral("[%1]").arg(parts.join(QStringLiteral(", ")));
            }
            case structs::FieldKind::text:
            {
                if (type.wide)
                {
                    std::vector<char16_t> characters;
                    const std::size_t     limit = std::min(type.count, bytes.size() / 2);
                    for (std::size_t index = 0; index < limit; ++index)
                    {
                        const auto code = static_cast<char16_t>(read_le(bytes.subspan(index * 2), 2));
                        if (code == u'\0')
                        {
                            break;
                        }
                        characters.push_back(code);
                    }
                    return QString::fromUtf16(characters.data(), static_cast<int>(characters.size()));
                }
                const std::size_t limit  = std::min(type.count, bytes.size());
                std::size_t       length = 0;
                while (length < limit && bytes[length] != std::byte {0})
                {
                    ++length;
                }
                return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(length));
            }
            case structs::FieldKind::structure:
                return {};
            }
            return {};
        }

        [[nodiscard]] inline std::optional<std::vector<std::byte>>
        encode(const structs::StructNode& node, const QString& text, std::size_t pointer_size)
        {
            const structs::FieldType& type    = node.type;
            const QString             trimmed = text.trimmed();
            if (trimmed.isEmpty())
            {
                return std::nullopt;
            }
            switch (type.kind)
            {
            case structs::FieldKind::integer:
            {
                const auto value = parse_integer(trimmed, node.hex);
                return value.has_value() ? std::optional<std::vector<std::byte>> {le_bytes(*value, type.width)}
                                         : std::nullopt;
            }
            case structs::FieldKind::pointer:
            {
                const auto value = parse_integer(trimmed, true);
                return value.has_value() ? std::optional<std::vector<std::byte>> {le_bytes(*value, pointer_size)}
                                         : std::nullopt;
            }
            case structs::FieldKind::floating:
            {
                bool         ok    = false;
                const double value = trimmed.toDouble(&ok);
                if (!ok)
                {
                    return std::nullopt;
                }
                if (type.width == 4)
                {
                    const float            single = static_cast<float>(value);
                    std::vector<std::byte> bytes(4);
                    std::memcpy(bytes.data(), &single, 4);
                    return bytes;
                }
                if (type.width == 8)
                {
                    std::vector<std::byte> bytes(8);
                    std::memcpy(bytes.data(), &value, 8);
                    return bytes;
                }
                return std::nullopt;
            }
            case structs::FieldKind::text:
            {
                std::vector<std::byte> bytes(type.count * (type.wide ? 2 : 1), std::byte {0});
                if (type.wide)
                {
                    const QString utf16 = trimmed;
                    const auto    limit = std::min(type.count, static_cast<std::size_t>(utf16.size()));
                    for (std::size_t index = 0; index < limit; ++index)
                    {
                        const auto code      = static_cast<char16_t>(utf16[static_cast<int>(index)].unicode());
                        bytes[index * 2]     = static_cast<std::byte>(code & 0xFF);
                        bytes[index * 2 + 1] = static_cast<std::byte>((code >> 8) & 0xFF);
                    }
                    return bytes;
                }
                const QByteArray utf8  = trimmed.toUtf8();
                const auto       limit = std::min(type.count, static_cast<std::size_t>(utf8.size()));
                for (std::size_t index = 0; index < limit; ++index)
                {
                    bytes[index] = static_cast<std::byte>(utf8[static_cast<int>(index)]);
                }
                return bytes;
            }
            case structs::FieldKind::array:
            case structs::FieldKind::structure:
                return std::nullopt;
            }
            return std::nullopt;
        }
    }

}
