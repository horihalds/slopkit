#include "script/hook_script.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "disasm/assembler.hpp"

namespace slopkit::script
{
    namespace
    {
        // A near jump rel32 is the shortest form the site's jump into the cave
        // takes, so the window always covers at least this many bytes.
        constexpr std::size_t   kWindowMinimum     = 5;
        // A short instruction still needs a signature this long to be unique.
        constexpr std::size_t   kPatternMinimum    = 16;
        // How many rows one side of the window the pattern prefers to include.
        constexpr std::size_t   kPatternNeighbours = 2;
        // The cave budget: a stub allowance for the user's hook code plus a
        // re-encoding allowance for the trampoline (a rel8 branch can grow into
        // rel32), rounded up to a comfortable boundary.
        constexpr std::size_t   kStubAllowance     = 128;
        constexpr std::size_t   kTrampolineGrowth  = 4;
        // A near jump reaches ±2 GB; the generated guard leaves a margin for the
        // stub, the trampoline and both jump displacements, so a cave that
        // passes it always assembles.
        constexpr std::uint64_t kCaveReach         = 0x7FFF0000;

        [[nodiscard]] std::size_t round_up(std::size_t value, std::size_t multiple)
        {
            return (value + multiple - 1) / multiple * multiple;
        }

        [[nodiscard]] std::string lower_ascii(std::string text)
        {
            for (char& character : text)
            {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            return text;
        }

        // " + 0x40" / " - 0x120": how far past the site the runtime value of an
        // address operand sits, so the emitted text keeps pointing at the same
        // place after the module moves.
        [[nodiscard]] std::string offset_suffix(std::int64_t delta)
        {
            if (delta >= 0)
            {
                return std::format(" + 0x{:X}", static_cast<std::uint64_t>(delta));
            }
            return std::format(" - 0x{:X}", static_cast<std::uint64_t>(-delta));
        }

        // The rows are decoded from one contiguous cache, so the next row starts
        // where the previous one ended; a row the cache could not fill ends the
        // run.
        [[nodiscard]] bool follows(const HookRow& previous, const HookRow& next)
        {
            return !previous.bytes.empty() && previous.address + previous.bytes.size() == next.address;
        }

        [[nodiscard]] bool usable(const HookRow& row)
        {
            return row.valid && !row.bytes.empty();
        }

        // The window a hook overwrites: whole instructions from the selected row
        // until a near jump fits. Sets `reason` and returns nothing when the
        // listing has not decoded enough bytes after the row.
        [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> hook_window(const HookCandidate& candidate,
                                                                                     std::string&         reason)
        {
            std::size_t last  = candidate.selected;
            std::size_t total = 0;
            while (total < kWindowMinimum)
            {
                if (last >= candidate.rows.size() || !usable(candidate.rows[last]))
                {
                    reason = "the hook window runs past the bytes the listing has decoded";
                    return std::nullopt;
                }
                if (last > candidate.selected && !follows(candidate.rows[last - 1], candidate.rows[last]))
                {
                    reason = "the hook window runs past the bytes the listing has decoded";
                    return std::nullopt;
                }
                total += candidate.rows[last].bytes.size();
                ++last;
            }
            return std::pair {candidate.selected, last};
        }

        // The pattern span: the window plus up to two rows on each side, widened
        // until the signature is at least `kPatternMinimum` bytes and clipped at
        // the decoded listing's ends.
        [[nodiscard]] std::pair<std::size_t, std::size_t> pattern_span(const HookCandidate& candidate,
                                                                       std::size_t          window_first,
                                                                       std::size_t          window_last,
                                                                       std::size_t          window_bytes)
        {
            std::size_t first = window_first;
            std::size_t last  = window_last;
            std::size_t total = window_bytes;

            const auto left = [&]
            {
                return first > 0 && usable(candidate.rows[first - 1])
                    && follows(candidate.rows[first - 1], candidate.rows[first]);
            };
            const auto right = [&]
            {
                return last < candidate.rows.size() && usable(candidate.rows[last])
                    && follows(candidate.rows[last - 1], candidate.rows[last]);
            };

            for (std::size_t neighbour = 0; neighbour < kPatternNeighbours; ++neighbour)
            {
                if (left())
                {
                    --first;
                    total += candidate.rows[first].bytes.size();
                }
                if (right())
                {
                    total += candidate.rows[last].bytes.size();
                    ++last;
                }
            }
            while (total < kPatternMinimum)
            {
                if (left())
                {
                    --first;
                    total += candidate.rows[first].bytes.size();
                }
                else if (right())
                {
                    total += candidate.rows[last].bytes.size();
                    ++last;
                }
                else
                {
                    break;
                }
            }

            return {first, last};
        }

        // A Lua double-quoted string; the generated text only ever holds hex and
        // spaces, but escaping keeps the render robust.
        [[nodiscard]] std::string quote(std::string_view text)
        {
            std::string quoted = "\"";
            for (const char character : text)
            {
                if (character == '"' || character == '\\')
                {
                    quoted += '\\';
                }
                quoted += character;
            }
            quoted += '"';
            return quoted;
        }
    } // namespace

    std::string pattern_text(std::span<const std::byte> bytes, std::span<const disasm::AddressBytes> wildcards)
    {
        std::vector<bool> masked(bytes.size(), false);
        for (const disasm::AddressBytes& field : wildcards)
        {
            for (std::size_t index = field.offset; index < field.offset + field.length && index < bytes.size(); ++index)
            {
                masked[index] = true;
            }
        }

        std::string text;
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            if (index != 0)
            {
                text += ' ';
            }
            text += masked[index] ? std::string {"??"} : std::format("{:02X}", std::to_integer<unsigned>(bytes[index]));
        }
        return text;
    }

