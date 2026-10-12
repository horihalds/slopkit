#include "analysis/string_references.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
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

        [[nodiscard]] bool within_module(const ModuleRange& module, const process::RegionInfo& region) noexcept
        {
            return region.start >= module.base && region.end <= module.end;
        }

        [[nodiscard]] bool is_string_data_region(const ModuleRange&                   module,
                                                 std::span<const process::RegionInfo> regions,
                                                 std::uint64_t                        address) noexcept
        {
            for (const process::RegionInfo& region : regions)
            {
                if (address < region.start || address >= region.end)
                {
                    continue;
                }
                return region.readable && !region.executable && within_module(module, region);
            }
            return false;
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

    std::vector<StringReference> find_string_references(ReadIntoFn                           read,
                                                        const ModuleRange&                   module,
                                                        std::span<const process::RegionInfo> regions,
                                                        disasm::MachineMode                  mode,
                                                        const StringReferenceConfig&         config)
    {
        std::vector<StringReference>                            references;
        std::unordered_map<std::uint64_t, std::optional<Match>> cache;
        std::vector<std::byte>                                  string_buffer(config.max_string_bytes);
        std::vector<std::byte>                                  code_buffer(config.chunk_size + 15);

        const auto lookup_string = [&](std::uint64_t address) -> const std::optional<Match>&
        {
            const auto found = cache.find(address);
            if (found != cache.end())
            {
                return found->second;
            }

            std::optional<Match> match;
            if (const auto result = read(address, string_buffer); result.has_value() && *result > 0)
            {
                const std::size_t available = std::min<std::size_t>(*result, string_buffer.size());
                match = classify(std::span<const std::byte>(string_buffer.data(), available), config.min_length);
            }
            return cache.emplace(address, std::move(match)).first->second;
        };

        // The decoder resolves every absolute operand target - a RIP-relative or
        // absolute memory operand and a LEA's address-generation operand alike -
        // into `addresses`; a base/index register operand has no absolute target
        // and so is excluded there. Branch targets share the list and are dropped
        // by the readable, non-executable region test.
        const auto annotate = [&](const disasm::Instruction& instruction)
        {
            for (const disasm::AddressRef& ref : instruction.addresses)
            {
                if (!is_string_data_region(module, regions, ref.address))
                {
                    continue;
                }
                if (!references.empty() && references.back().instruction == instruction.address
                    && references.back().string_address == ref.address)
                {
                    continue;
                }
                if (const std::optional<Match>& match = lookup_string(ref.address); match.has_value())
                {
                    references.push_back(StringReference {instruction.address, ref.address, match->text, match->wide});
                }
            }
        };

        for (const process::RegionInfo& region : regions)
        {
            if (!region.executable || !region.readable || region.path.empty() || !within_module(module, region))
            {
                continue;
            }

            std::uint64_t address = region.start;
            std::size_t   carry   = 0;
            while (address < region.end)
            {
                const std::size_t want =
                    static_cast<std::size_t>(std::min<std::uint64_t>(config.chunk_size, region.end - address));
                std::size_t got = 0;
                if (const auto result = read(address, std::span<std::byte>(code_buffer.data() + carry, want));
                    result.has_value())
                {
                    got = std::min<std::size_t>(*result, want);
                }
                if (got == 0)
                {
                    break;
                }

                const std::size_t   limit = carry + got;
                const std::uint64_t base  = address - carry;
                const bool          last  = address + got >= region.end;

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

        log::debug(
            log::category::scan,
            std::format("referenced-string analysis of {} produced {} reference(s)", module.name, references.size()));
        return references;
    }
}
