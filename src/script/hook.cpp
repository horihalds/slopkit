#include "script/hook.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "disasm/assembler.hpp"

namespace slopkit::script::hook
{
    namespace
    {
        // A near jump reaches ±2 GB; the guard leaves a margin for the payload,
        // so a cave that passes it always assembles.
        constexpr std::uint64_t kCaveReach = 0x7FFF0000;
        // The cave is rounded up to this boundary, the mapping granularity the
        // old generated script budgeted for.
        constexpr std::size_t   kCaveAlign = 16;
        // The byte a leftover slot of the hooked window is padded with.
        constexpr std::byte     kPad {0x90};
        // The longest a near jump back to the site can encode: a `jmp` with a
        // single immediate target is at most a five-byte rel32.
        constexpr std::size_t   kMaxJumpBack = 5;

        [[nodiscard]] std::size_t round_up(std::size_t value, std::size_t multiple)
        {
            return (value + multiple - 1) / multiple * multiple;
        }

        // The mode the target's instructions encode for, from the pointer width
        // the session reports (8 bytes when the seam cannot answer).
        [[nodiscard]] disasm::MachineMode machine_mode(const MemoryApi& memory)
        {
            const std::size_t pointer = memory.pointer_size ? memory.pointer_size() : sizeof(std::uint64_t);
            return pointer == 4 ? disasm::MachineMode::legacy_32 : disasm::MachineMode::long_64;
        }

        // Expands each `%X` in `text` with the next absolute address, exactly as
        // the `assemble` global's Lua `string.format` expansion would. The text
        // is generated, so `%` only ever introduces a placeholder.
        [[nodiscard]] std::string expand(std::string_view text, std::span<const std::uint64_t> values)
        {
            std::string expanded;
            std::size_t next = 0;
            for (std::size_t index = 0; index < text.size(); ++index)
            {
                if (text[index] == '%' && index + 1 < text.size() && text[index + 1] == 'X')
                {
                    if (next < values.size())
                    {
                        expanded += std::format("{:X}", values[next++]);
                    }
                    ++index;
                    continue;
                }
                expanded += text[index];
            }
            return expanded;
        }

        // The payload the cave holds, assembled from `base`: the user's code,
        // then the replaced instructions (their `site`-relative addresses
        // expanded), then a jump back to the byte after the window.
        [[nodiscard]] std::expected<std::vector<std::byte>, std::string>
        build_payload(const Spec& spec, disasm::MachineMode mode, std::uint64_t base, std::uint64_t site)
        {
            const std::expected<std::vector<std::byte>, std::string> code =
                disasm::assemble_block(spec.code_text, disasm::AssembleBlockContext {.base = base, .mode = mode});
            if (!code)
            {
                return std::unexpected("the hook code: " + code.error());
            }

            std::vector<std::uint64_t> arguments;
            arguments.reserve(spec.address_args.size());
            for (const std::int64_t delta : spec.address_args)
            {
                arguments.push_back(site + static_cast<std::uint64_t>(delta));
            }

            const std::expected<std::vector<std::byte>, std::string> trampoline =
                disasm::assemble_block(expand(spec.trampoline_text, arguments),
                                       disasm::AssembleBlockContext {.base = base + code->size(), .mode = mode});
            if (!trampoline)
            {
                return std::unexpected("the trampoline: " + trampoline.error());
            }

            const std::string back_text = std::format("jmp 0x{:X}", site + spec.original.size());
            const std::expected<std::vector<std::byte>, std::string> back = disasm::assemble_block(
                back_text,
                disasm::AssembleBlockContext {.base = base + code->size() + trampoline->size(), .mode = mode});
            if (!back)
            {
                return std::unexpected("the jump back: " + back.error());
            }

            std::vector<std::byte> payload;
            payload.reserve(code->size() + trampoline->size() + back->size());
            payload.insert(payload.end(), code->begin(), code->end());
            payload.insert(payload.end(), trampoline->begin(), trampoline->end());
            payload.insert(payload.end(), back->begin(), back->end());
            return payload;
        }
    } // namespace

