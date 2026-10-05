#include "disasm/decoder.hpp"

#include <cctype>
#include <format>
#include <string_view>
#include <utility>

#include <Zydis/Decoder.h>
#include <Zydis/Formatter.h>
#include <Zydis/FormatterBuffer.h>
#include <Zydis/Status.h>
#include <Zydis/Utils.h>

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

        // Zydis reports register names lower-case; the listing and the debugger
        // both print them upper-case, so normalise here.
        [[nodiscard]] std::string register_name(ZydisRegister reg)
        {
            if (reg == ZYDIS_REGISTER_NONE)
            {
                return {};
            }

            const char* name = ZydisRegisterGetString(reg);
            if (name == nullptr)
            {
                return {};
            }

            std::string result(name);
            for (char& character : result)
            {
                character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            }
            return result;
        }

        // Every explicit memory operand of a decoded instruction, skipping the
        // address-generation forms (`lea`) that touch no memory. `text` is the
        // already-formatted instruction text the operand slices point into.
        [[nodiscard]] std::vector<MemoryRef> collect_memory(const ZydisDecodedInstruction& decoded,
                                                            const ZydisDecodedOperand*     operands,
                                                            std::string_view               text,
                                                            std::uint64_t                  address)
        {
            std::vector<MemoryRef> refs;
            std::size_t            search_from = 0;

            for (ZyanU8 i = 0; i < decoded.operand_count_visible; ++i)
            {
                const ZydisDecodedOperand& operand = operands[i];
                if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY || operand.mem.type == ZYDIS_MEMOP_TYPE_AGEN)
                {
                    continue;
                }

                MemoryRef ref;
                ref.base         = register_name(operand.mem.base);
                ref.index        = register_name(operand.mem.index);
                ref.scale        = ref.index.empty() ? 1 : operand.mem.scale;
                ref.displacement = operand.mem.disp.value;
                ref.width        = static_cast<std::uint8_t>(operand.size / 8);
                ref.rip_relative = operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP;
                ref.writes       = (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;

                // The slice the operand occupies in the printed text, so a caller
                // can show or copy the operand exactly as the listing prints it.
                char operand_buffer[128] {};
                if (ZYAN_SUCCESS(ZydisFormatterFormatOperand(
                        &formatter(), &decoded, &operand, operand_buffer, sizeof(operand_buffer), address, nullptr)))
                {
                    const std::string_view operand_text(operand_buffer);
                    if (const std::size_t found = text.find(operand_text, search_from); found != std::string_view::npos)
                    {
                        ref.offset  = found;
                        ref.length  = operand_text.size();
                        search_from = found + ref.length;
                    }
                }

                refs.push_back(std::move(ref));
            }

            return refs;
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
                    status, {address, 1, byte_text(code.front()), false, {}, {}},
                     false
                };
            }

            char                      buffer[256] {};
            ZydisFormatterTokenConst* token     = nullptr;
            const ZyanStatus          tokenized = ZydisFormatterTokenizeInstruction(&formatter(),
                                                                                    &decoded,
                                                                                    operands,
                                                                                    decoded.operand_count_visible,
                                                                                    buffer,
                                                                                    sizeof(buffer),
                                                                                    address,
                                                                                    &token,
                                                                                    nullptr);
            if (!ZYAN_SUCCESS(tokenized))
            {
                // The token stream is the only source of slice offsets; if it is
                // unavailable, fall back to the one-shot text with no slices.
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
                        formatted, {address, 1, byte_text(code.front()), false, {}, {}},
                         false
                    };
                }

                return {
                    ZYAN_STATUS_SUCCESS, {address, decoded.length, buffer, true, {}, {}},
                     true
                };
            }

            // Concatenating the token values reproduces the one-shot text, while
            // each absolute-address token marks a slice and pairs with the next
            // operand whose absolute address Zydis can calculate.
            std::string             text;
            std::vector<AddressRef> addresses;
            std::size_t             operand = 0;

            while (token != nullptr)
            {
                ZydisTokenType       type {};
                ZyanConstCharPointer value = nullptr;
                if (!ZYAN_SUCCESS(ZydisFormatterTokenGetValue(token, &type, &value)))
                {
                    break;
                }

                const std::string_view value_text = value != nullptr ? std::string_view(value) : std::string_view {};
                const std::size_t      offset     = text.size();
                text += value_text;

                if (type == ZYDIS_TOKEN_ADDRESS_ABS)
                {
                    while (operand < decoded.operand_count_visible)
                    {
                        std::uint64_t    target = 0;
                        const ZyanStatus calculated =
                            ZydisCalcAbsoluteAddress(&decoded, &operands[operand], address, &target);
                        ++operand;
                        if (ZYAN_SUCCESS(calculated))
                        {
                            addresses.push_back({offset, value_text.size(), target});
                            break;
                        }
                    }
                }

                if (!ZYAN_SUCCESS(ZydisFormatterTokenNext(&token)))
                {
                    break;
                }
            }

            // Written before the text is moved into the record, so the operand
            // slices still point into the string.
            std::vector<MemoryRef> memory = collect_memory(decoded, operands, text, address);

            return {
                ZYAN_STATUS_SUCCESS,
                {address, decoded.length, std::move(text), true, std::move(addresses), std::move(memory)},
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
