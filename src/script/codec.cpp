#include "script/codec.hpp"

#include <cstdint>
#include <cstring>
#include <format>

namespace slopkit::script
{
    namespace
    {
        enum class NumberKind
        {
            unsigned_integer,
            signed_integer,
            floating,
        };

        struct TokenInfo
        {
            std::string_view token;
            std::size_t      width;
            NumberKind       kind;
        };

        // The token vocabulary, mirroring `scan::value_size` for the widths:
        // `ptr` is the host pointer width, 8 on the only target this builds for.
        constexpr TokenInfo kTokens[] = {
            { "u8", 1, NumberKind::unsigned_integer},
            { "i8", 1,   NumberKind::signed_integer},
            {"u16", 2, NumberKind::unsigned_integer},
            {"i16", 2,   NumberKind::signed_integer},
            {"u32", 4, NumberKind::unsigned_integer},
            {"i32", 4,   NumberKind::signed_integer},
            {"u64", 8, NumberKind::unsigned_integer},
            {"i64", 8,   NumberKind::signed_integer},
            {"f32", 4,         NumberKind::floating},
            {"f64", 8,         NumberKind::floating},
            {"ptr", 8, NumberKind::unsigned_integer},
        };

        const TokenInfo* find_token(std::string_view token)
        {
            for (const TokenInfo& info : kTokens)
            {
                if (info.token == token)
                {
                    return &info;
                }
            }
            return nullptr;
        }

        std::uint64_t load_little_endian(std::span<const std::byte> bytes)
        {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[i])) << (8 * i);
            }
            return value;
        }

        std::vector<std::byte> store_little_endian(std::uint64_t value, std::size_t width)
        {
            std::vector<std::byte> bytes(width);
            for (std::size_t i = 0; i < width; ++i)
            {
                bytes[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
            }
            return bytes;
        }

        double decode_integer(const TokenInfo& info, std::uint64_t raw)
        {
            if (info.kind == NumberKind::unsigned_integer)
            {
                return static_cast<double>(raw);
            }
            switch (info.width)
            {
            case 1:
                return static_cast<double>(static_cast<std::int8_t>(static_cast<std::uint8_t>(raw)));
            case 2:
                return static_cast<double>(static_cast<std::int16_t>(static_cast<std::uint16_t>(raw)));
            case 4:
                return static_cast<double>(static_cast<std::int32_t>(static_cast<std::uint32_t>(raw)));
            default:
                return static_cast<double>(static_cast<std::int64_t>(raw));
            }
        }
    } // namespace

    std::optional<std::size_t> type_width(std::string_view token)
    {
        const TokenInfo* info = find_token(token);
        if (info == nullptr)
        {
            return std::nullopt;
        }
        return info->width;
    }

    bool is_integer_token(std::string_view token)
    {
        const TokenInfo* info = find_token(token);
        return info != nullptr && info->kind != NumberKind::floating;
    }

    std::expected<double, std::string> decode_number(std::string_view token, std::span<const std::byte> bytes)
    {
        const TokenInfo* info = find_token(token);
        if (info == nullptr)
        {
            return std::unexpected(std::format("unknown value type '{}'", token));
        }
        if (bytes.size() != info->width)
        {
            return std::unexpected(std::format("'{}' needs {} byte(s) but got {}", token, info->width, bytes.size()));
        }

        if (info->kind == NumberKind::floating)
        {
            if (info->width == 4)
            {
                float value = 0.0F;
                std::memcpy(&value, bytes.data(), sizeof(value));
                return static_cast<double>(value);
            }
            double value = 0.0;
            std::memcpy(&value, bytes.data(), sizeof(value));
            return value;
        }

        return decode_integer(*info, load_little_endian(bytes));
    }

    std::expected<std::vector<std::byte>, std::string> encode_number(std::string_view token, double value)
    {
        const TokenInfo* info = find_token(token);
        if (info == nullptr)
        {
            return std::unexpected(std::format("unknown value type '{}'", token));
        }

        if (info->kind == NumberKind::floating)
        {
            if (info->width == 4)
            {
                const float            narrowed = static_cast<float>(value);
                std::vector<std::byte> bytes(sizeof(narrowed));
                std::memcpy(bytes.data(), &narrowed, sizeof(narrowed));
                return bytes;
            }
            std::vector<std::byte> bytes(sizeof(value));
            std::memcpy(bytes.data(), &value, sizeof(value));
            return bytes;
        }

        // Truncates toward zero and keeps the low `width` bytes, which is the
        // two's-complement representation for both signed and unsigned tokens.
        return store_little_endian(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)), info->width);
    }

} // namespace slopkit::script