    std::expected<std::uint64_t, std::string> install(const Spec& spec, const MemoryApi& memory, std::uint64_t site)
    {
        if (!memory.read || !memory.write)
        {
            return std::unexpected(std::string {"no target is attached"});
        }
        if (!memory.allocate || !memory.deallocate)
        {
            return std::unexpected(std::string {"the target's plugin cannot allocate memory"});
        }

        const std::expected<std::vector<std::byte>, std::string> current = memory.read(site, spec.original.size());
        if (!current)
        {
            return std::unexpected(current.error());
        }
        if (*current != spec.original)
        {
            return std::unexpected(std::format("the bytes at 0x{:X} are not the expected instruction", site));
        }

        const disasm::MachineMode mode = machine_mode(memory);

        // Size the cave from the payload. The measurement assembles at the site
        // itself: that is the base every site-relative branch target is near -
        // unlike address zero, whose distance to a 64-bit site the encoder
        // refuses - so the only estimate left is the jump back, budgeted at its
        // longest (rel32) form because the cave's exact distance is not known
        // before it is mapped.
        const std::expected<std::vector<std::byte>, std::string> preview = build_payload(spec, mode, site, site);
        if (!preview)
        {
            return std::unexpected(preview.error());
        }
        std::size_t size = spec.cave_size.value_or(round_up(preview->size() + kMaxJumpBack, kCaveAlign));

        // Maps `bytes` of cave near the site, freeing and refusing one out of a
        // near jump's reach.
        const auto map_cave = [&](std::size_t bytes) -> std::expected<std::uint64_t, std::string>
        {
            const std::expected<std::uint64_t, std::string> mapped = memory.allocate(bytes, site);
            if (!mapped)
            {
                return std::unexpected(mapped.error());
            }
            const std::uint64_t span = site > *mapped ? site - *mapped : *mapped - site;
            if (span > kCaveReach)
            {
                (void)memory.deallocate(*mapped);
                return std::unexpected(std::format("the cave at 0x{:X} is out of the site's jump reach", *mapped));
            }
            return *mapped;
        };

        std::expected<std::uint64_t, std::string> cave = map_cave(size);
        if (!cave)
        {
            return std::unexpected(cave.error());
        }
        std::expected<std::vector<std::byte>, std::string> payload = build_payload(spec, mode, *cave, site);
        if (!payload)
        {
            (void)memory.deallocate(*cave);
            return std::unexpected(payload.error());
        }

        // Re-encoded branches can grow to rel32 once the payload sits at the
        // cave; when the caller did not pin a size, remap once with the measured
        // size so a grown payload still fits.
        if (payload->size() > size && !spec.cave_size)
        {
            (void)memory.deallocate(*cave);
            size = round_up(payload->size(), kCaveAlign);
            cave = map_cave(size);
            if (!cave)
            {
                return std::unexpected(cave.error());
            }
            payload = build_payload(spec, mode, *cave, site);
            if (!payload)
            {
                (void)memory.deallocate(*cave);
                return std::unexpected(payload.error());
            }
        }
        if (payload->size() > size)
        {
            (void)memory.deallocate(*cave);
            return std::unexpected(std::string {"the hook payload does not fit in the cave"});
        }

        const std::expected<void, std::string> wrote = memory.write(*cave, *payload);
        if (!wrote)
        {
            (void)memory.deallocate(*cave);
            return std::unexpected(wrote.error());
        }

        // The site becomes a near jump into the cave, every leftover byte of the
        // window NOP-padded.
        const std::expected<std::vector<std::byte>, std::string> jump = disasm::assemble_block(
            std::format("jmp 0x{:X}", *cave), disasm::AssembleBlockContext {.base = site, .mode = mode});
        if (!jump)
        {
            (void)memory.deallocate(*cave);
            return std::unexpected("the jump to the cave: " + jump.error());
        }
        if (jump->size() > spec.original.size())
        {
            (void)memory.deallocate(*cave);
            return std::unexpected(std::string {"the jump to the cave is longer than the hook window"});
        }
        std::vector<std::byte> patch = *jump;
        patch.resize(spec.original.size(), kPad);

        const std::expected<void, std::string> patched = memory.write(site, patch);
        if (!patched)
        {
            (void)memory.deallocate(*cave);
            return std::unexpected(patched.error());
        }
        return *cave;
    }

    std::expected<void, std::string>
    remove(const Spec& spec, const MemoryApi& memory, std::uint64_t site, std::uint64_t cave)
    {
        if (!memory.write)
        {
            return std::unexpected(std::string {"no target is attached"});
        }
        const std::expected<void, std::string> restored = memory.write(site, spec.original);
        if (!restored)
        {
            return std::unexpected(restored.error());
        }
        if (cave != 0)
        {
            if (!memory.deallocate)
            {
                return std::unexpected(std::string {"the target's plugin cannot allocate memory"});
            }
            const std::expected<void, std::string> freed = memory.deallocate(cave);
            if (!freed)
            {
                return std::unexpected(freed.error());
            }
        }
        return {};
    }
} // namespace slopkit::script::hook
