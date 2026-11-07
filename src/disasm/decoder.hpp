#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace slopkit::disasm
{
    enum class MachineMode
    {
        long_64,
        legacy_32,
    };

    // The decode mode a target with this pointer size runs in. Zero, the
    // "unknown" value, falls back to the 64-bit mode.
    [[nodiscard]] constexpr MachineMode machine_mode_for(std::uint32_t pointer_size) noexcept
    {
        return pointer_size == 4 ? MachineMode::legacy_32 : MachineMode::long_64;
    }

    struct AddressRef
    {
        std::size_t   offset {};
        std::size_t   length {};
        std::uint64_t address {};
    };

    struct EncodingBytes
    {
        std::size_t offset {};
        std::size_t length {};
        bool        relative {};
    };

    struct MemoryRef
    {
        std::size_t  offset {};
        std::size_t  length {};
        std::string  base;
        std::string  index;
        std::uint8_t scale {1};
        std::int64_t displacement {};
        std::uint8_t width {};
        bool         rip_relative {};
        bool         writes {};
    };

    enum class TokenKind
    {
        cpu_register,
        immediate,
        displacement,
    };

    struct TokenSpan
    {
        std::size_t offset {};
        std::size_t length {};
        TokenKind   kind {};
    };

    struct Instruction
    {
        std::uint64_t              address {};
        std::size_t                length {};
        std::string                text;
        bool                       valid {};
        std::vector<AddressRef>    addresses;
        std::vector<MemoryRef>     memory;
        std::vector<TokenSpan>     tokens;
        std::vector<EncodingBytes> encoding_bytes;
    };

    [[nodiscard]] std::optional<Instruction>
    decode(std::span<const std::byte> code, std::uint64_t address, MachineMode mode = MachineMode::long_64);

    [[nodiscard]] std::vector<Instruction> decode_block(std::span<const std::byte> code,
                                                        std::uint64_t              base,
                                                        std::size_t                count,
                                                        MachineMode                mode = MachineMode::long_64);

}