    std::optional<HookTarget> build_hook(const HookCandidate& candidate, std::string& reason)
    {
        if (candidate.selected >= candidate.rows.size() || !usable(candidate.rows[candidate.selected]))
        {
            reason = "this row is not a decoded instruction";
            return std::nullopt;
        }

        const std::optional<std::pair<std::size_t, std::size_t>> window = hook_window(candidate, reason);
        if (!window)
        {
            return std::nullopt;
        }
        const std::size_t window_first = window->first;
        const std::size_t window_last  = window->second;
        const HookRow&    selected     = candidate.rows[candidate.selected];

        HookTarget target;
        target.address            = selected.address;
        target.description        = candidate.description;
        target.module_name        = candidate.module_name;
        target.module_rva_text    = candidate.module_rva_text;
        target.instruction_length = selected.bytes.size();

        std::string verify_text;
        for (std::size_t index = window_first; index < window_last; ++index)
        {
            const HookRow& row = candidate.rows[index];

            HookInstruction covered;
            covered.address = row.address;
            covered.bytes.assign(row.bytes.begin(), row.bytes.end());
            covered.text = row.text;

            // The listing prints every address absolute. Re-emit each operand as
            // `0x%X` with its runtime value passed as `site ± delta`, so the
            // trampoline keeps pointing at the same data after ASLR moves the
            // module.
            std::string line        = row.text;
            std::string verify_line = row.text;
            for (auto reference = row.addresses.rbegin(); reference != row.addresses.rend(); ++reference)
            {
                line.replace(reference->offset, reference->length, "0x%X");
                verify_line.replace(reference->offset, reference->length, std::format("0x{:X}", reference->address));
            }
            for (const disasm::AddressRef& reference : row.addresses)
            {
                const std::int64_t delta =
                    static_cast<std::int64_t>(reference.address) - static_cast<std::int64_t>(target.address);
                covered.address_arguments.push_back(std::format("site{}", offset_suffix(delta)));
            }
            covered.rewritten = {line};

            target.trampoline_lines.push_back(line);
            target.original.insert(target.original.end(), row.bytes.begin(), row.bytes.end());
            target.covered.push_back(std::move(covered));

            if (!verify_text.empty())
            {
                verify_text += '\n';
            }
            verify_text += verify_line;
        }

        // Prove the rewritten trampoline encodes before any of it reaches the
        // editor, so a broken hook is a refusal, not a script that dies on
        // activation.
        const std::expected<std::vector<std::byte>, std::string> verified = disasm::assemble_block(
            verify_text, disasm::AssembleBlockContext {.base = target.address, .mode = candidate.mode});
        if (!verified)
        {
            reason = "the replaced instructions cannot be re-encoded: " + verified.error();
            return std::nullopt;
        }

        const std::pair<std::size_t, std::size_t> span =
            pattern_span(candidate, window_first, window_last, target.original.size());

        std::vector<std::byte>            pattern_bytes;
        std::vector<disasm::AddressBytes> wildcards;
        std::size_t                       offset       = 0;
        std::size_t                       match_offset = 0;
        for (std::size_t index = span.first; index < span.second; ++index)
        {
            const HookRow& row = candidate.rows[index];
            if (index == window_first)
            {
                match_offset = offset;
            }
            pattern_bytes.insert(pattern_bytes.end(), row.bytes.begin(), row.bytes.end());
            // Only an absolute field changes with the module base; a relative
            // branch or rip-relative displacement is module-relative and stays
            // literal.
            for (const disasm::AddressBytes& field : row.address_bytes)
            {
                if (!field.relative)
                {
                    wildcards.push_back({offset + field.offset, field.length, false});
                }
            }
            offset += row.bytes.size();
        }

        target.pattern        = pattern_text(pattern_bytes, wildcards);
        target.pattern_offset = match_offset;
        target.cave_size      = round_up(kStubAllowance + kTrampolineGrowth * target.original.size(), 16);
        return target;
    }

