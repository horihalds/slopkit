#include "ui/access_watch.hpp"
#include "ui/address_format.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace slopkit::ui
{
    namespace
    {
        // The register file holds the 64-bit names; an address-size override can
        // name a 32-bit register, which shares the 64-bit value.
        constexpr std::array<std::pair<std::string_view, std::string_view>, 17> kRegisterAliases = {
            {
             {"EAX", "RAX"},
             {"EBX", "RBX"},
             {"ECX", "RCX"},
             {"EDX", "RDX"},
             {"ESI", "RSI"},
             {"EDI", "RDI"},
             {"EBP", "RBP"},
             {"ESP", "RSP"},
             {"EIP", "RIP"},
             {"R8D", "R8"},
             {"R9D", "R9"},
             {"R10D", "R10"},
             {"R11D", "R11"},
             {"R12D", "R12"},
             {"R13D", "R13"},
             {"R14D", "R14"},
             {"R15D", "R15"},
             }
        };

        [[nodiscard]] std::string_view canonical_register(std::string_view name)
        {
            for (const auto& [from, to] : kRegisterAliases)
            {
                if (name == from)
                {
                    return to;
                }
            }
            return name;
        }

        [[nodiscard]] std::optional<std::uint64_t> register_value(std::string_view                      name,
                                                                  std::span<const debug::RegisterValue> registers)
        {
            if (name.empty())
            {
                return std::nullopt;
            }

            const std::string_view canonical = canonical_register(name);
            for (const debug::RegisterValue& entry : registers)
            {
                if (entry.name == canonical)
                {
                    return entry.value;
                }
            }
            return std::nullopt;
        }

        // The operand exactly as Zydis prints it: a rip-relative or absolute
        // operand shows its 16-digit address, a register operand its sum.
        [[nodiscard]] QString operand_text(const disasm::MemoryRef& ref, std::uint64_t address)
        {
            if (ref.rip_relative || (ref.base.empty() && ref.index.empty()))
            {
                return QLatin1Char('[') + format_padded_hex(address, 16) + QLatin1Char(']');
            }

            QString text = QStringLiteral("[");
            if (!ref.base.empty())
            {
                text += QString::fromStdString(ref.base);
            }
            if (!ref.index.empty())
            {
                if (!ref.base.empty())
                {
                    text += QLatin1Char('+');
                }
                text += QString::fromStdString(ref.index);
                if (ref.scale > 1)
                {
                    text += QLatin1Char('*') + QString::number(ref.scale);
                }
            }
            if (ref.displacement != 0)
            {
                text += ref.displacement > 0 ? QLatin1Char('+') : QLatin1Char('-');
                const std::uint64_t magnitude = ref.displacement > 0
                                                  ? static_cast<std::uint64_t>(ref.displacement)
                                                  : static_cast<std::uint64_t>(-(ref.displacement + 1)) + 1;
                text += format_padded_hex(magnitude, 2);
            }
            text += QLatin1Char(']');
            return text;
        }
    } // namespace

    std::vector<ResolvedAccess> resolve_accesses(std::span<const disasm::MemoryRef>    operands,
                                                 std::uint64_t                         instruction_address,
                                                 std::size_t                           instruction_length,
                                                 std::span<const debug::RegisterValue> registers)
    {
        std::vector<ResolvedAccess> accesses;
        accesses.reserve(operands.size());

        for (const disasm::MemoryRef& ref : operands)
        {
            ResolvedAccess access;
            access.width  = ref.width == 0 ? 1 : ref.width;
            access.writes = ref.writes;

            bool          resolved = true;
            std::uint64_t address  = 0;

            if (ref.rip_relative)
            {
                address = instruction_address + instruction_length + static_cast<std::uint64_t>(ref.displacement);
            }
            else
            {
                std::uint64_t base = 0;
                if (!ref.base.empty())
                {
                    if (const auto found = register_value(ref.base, registers))
                    {
                        base = *found;
                    }
                    else
                    {
                        resolved = false;
                    }
                }

                std::uint64_t index = 0;
                if (!ref.index.empty())
                {
                    if (const auto found = register_value(ref.index, registers))
                    {
                        index = *found * ref.scale;
                    }
                    else
                    {
                        resolved = false;
                    }
                }

                if (resolved)
                {
                    address = base + index + static_cast<std::uint64_t>(ref.displacement);
                }
            }

            access.resolved = resolved;
            access.address  = resolved ? address : 0;
            access.operand  = operand_text(ref, address);
            accesses.push_back(std::move(access));
        }

        return accesses;
    }

    std::size_t watch_size(std::size_t width) noexcept
    {
        switch (width)
        {
        case 1:
        case 2:
        case 4:
        case 8:
            return width;
        default:
            return 4;
        }
    }

} // namespace slopkit::ui
