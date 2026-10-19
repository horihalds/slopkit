#include "analysis/string_references.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::analysis
{
    namespace
    {
        [[nodiscard]] constexpr bool is_printable_ascii(std::byte value) noexcept
        {
            const auto byte = std::to_integer<unsigned char>(value);
            return byte >= 0x20 && byte <= 0x7E;
        }

        // The readable region of the module that holds `address`, or nullptr when
        // the address is outside the module or inside no readable region. The
        // region neither needs a mapped file nor has to be non-executable, and it
        // may reach past the module end: the address, not the region, is what has
        // to be inside the module.
        [[nodiscard]] const process::RegionInfo* readable_module_region(const ModuleRange&                   module,
                                                                        std::span<const process::RegionInfo> regions,
                                                                        std::uint64_t address) noexcept
        {
            if (address < module.base || address >= module.end)
            {
                return nullptr;
            }
            for (const process::RegionInfo& region : regions)
            {
                if (region.readable && address >= region.start && address < region.end)
                {
                    return &region;
                }
            }
            return nullptr;
        }

        struct Match
        {
            std::string text;
            bool        wide {};
        };

        [[nodiscard]] std::optional<Match> classify(std::span<const std::byte> bytes, std::size_t min_length)
        {
            std::size_t printable = 0;
            while (printable < bytes.size() && is_printable_ascii(bytes[printable]))
            {
                ++printable;
            }
            if (printable >= min_length && printable < bytes.size() && bytes[printable] == std::byte {0})
            {
                return Match {std::string(reinterpret_cast<const char*>(bytes.data()), printable), false};
            }

            std::size_t characters = 0;
            while (2 * characters + 1 < bytes.size() && is_printable_ascii(bytes[2 * characters])
                   && bytes[2 * characters + 1] == std::byte {0})
            {
                ++characters;
            }
            if (characters >= min_length && 2 * characters + 1 < bytes.size() && bytes[2 * characters] == std::byte {0}
                && bytes[2 * characters + 1] == std::byte {0})
            {
                std::string text;
                text.reserve(characters);
                for (std::size_t index = 0; index < characters; ++index)
                {
                    text.push_back(std::to_integer<char>(bytes[2 * index]));
                }
                return Match {std::move(text), true};
            }
            return std::nullopt;
        }
    }

    std::string describe(const StringScanReport& report)
    {
        std::string text =
            std::format("{} region(s) in the module, {} code region(s), {} byte(s), {} unreadable chunk(s), "
                        "{} instruction(s), {} resolved reference(s), {} string(s)",
                        report.regions_in_module,
                        report.code_regions,
                        report.bytes_read,
                        report.read_failures,
                        report.instructions,
                        report.address_references,
                        report.strings);
        if (report.excluded.empty())
        {
            return text;
        }
        return std::format("{}; skipped {}", text, describe_excluded_regions(report.excluded));
    }

    StringScanResult find_string_references(ReadIntoFn                           read,
                                            const ModuleRange&                   module,
                                            std::span<const process::RegionInfo> regions,
                                            disasm::MachineMode                  mode,
                                            const StringReferenceConfig&         config)
    {
        StringScanResult                                        scan;
        StringScanReport&                                       report     = scan.report;
        std::vector<StringReference>&                           references = scan.references;
        std::unordered_map<std::uint64_t, std::optional<Match>> cache;
        std::vector<std::byte>                                  string_buffer(config.max_string_bytes);
        std::vector<std::byte>                                  code_buffer(config.chunk_size + 15);

        if (module.end <= module.base)
        {
            exclude_region(report.excluded, module.base, module.end, RegionReason::empty_module);
            log::debug(log::category::scan,
                       std::format("referenced-string analysis of {}: {}", module.name, describe(report)));
            return scan;
        }

        const auto lookup_string = [&](std::uint64_t address, std::size_t limit) -> const std::optional<Match>&
        {
            const auto found = cache.find(address);
            if (found != cache.end())
            {
                return found->second;
            }

            std::optional<Match> match;
            const std::size_t    want = std::min(limit, string_buffer.size());
            if (const auto result = read(address, std::span<std::byte>(string_buffer.data(), want));
                result.has_value() && *result > 0)
            {
                const std::size_t available = std::min<std::size_t>(*result, want);
                match = classify(std::span<const std::byte>(string_buffer.data(), available), config.min_length);
            }
            return cache.emplace(address, std::move(match)).first->second;
        };

        // The decoder resolves every absolute operand target - a RIP-relative or
        // absolute memory operand and a LEA's address-generation operand alike -
        // into `addresses`; a base/index register operand has no absolute target
        // and so is excluded there. Branch targets share the list and are dropped
        // by the readable region test.
        const auto annotate = [&](const disasm::Instruction& instruction)
        {
            report.address_references += instruction.addresses.size();
            for (const disasm::AddressRef& ref : instruction.addresses)
            {
                const process::RegionInfo* region = readable_module_region(module, regions, ref.address);
                if (region == nullptr)
                {
                    continue;
                }
                if (!references.empty() && references.back().instruction == instruction.address
                    && references.back().string_address == ref.address)
                {
                    continue;
                }
                // A string that ends at the region boundary is still classified,
                // so the read stops there instead of at the global cap alone.
                const std::size_t limit = static_cast<std::size_t>(
                    std::min<std::uint64_t>(config.max_string_bytes, region->end - ref.address));
                if (const std::optional<Match>& match = lookup_string(ref.address, limit); match.has_value())
                {
                    references.push_back(StringReference {instruction.address, ref.address, match->text, match->wide});
                }
            }
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
                    // references, and the carry no longer precedes the new
                    // address, so it is dropped with the skipped bytes.
                    ++report.read_failures;
                    log::debug(log::category::scan,
                               std::format("referenced-string analysis of {}: unreadable chunk at {:#x}",
                                           module.name,
                                           address));
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
                    annotate(*decoded);
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

        std::stable_sort(references.begin(),
                         references.end(),
                         [](const StringReference& lhs, const StringReference& rhs)
                         {
                             if (lhs.instruction != rhs.instruction)
                             {
                                 return lhs.instruction < rhs.instruction;
                             }
                             return lhs.string_address < rhs.string_address;
                         });

        report.strings = references.size();
        log::debug(log::category::scan,
                   std::format("referenced-string analysis of {}: {}", module.name, describe(report)));
        return scan;
    }
}
