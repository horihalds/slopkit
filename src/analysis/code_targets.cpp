#include "analysis/code_targets.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::analysis
{
    namespace
    {
        [[nodiscard]] std::string_view mnemonic_of(std::string_view text) noexcept
        {
            return text.substr(0, text.find_first_of(" \t"));
        }

        [[nodiscard]] std::optional<CodeTargetKind> classify_branch(std::string_view mnemonic) noexcept
        {
            if (mnemonic == "CALL")
            {
                return CodeTargetKind::call;
            }
            if (mnemonic == "JMP" || mnemonic == "LJMP")
            {
                return CodeTargetKind::jump;
            }

            static constexpr std::string_view conditional[] = {
                "JA",    "JAE",  "JB",  "JBE",  "JC",    "JCXZ",   "JE",     "JECXZ", "JG",  "JGE",
                "JL",    "JLE",  "JNA", "JNAE", "JNB",   "JNBE",   "JNC",    "JNE",   "JNG", "JNGE",
                "JNL",   "JNLE", "JNO", "JNP",  "JNS",   "JNZ",    "JO",     "JP",    "JPE", "JPO",
                "JRCXZ", "JS",   "JZ",  "LOOP", "LOOPE", "LOOPNE", "LOOPNZ", "LOOPZ"};
            if (std::ranges::find(conditional, mnemonic) != std::ranges::end(conditional))
            {
                return CodeTargetKind::conditional_jump;
            }
            return std::nullopt;
        }

        [[nodiscard]] bool enabled(CodeTargetKinds kinds, CodeTargetKind kind) noexcept
        {
            switch (kind)
            {
            case CodeTargetKind::call:
                return kinds.call;
            case CodeTargetKind::jump:
                return kinds.jump;
            case CodeTargetKind::conditional_jump:
                return kinds.conditional_jump;
            }
            return false;
        }
    }

    std::string describe(const CodeTargetScanReport& report)
    {
        std::string text =
            std::format("{} region(s) in the module, {} code region(s), {} byte(s), {} unreadable chunk(s), "
                        "{} instruction(s), {} branch(es), {} indirect branch(es), {} outside the module, {} target(s)",
                        report.regions_in_module,
                        report.code_regions,
                        report.bytes_read,
                        report.read_failures,
                        report.instructions,
                        report.branches,
                        report.indirect_branches,
                        report.outside_module,
                        report.targets);
        if (report.excluded.empty())
        {
            return text;
        }
        return std::format("{}; skipped {}", text, describe_excluded_regions(report.excluded));
    }

    CodeTargetScanResult find_code_targets(ReadIntoFn                           read,
                                           const ModuleRange&                   module,
                                           std::span<const process::RegionInfo> regions,
                                           disasm::MachineMode                  mode,
                                           CodeTargetKinds                      kinds,
                                           const CodeTargetScanConfig&          config)
    {
        CodeTargetScanResult              result;
        CodeTargetScanReport&             report  = result.report;
        std::vector<CodeTarget>&          targets = result.targets;
        std::vector<std::byte>            code_buffer(config.chunk_size + 15);
        std::unordered_set<std::uint64_t> seen;

        if (module.end <= module.base)
        {
            exclude_region(report.excluded, module.base, module.end, RegionReason::empty_module);
            log::debug(log::category::scan,
                       std::format("code-target analysis of {}: {}", module.name, describe(report)));
            return result;
        }

        const auto collect = [&](const disasm::Instruction& instruction)
        {
            const std::optional<CodeTargetKind> kind = classify_branch(mnemonic_of(instruction.text));
            if (!kind.has_value() || !enabled(kinds, *kind))
            {
                return;
            }
            ++report.branches;

            // A direct branch prints its target as the instruction's only
            // absolute address; an indirect one (`CALL [RIP+x]`, `JMP RAX`) has
            // a memory operand or a register operand and no target at all.
            if (instruction.addresses.size() != 1 || !instruction.memory.empty())
            {
                ++report.indirect_branches;
                return;
            }

            const std::uint64_t target = instruction.addresses.front().address;
            if (target < module.base || target >= module.end)
            {
                ++report.outside_module;
                return;
            }
            if (!seen.insert(target).second)
            {
                return;
            }
            targets.push_back(CodeTarget {instruction.address, target, *kind});
        };

        struct SweepRange
        {
            std::uint64_t start {};
            std::uint64_t end {};
        };

        std::vector<SweepRange> sweeps;
        for (const process::RegionInfo& region : regions)
        {
            if (!overlaps_module(module, region))
            {
                continue;
            }
            ++report.regions_in_module;

            if (!region.readable)
            {
                exclude_region(report.excluded, region.start, region.end, RegionReason::not_readable);
                continue;
            }
            if (!region.executable)
            {
                exclude_region(report.excluded, region.start, region.end, RegionReason::not_executable);
                continue;
            }
            sweeps.push_back(SweepRange {std::max(region.start, module.base), std::min(region.end, module.end)});
            ++report.code_regions;
        }

        std::ranges::sort(sweeps, {}, &SweepRange::start);

        for (const SweepRange& sweep : sweeps)
        {
            std::uint64_t address = sweep.start;
            std::size_t   carry   = 0;
            while (address < sweep.end)
            {
                const std::size_t want =
                    static_cast<std::size_t>(std::min<std::uint64_t>(config.chunk_size, sweep.end - address));
                if (want == 0)
                {
                    break;
                }

                std::size_t got = 0;
                if (const auto chunk = read(address, std::span<std::byte>(code_buffer.data() + carry, want));
                    chunk.has_value())
                {
                    got = std::min<std::size_t>(*chunk, want);
                }
                if (got == 0)
                {
                    // One unreadable chunk must not erase the region's other
                    // targets, and the carry no longer precedes the new
                    // address, so it is dropped with the skipped bytes.
                    ++report.read_failures;
                    log::debug(
                        log::category::scan,
                        std::format("code-target analysis of {}: unreadable chunk at {:#x}", module.name, address));
                    carry = 0;
                    address += want;
                    continue;
                }
                report.bytes_read += got;

                const std::size_t   limit = carry + got;
                const std::uint64_t base  = address - carry;
                const bool          last  = address + got >= sweep.end;

                std::size_t offset = 0;
                while (offset < limit)
                {
                    // A tail shorter than the longest instruction may be the
                    // prefix of one continuing after the boundary, so it is
                    // carried over instead of decoded.
                    if (!last && limit - offset < 15)
                    {
                        break;
                    }
                    const auto decoded = disasm::decode(
                        std::span<const std::byte>(code_buffer.data() + offset, limit - offset), base + offset, mode);
                    if (!decoded.has_value() || !decoded->valid || decoded->length == 0
                        || decoded->length > limit - offset)
                    {
                        ++offset;
                        continue;
                    }
                    ++report.instructions;
                    collect(*decoded);
                    offset += decoded->length;
                }

                const std::size_t tail = limit - offset;
                if (tail > 0)
                {
                    std::memmove(code_buffer.data(), code_buffer.data() + offset, tail);
                }
                carry = tail;
                address += got;
            }
        }

        std::stable_sort(targets.begin(),
                         targets.end(),
                         [](const CodeTarget& lhs, const CodeTarget& rhs)
                         {
                             if (lhs.instruction != rhs.instruction)
                             {
                                 return lhs.instruction < rhs.instruction;
                             }
                             return lhs.target < rhs.target;
                         });
        report.targets = targets.size();
        log::debug(log::category::scan, std::format("code-target analysis of {}: {}", module.name, describe(report)));
        return result;
    }
}
