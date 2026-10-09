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
        constexpr std::size_t kWindowMinimum     = 5;
        // A short instruction still needs a signature this long to be unique.
        constexpr std::size_t kPatternMinimum    = 16;
        // How many rows one side of the window the pattern prefers to include.
        constexpr std::size_t kPatternNeighbours = 2;

        [[nodiscard]] std::string lower_ascii(std::string text)
        {
            for (char& character : text)
            {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            return text;
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
                covered.address_args.push_back(delta);
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
        return target;
    }

    std::string render(const HookTarget& target)
    {
        const bool        in_module = !target.module_name.empty();
        const std::string suffix =
            in_module ? lower_ascii(target.module_rva_text) : std::format("{:x}", target.address);
        const std::string site_name = "hook_site_" + suffix;
        const std::string cave_name = "hook_cave_" + suffix;

        // Every `%X` slot of the trampoline, paired with its site-relative value,
        // in the order the placeholders appear.
        std::vector<std::int64_t> arguments;
        for (const HookInstruction& instruction : target.covered)
        {
            arguments.insert(arguments.end(), instruction.address_args.begin(), instruction.address_args.end());
        }
        const std::size_t key_width =
            arguments.empty() ? std::string_view {"trampoline"}.size() : std::string_view {"trampoline_args"}.size();
        const auto field = [key_width](std::string_view key)
        {
            return std::format("{:<{}} = ", key, key_width);
        };

        std::string out;
        out += std::format("-- Hook for \"{}\" at {}, generated by the disassembly view.\n",
                           target.covered.front().text,
                           target.description);
        out += "-- activate() finds the instruction by its AoB pattern (it must match exactly once),\n";
        out += "-- maps a cave near the site and writes your code plus the instructions the hook\n";
        out += "-- replaces into it; deactivate() restores the original bytes and frees the cave.\n\n";

        out += "local kHook = {\n";
        out += std::format("    {}{},\n", field("site_name"), quote(site_name));
        out += std::format("    {}{},\n", field("cave_name"), quote(cave_name));
        if (in_module)
        {
            out += std::format("    {}{},\n", field("module"), quote(target.module_name));
        }
        out += std::format("    {}{},\n", field("pattern"), quote(target.pattern));
        out += std::format(
            "    {}{},   -- the window's offset inside the match\n", field("offset"), target.pattern_offset);
        out += "    " + field("original") + "string.char(";
        for (std::size_t index = 0; index < target.original.size(); ++index)
        {
            if (index != 0)
            {
                out += ", ";
            }
            out += std::format("0x{:02X}", std::to_integer<unsigned>(target.original[index]));
        }
        out += "),\n";
        out += "    " + field("code") + "[[\n";
        out += "        nop    ; TODO: replace with your code\n";
        out += "    ]],\n";
        out += "    " + field("trampoline") + "[[\n";
        for (const std::string& line : target.trampoline_lines)
        {
            out += std::format("        {}\n", line);
        }
        out += "    ]],\n";
        if (!arguments.empty())
        {
            out += "    " + field("trampoline_args") + "{ ";
            for (std::size_t index = 0; index < arguments.size(); ++index)
            {
                if (index != 0)
                {
                    out += ", ";
                }
                const std::int64_t argument = arguments[index];
                out += argument < 0 ? std::format("-0x{:X}", static_cast<std::uint64_t>(-argument))
                                    : std::format("0x{:X}", static_cast<std::uint64_t>(argument));
            }
            out += " },\n";
        }
        out += "}\n\n";

        out += "function activate()\n";
        out += "    return hook.install(kHook)\n";
        out += "end\n\n";
        out += "function deactivate()\n";
        out += "    return hook.remove(kHook)\n";
        out += "end\n";

        return out;
    }
} // namespace slopkit::script
