#include "disasm/decoder.hpp"

#include <format>

#include <Zydis/Decoder.h>
#include <Zydis/Formatter.h>
#include <Zydis/Status.h>

namespace slopkit::disasm
{
    namespace
    {
        [[nodiscard]] const ZydisDecoder& decoder_for(MachineMode mode) noexcept
        {
            static const ZydisDecoder long_64 = []
            {
                ZydisDecoder decoder {};
                ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
                return decoder;
            }();

            static const ZydisDecoder legacy_32 = []
            {
                ZydisDecoder decoder {};
                ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
                return decoder;
            }();

            return mode == MachineMode::legacy_32 ? legacy_32 : long_64;
        }

        // One Intel-style formatter shared by every call. It is stateless per
        // format, so decoding stays on a single (the UI) thread.
        [[nodiscard]] const ZydisFormatter& formatter() noexcept
        {
            static const ZydisFormatter instance = []
            {
                ZydisFormatter formatter {};
                ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
                ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_UPPERCASE_MNEMONIC, ZYAN_TRUE);
                ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_UPPERCASE_REGISTERS, ZYAN_TRUE);
                ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_HEX_UPPERCASE, ZYAN_TRUE);
                ZydisFormatterSetProperty(
                    &formatter, ZYDIS_FORMATTER_PROP_HEX_PREFIX, reinterpret_cast<ZyanUPointer>("0x"));
                return formatter;
            }();
            return instance;
        }

        [[nodiscard]] std::string byte_text(std::byte value)
        {
            return std::format(".byte 0x{:02X}", static_cast<unsigned>(value));
        }

        // A decode attempt that keeps the raw Zydis status so a block sweep can
        // tell a truncated tail (stop) from an undefined opcode (`.byte` row).
        struct Attempt
        {
            ZyanStatus  status {};
            Instruction instruction;
            bool        decoded {};
        };

        [[nodiscard]] Attempt decode_at(std::span<const std::byte> code, std::uint64_t address, MachineMode mode)
        {
            const ZydisDecoder&     decoder = decoder_for(mode);
            ZydisDecodedInstruction decoded {};
            ZydisDecodedOperand     operands[ZYDIS_MAX_OPERAND_COUNT] {};

            const ZyanStatus status = ZydisDecoderDecodeFull(&decoder, code.data(), code.size(), &decoded, operands);
            if (!ZYAN_SUCCESS(status))
            {
                return {
                    status, {address, 1, byte_text(code.front()), false},
                     false
                };
            }

            char             buffer[256] {};
            const ZyanStatus formatted = ZydisFormatterFormatInstruction(&formatter(),
                                                                         &decoded,
                                                                         operands,
                                                                         decoded.operand_count_visible,
                                                                         buffer,
                                                                         sizeof(buffer),
                                                                         address,
                                                                         nullptr);
            if (!ZYAN_SUCCESS(formatted))
            {
                return {
                    formatted, {address, 1, byte_text(code.front()), false},
                     false
                };
            }

            return {
                ZYAN_STATUS_SUCCESS, {address, decoded.length, buffer, true},
                 true
            };
        }
    } // namespace

    std::optional<Instruction> decode(std::span<const std::byte> code, std::uint64_t address, MachineMode mode)
    {
        if (code.empty())
        {
            return std::nullopt;
        }

        return decode_at(code, address, mode).instruction;
    }

    std::vector<Instruction>
    decode_block(std::span<const std::byte> code, std::uint64_t base, std::size_t count, MachineMode mode)
    {
        std::vector<Instruction> instructions;
        std::size_t              offset = 0;

        while (offset < code.size() && instructions.size() < count)
        {
            const Attempt attempt = decode_at(code.subspan(offset), base + offset, mode);
            if (!attempt.decoded && attempt.status == ZYDIS_STATUS_NO_MORE_DATA)
            {
                break;
            }

            instructions.push_back(attempt.instruction);
            offset += instructions.back().length;
        }

        return instructions;
    }

} // namespace slopkit::disasm
