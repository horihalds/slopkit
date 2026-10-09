#include "disasm/assembler.hpp"
#include "expr/expression.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <Zydis/Encoder.h>
#include <Zydis/Mnemonic.h>
#include <Zydis/Register.h>
#include <Zydis/Status.h>

namespace slopkit::disasm
{
    namespace
    {
        [[nodiscard]] bool is_space(char character) noexcept
        {
            return character == ' ' || character == '\t' || character == '\r' || character == '\n';
        }

        [[nodiscard]] std::string_view trim(std::string_view text) noexcept
        {
            while (!text.empty() && is_space(text.front()))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && is_space(text.back()))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        [[nodiscard]] std::string lower(std::string_view text)
        {
            std::string result(text);
            for (char& character : result)
            {
                if (character >= 'A' && character <= 'Z')
                {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            return result;
        }

        [[nodiscard]] const std::unordered_map<std::string, ZydisMnemonic>& mnemonic_table()
        {
            static const std::unordered_map<std::string, ZydisMnemonic> table = []
            {
                std::unordered_map<std::string, ZydisMnemonic> built;
                for (int value = 0; value <= ZYDIS_MNEMONIC_MAX_VALUE; ++value)
                {
                    const auto mnemonic = static_cast<ZydisMnemonic>(value);
                    if (const char* name = ZydisMnemonicGetString(mnemonic); name != nullptr)
                    {
                        built.try_emplace(lower(name), mnemonic);
                    }
                }
                return built;
            }();
            return table;
        }

        [[nodiscard]] const std::unordered_map<std::string, ZydisRegister>& register_table()
        {
            static const std::unordered_map<std::string, ZydisRegister> table = []
            {
                std::unordered_map<std::string, ZydisRegister> built;
                for (int value = 0; value <= ZYDIS_REGISTER_MAX_VALUE; ++value)
                {
                    const auto reg = static_cast<ZydisRegister>(value);
                    if (const char* name = ZydisRegisterGetString(reg); name != nullptr)
                    {
                        built.try_emplace(lower(name), reg);
                    }
                }
                return built;
            }();
            return table;
        }

        [[nodiscard]] std::optional<ZydisRegister> find_register(std::string_view name)
        {
            const auto found = register_table().find(lower(trim(name)));
            return found == register_table().end() ? std::nullopt : std::optional<ZydisRegister>(found->second);
        }

        // One number the way an instruction prints it: bare hex, `0x…`
        // accepted, `#…` decimal; optionally signed.
        [[nodiscard]] std::optional<std::uint64_t> parse_number(std::string_view text)
        {
            text = trim(text);
            if (text.empty())
            {
                return std::nullopt;
            }

            bool negative = false;
            if (text.front() == '+')
            {
                text.remove_prefix(1);
            }
            else if (text.front() == '-')
            {
                negative = true;
                text.remove_prefix(1);
            }
            if (text.empty())
            {
                return std::nullopt;
            }

            const auto value = expr::parse_literal(text);
            if (!value)
            {
                return std::nullopt;
            }
            return negative ? static_cast<std::uint64_t>(0) - *value : *value;
        }

        // The memory operand sizes the listing prints (`qword ptr`, `xmmword`).
        [[nodiscard]] std::optional<unsigned char> size_keyword(std::string_view text)
        {
            const std::string name = lower(trim(text));
            if (name.empty())
            {
                return std::nullopt;
            }
            if (name == "byte")
            {
                return 1;
            }
            if (name == "word")
            {
                return 2;
            }
            if (name == "dword")
            {
                return 4;
            }
            if (name == "fword")
            {
                return 6;
            }
            if (name == "qword")
            {
                return 8;
            }
            if (name == "tbyte")
            {
                return 10;
            }
            if (name == "oword" || name == "xmmword")
            {
                return 16;
            }
            if (name == "ymmword")
            {
                return 32;
            }
            if (name == "zmmword")
            {
                return 64;
            }
            return std::nullopt;
        }

        struct Operand
        {
            ZydisOperandType type {};
            ZydisRegister    reg {};
            std::uint64_t    imm {};
            ZydisRegister    base {};
            ZydisRegister    index {};
            unsigned char    scale {};
            std::int64_t     displacement {};
            unsigned char    size {}; // memory access size, 0 when the text does not say
        };

        // The `[...]` expression: `RBP-04`, `RAX+RCX*4+10`, `22FE`.
        [[nodiscard]] std::expected<Operand, std::string> parse_memory(std::string_view expression)
        {
            Operand operand;
            operand.type  = ZYDIS_OPERAND_TYPE_MEMORY;
            operand.base  = ZYDIS_REGISTER_NONE;
            operand.index = ZYDIS_REGISTER_NONE;

            std::string_view rest = trim(expression);
            while (!rest.empty())
            {
                bool negative = false;
                while (!rest.empty() && (rest.front() == '+' || rest.front() == '-'))
                {
                    negative = negative != (rest.front() == '-');
                    rest.remove_prefix(1);
                }

                const std::size_t separator = rest.find_first_of("+-");
                std::string_view  term      = trim(rest.substr(0, separator));
                rest = separator == std::string_view::npos ? std::string_view {} : rest.substr(separator);
                if (term.empty())
                {
                    return std::unexpected(std::format("malformed memory operand '[{}]'", expression));
                }

                if (const std::size_t star = term.find('*'); star != std::string_view::npos)
                {
                    const auto index = find_register(term.substr(0, star));
                    const auto scale = parse_number(term.substr(star + 1));
                    if (!index.has_value() || !scale.has_value()
                        || (*scale != 1 && *scale != 2 && *scale != 4 && *scale != 8)
                        || operand.index != ZYDIS_REGISTER_NONE)
                    {
                        return std::unexpected(std::format("unsupported scaled index '{}'", term));
                    }
                    operand.index = *index;
                    operand.scale = static_cast<unsigned char>(*scale);
                    continue;
                }

                if (const auto reg = find_register(term); reg.has_value())
                {
                    if (operand.base == ZYDIS_REGISTER_NONE)
                    {
                        operand.base = *reg;
                    }
                    else if (operand.index == ZYDIS_REGISTER_NONE)
                    {
                        operand.index = *reg;
                        operand.scale = 1;
                    }
                    else
                    {
                        return std::unexpected(std::format("too many registers in '[{}]'", expression));
                    }
                    continue;
                }

                const auto value = parse_number(term);
                if (!value.has_value())
                {
                    return std::unexpected(std::format("unrecognised term '{}' in '[{}]'", term, expression));
                }
                const std::uint64_t signed_value = negative ? static_cast<std::uint64_t>(0) - *value : *value;
                operand.displacement += static_cast<std::int64_t>(signed_value);
            }

            // The encoder wants no scale at all when there is no index, and at
            // least one when there is.
            operand.scale = operand.index == ZYDIS_REGISTER_NONE ? 0 : std::max<unsigned char>(operand.scale, 1);
            return operand;
        }

        [[nodiscard]] std::expected<Operand, std::string> parse_operand(std::string_view text)
        {
            text = trim(text);
            if (text.empty())
            {
                return std::unexpected(std::string("empty operand"));
            }

            const std::size_t bracket = text.find('[');
            if (bracket == std::string_view::npos)
            {
                if (const auto reg = find_register(text); reg.has_value())
                {
                    Operand operand;
                    operand.type = ZYDIS_OPERAND_TYPE_REGISTER;
                    operand.reg  = *reg;
                    return operand;
                }
                if (const auto value = parse_number(text); value.has_value())
                {
                    Operand operand;
                    operand.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
                    operand.imm  = *value;
                    return operand;
                }
                return std::unexpected(std::format("unrecognised operand '{}'", text));
            }

            // An optional size keyword (`byte ptr`, `qword`) precedes the bracket.
            std::string_view prefix = trim(text.substr(0, bracket));
            unsigned char    size   = 0;
            if (!prefix.empty())
            {
                std::string_view  name    = prefix;
                const std::string lowered = lower(name);
                if (const std::size_t so = lowered.rfind(" ptr"); so != std::string::npos)
                {
                    name = prefix.substr(0, so);
                }
                const auto keyword = size_keyword(name);
                if (!keyword.has_value())
                {
                    return std::unexpected(std::format("unrecognised size '{}'", prefix));
                }
                size = *keyword;
            }

            const std::size_t close = text.find(']', bracket);
            if (close == std::string_view::npos || !trim(text.substr(close + 1)).empty())
            {
                return std::unexpected(std::format("malformed memory operand '{}'", text));
            }

            auto operand = parse_memory(text.substr(bracket + 1, close - bracket - 1));
            if (!operand.has_value())
            {
                return operand;
            }
            if (operand->base == ZYDIS_REGISTER_RIP || operand->base == ZYDIS_REGISTER_EIP)
            {
                return std::unexpected(std::format("write '[{}]' as an absolute address, not relative to RIP",
                                                   text.substr(bracket + 1, close - bracket - 1)));
            }
            operand->size = size;
            return operand;
        }

        [[nodiscard]] ZydisMachineMode zydis_mode(MachineMode mode) noexcept
        {
            return mode == MachineMode::legacy_32 ? ZYDIS_MACHINE_MODE_LEGACY_32 : ZYDIS_MACHINE_MODE_LONG_64;
        }

        // The bytes a register operand names, for sizing a memory operand whose
        // text does not say how wide it is.
        [[nodiscard]] unsigned char register_bytes(ZydisRegister reg, MachineMode mode)
        {
            if (reg == ZYDIS_REGISTER_NONE)
            {
                return 0;
            }
            return static_cast<unsigned char>(ZydisRegisterGetWidth(zydis_mode(mode), reg) / 8);
        }

        [[nodiscard]] ZyanStatus
        encode(const ZydisEncoderRequest& request, std::uint64_t address, std::vector<std::byte>& bytes)
        {
            ZydisEncoderRequest mutable_request = request;
            unsigned char       buffer[ZYDIS_MAX_INSTRUCTION_LENGTH] {};
            ZyanUSize           length = sizeof(buffer);
            const ZyanStatus status = ZydisEncoderEncodeInstructionAbsolute(&mutable_request, buffer, &length, address);
            if (ZYAN_SUCCESS(status))
            {
                bytes.assign(reinterpret_cast<std::byte*>(buffer), reinterpret_cast<std::byte*>(buffer) + length);
            }
            return status;
        }
    } // namespace

    std::expected<std::vector<std::byte>, std::string> assemble(std::string_view text, const AssembleContext& context)
    {
        const std::string_view instruction_text = trim(text);
        if (instruction_text.empty())
        {
            return std::unexpected(std::string("enter an instruction"));
        }

        const std::size_t split = instruction_text.find_first_of(" \t");
        const std::string name  = lower(instruction_text.substr(0, split));
        const auto        found = mnemonic_table().find(name);
        if (found == mnemonic_table().end())
        {
            return std::unexpected(std::format("unknown mnemonic '{}'", name));
        }

        ZydisEncoderRequest request {};
        request.machine_mode      = zydis_mode(context.mode);
        request.allowed_encodings = ZYDIS_ENCODABLE_ENCODING_LEGACY;
        request.mnemonic          = found->second;

        std::vector<Operand> operands;
        if (split != std::string_view::npos)
        {
            std::string_view rest = instruction_text.substr(split);
            while (true)
            {
                const std::size_t comma   = rest.find(',');
                auto              operand = parse_operand(rest.substr(0, comma));
                if (!operand.has_value())
                {
                    return std::unexpected(std::move(operand.error()));
                }
                operands.push_back(*operand);
                if (comma == std::string_view::npos)
                {
                    break;
                }
                rest = rest.substr(comma + 1);
            }
        }

        if (operands.size() > ZYDIS_MAX_OPERAND_COUNT)
        {
            return std::unexpected(std::format("{} is too many operands", operands.size()));
        }

        request.operand_count    = static_cast<ZyanU8>(operands.size());
        std::size_t memory_index = 0;
        for (std::size_t i = 0; i < operands.size(); ++i)
        {
            const Operand&       operand = operands[i];
            ZydisEncoderOperand& encoded = request.operands[i];
            encoded.type                 = operand.type;
            switch (operand.type)
            {
            case ZYDIS_OPERAND_TYPE_REGISTER:
                encoded.reg.value = operand.reg;
                break;
            case ZYDIS_OPERAND_TYPE_IMMEDIATE:
                encoded.imm.u = operand.imm;
                break;
            case ZYDIS_OPERAND_TYPE_MEMORY:
            {
                encoded.mem.base         = operand.base;
                encoded.mem.index        = operand.index;
                encoded.mem.scale        = operand.scale;
                encoded.mem.displacement = operand.displacement;

                unsigned char size = operand.size;
                if (size == 0)
                {
                    // A sibling register fixes the width; otherwise fall back to
                    // the size of the operand the edited instruction held.
                    for (const Operand& sibling : operands)
                    {
                        if (sibling.type == ZYDIS_OPERAND_TYPE_REGISTER)
                        {
                            size = std::max(size, register_bytes(sibling.reg, context.mode));
                        }
                    }
                    if (size == 0 && memory_index < context.memory.size())
                    {
                        size = context.memory[memory_index].width;
                    }
                }
                encoded.mem.size = size;
                ++memory_index;
                break;
            }
            default:
                return std::unexpected(std::string("unsupported operand"));
            }
        }

        // The listing prints a rip-relative operand as an absolute address; give
        // that operand the register the instruction held so it stays relative.
        std::size_t memory_position = 0;
        for (std::size_t i = 0; i < operands.size(); ++i)
        {
            if (operands[i].type != ZYDIS_OPERAND_TYPE_MEMORY)
            {
                continue;
            }
            if (operands[i].base == ZYDIS_REGISTER_NONE && operands[i].index == ZYDIS_REGISTER_NONE
                && memory_position < context.memory.size() && context.memory[memory_position].rip_relative)
            {
                request.operands[i].mem.base = ZYDIS_REGISTER_RIP;
            }
            ++memory_position;
        }

        std::vector<std::byte> bytes;
        ZyanStatus             status = encode(request, context.address, bytes);

        // A base-less absolute operand can also be written relative to the
        // instruction pointer (`lea` has no absolute form at all); encode that
        // way too and keep whichever is shorter, so an edit never grows an
        // instruction it could fit.
        for (std::size_t i = 0; i < operands.size(); ++i)
        {
            if (operands[i].type != ZYDIS_OPERAND_TYPE_MEMORY || operands[i].base != ZYDIS_REGISTER_NONE
                || operands[i].index != ZYDIS_REGISTER_NONE || request.operands[i].mem.base != ZYDIS_REGISTER_NONE)
            {
                continue;
            }

            request.operands[i].mem.base = ZYDIS_REGISTER_RIP;
            std::vector<std::byte> relative;
            const ZyanStatus       relative_status = encode(request, context.address, relative);
            if (ZYAN_SUCCESS(relative_status) && (!ZYAN_SUCCESS(status) || relative.size() < bytes.size()))
            {
                bytes  = std::move(relative);
                status = relative_status;
            }
            else
            {
                request.operands[i].mem.base = ZYDIS_REGISTER_NONE;
            }
        }

        if (!ZYAN_SUCCESS(status))
        {
            return std::unexpected(std::format("the encoder rejected '{}'", instruction_text));
        }
        return bytes;
    }

    std::expected<std::vector<std::byte>, std::string> assemble_block(std::string_view            text,
                                                                      const AssembleBlockContext& context)
    {
        std::vector<std::byte> bytes;
        std::uint64_t          address     = context.base;
        std::size_t            line_number = 0;
        std::size_t            start       = 0;

        while (start <= text.size())
        {
            const std::size_t end  = text.find('\n', start);
            const std::size_t stop = end == std::string_view::npos ? text.size() : end;
            std::string_view  line = text.substr(start, stop - start);
            ++line_number;

            if (const std::size_t comment = line.find(';'); comment != std::string_view::npos)
            {
                line = line.substr(0, comment);
            }
            line = trim(line);
            if (!line.empty())
            {
                const std::expected<std::vector<std::byte>, std::string> instruction =
                    assemble(line, AssembleContext {.address = address, .mode = context.mode});
                if (!instruction)
                {
                    return std::unexpected(std::format("line {}: {}", line_number, instruction.error()));
                }
                bytes.insert(bytes.end(), instruction->begin(), instruction->end());
                address += instruction->size();
            }

            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1;
        }

        if (bytes.empty())
        {
            return std::unexpected(std::string("the text holds no instruction"));
        }
        return bytes;
    }

} // namespace slopkit::disasm