    std::string render(const HookTarget& target)
    {
        const bool        in_module = !target.module_name.empty();
        const std::string suffix =
            in_module ? lower_ascii(target.module_rva_text) : std::format("{:x}", target.address);
        const std::string site_name = "hook_site_" + suffix;
        const std::string cave_name = "hook_cave_" + suffix;

        const std::size_t window = target.original.size();

        std::string out;
        out += std::format("-- Hook for \"{}\" at {}, generated by the disassembly view.\n",
                           target.covered.front().text,
                           target.description);
        out += "-- activate() finds the instruction with an AoB pattern, maps a code cave in the\n";
        out += "-- target, writes your code plus the instructions the hook replaces into it, then\n";
        out += "-- jumps the site into the cave; deactivate() restores the original bytes and frees\n";
        out += "-- the cave. The hook's state lives in the two labels kSiteName (the hooked address)\n";
        out += "-- and kCaveName (the stub); activate() publishes them and deactivate() forgets them.\n";
        if (window > target.instruction_length)
        {
            out += std::format("-- The window covers {} bytes: the selected instruction plus the next ones,\n"
                               "-- because a near jump needs {}; any leftover byte is NOP-padded.\n",
                               window,
                               kWindowMinimum);
        }
        else
        {
            out += std::format("-- The window is the selected instruction's {} bytes; the site keeps its jump\n"
                               "-- and NOP-pads the rest.\n",
                               window);
        }
        out += "-- Note: this write is not shown on the listing and is undone by deactivate() and\n";
        out += "-- on quit.\n\n";

        if (in_module)
        {
            out += std::format("local kModule      = {}\n", quote(target.module_name));
        }
        out += std::format("local kPattern     = {}\n", quote(target.pattern));
        out += std::format("local kMatchOffset = {}      -- the hooked window's offset inside the match\n",
                           target.pattern_offset);
        out += std::format("local kWindow      = {}       -- bytes the hook overwrites, whole instructions only\n",
                           window);
        out += "local kOriginal    = string.char(";
        for (std::size_t index = 0; index < target.original.size(); ++index)
        {
            if (index != 0)
            {
                out += ", ";
            }
            out += std::format("0x{:02X}", std::to_integer<unsigned>(target.original[index]));
        }
        out += ")\n";
        out += std::format("local kCaveSize    = {}     -- your hook code must fit beside the trampoline\n",
                           target.cave_size);
        out += std::format("local kSiteName    = {}\n", quote(site_name));
        out += std::format("local kCaveName    = {}\n", quote(cave_name));
        out += "local kPadding     = string.rep(string.char(0x90), kWindow)\n";
        out += std::format("local kReach       = 0x{:X}   -- a near jump reaches ±2 GB, less the payload's margin\n\n",
                           kCaveReach);

        out += "-- Your code runs first, then the instructions the hook replaced (re-encoded at the\n";
        out += "-- cave so their addresses keep working), then the jump back to the site.\n";
        out += "local kHookCode = [[\n";
        out += "    nop    ; TODO: replace with your code\n";
        out += "]]\n\n";

        out += "-- The instructions the hook replaces, re-encoded at the cave.\n";
        out += "local kTrampoline = [[\n";
        for (const std::string& line : target.trampoline_lines)
        {
            out += std::format("    {}\n", line);
        }
        out += "]]\n\n";

        const std::string scan = in_module ? ", kModule" : "";
        const std::string missed =
            in_module ? "\"the hook pattern was not found in \" .. kModule" : "\"the hook pattern was not found\"";
        std::string trampoline_arguments;
        for (const HookInstruction& instruction : target.covered)
        {
            for (const std::string& argument : instruction.address_arguments)
            {
                trampoline_arguments += ", " + argument;
            }
        }

        out += "function activate()\n";
        out += std::format("    local found, match = aobscan(kSiteName, kPattern{})\n", scan);
        out += "    if not found then\n";
        out += std::format("        return false, {}\n", missed);
        out += "    end\n";
        out += "    -- `match` is where the pattern starts; the window it stands for begins\n";
        out += "    -- kMatchOffset bytes in, and that site is what deactivate() restores.\n";
        out += "    local site = match + kMatchOffset\n";
        out += "    slabel(kSiteName, site)\n";
        out += "    for i = 0, kWindow - 1 do\n";
        out += "        if read_u8(site + i) ~= kOriginal:byte(i + 1) then\n";
        out += "            ulabel(kSiteName)\n";
        out +=
            "            return false, string.format(\"the bytes at 0x%X are not the expected instruction\", site)\n";
        out += "        end\n";
        out += "    end\n\n";
        out += "    local cave = alloc(kCaveName, kCaveSize, site)\n\n";
        out += "    -- The site jumps into the cave with a near jump, so a cave the hint could not\n";
        out += "    -- place within reach is freed and refused before the site is touched.\n";
        out += "    local span = site > cave and site - cave or cave - site\n";
        out += "    if span > kReach then\n";
        out += "        dealloc(kCaveName)\n";
        out += "        ulabel(kCaveName)\n";
        out += "        ulabel(kSiteName)\n";
        out += "        return false, string.format(\"the cave at 0x%X is out of the site's jump reach\", cave)\n";
        out += "    end\n\n";
        out += "    local ok, hook_len = assemble(cave, kHookCode)\n";
        out += "    if not ok then\n";
        out += "        dealloc(kCaveName)\n";
        out += "        ulabel(kCaveName)\n";
        out += "        ulabel(kSiteName)\n";
        out += "        return false, \"the hook code: \" .. hook_len\n";
        out += "    end\n";
        out +=
            std::format("    local ok2, tramp_len = assemble(cave + hook_len, kTrampoline{})\n", trampoline_arguments);
        out += "    if not ok2 then\n";
        out += "        dealloc(kCaveName)\n";
        out += "        ulabel(kCaveName)\n";
        out += "        ulabel(kSiteName)\n";
        out += "        return false, \"the trampoline: \" .. tramp_len\n";
        out += "    end\n";
        out += "    local ok3, back_len = assemble(cave + hook_len + tramp_len, \"jmp 0x%X\", site + kWindow)\n";
        out += "    if not ok3 then\n";
        out += "        dealloc(kCaveName)\n";
        out += "        ulabel(kCaveName)\n";
        out += "        ulabel(kSiteName)\n";
        out += "        return false, \"the jump back: \" .. back_len\n";
        out += "    end\n\n";
        out += "    -- The site jumps into the cave; every leftover byte is padded with NOP.\n";
        out += "    local ok4, jmp_len = assemble(site, \"jmp 0x%X\", cave)\n";
        out += "    if not ok4 then\n";
        out += "        dealloc(kCaveName)\n";
        out += "        ulabel(kCaveName)\n";
        out += "        ulabel(kSiteName)\n";
        out += "        return false, \"the jump to the cave: \" .. jmp_len\n";
        out += "    end\n";
        out += "    if jmp_len < kWindow then\n";
        out += "        mem.write_bytes(site + jmp_len, kPadding:sub(1, kWindow - jmp_len))\n";
        out += "    end\n";
        out += "    return true\n";
        out += "end\n\n";

        out += "function deactivate()\n";
        out += "    local site = label(kSiteName)\n";
        out += "    local cave = label(kCaveName)\n";
        out += "    if site == 0 then\n";
        out += "        return false, \"this hook's site is not known anymore\"\n";
        out += "    end\n";
        out += "    mem.write_bytes(site, kOriginal)\n";
        out += "    if cave ~= 0 then\n";
        out += "        dealloc(kCaveName)\n";
        out += "        ulabel(kCaveName)\n";
        out += "    end\n";
        out += "    ulabel(kSiteName)\n";
        out += "    return true\n";
        out += "end\n";

        return out;
    }
} // namespace slopkit::script
