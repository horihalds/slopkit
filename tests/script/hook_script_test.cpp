#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "disasm/decoder.hpp"
#include "script/engine.hpp"
#include "script/hook_script.hpp"
#include "support/script_helpers.hpp"

namespace
{
    using namespace slopkit;

    // A decoded listing over an owned byte buffer, built the way the document
    // hands one to the generator. The rows point into `code` and `instructions`,
    // so a listing must not be moved after it is built.
    struct Listing
    {
        std::vector<std::byte>           code;
        std::vector<disasm::Instruction> instructions;
        std::vector<script::HookRow>     rows;

        Listing(std::initializer_list<int> bytes,
                std::uint64_t              base,
                disasm::MachineMode        mode = disasm::MachineMode::long_64)
            : code(bytes.size())
        {
            std::size_t index = 0;
            for (const int value : bytes)
            {
                code[index++] = static_cast<std::byte>(value);
            }

            instructions = disasm::decode_block(code, base, code.size(), mode);
            rows.reserve(instructions.size());
            for (const disasm::Instruction& instruction : instructions)
            {
                script::HookRow row;
                row.address = instruction.address;
                row.bytes = std::span<const std::byte>(code.data() + (instruction.address - base), instruction.length);
                row.address_bytes = instruction.address_bytes;
                row.addresses     = instruction.addresses;
                row.text          = instruction.text;
                row.valid         = instruction.valid && row.bytes.size() == instruction.length;
                rows.push_back(std::move(row));
            }
        }
    };

    script::HookCandidate candidate_at(const Listing& listing, std::size_t index, std::string module = "game.exe")
    {
        script::HookCandidate candidate;
        candidate.rows            = listing.rows;
        candidate.selected        = index;
        candidate.module_name     = module;
        candidate.module_rva_text = "1A2B40";
        candidate.description     = module.empty() ? "7F3A1B2C" : module + "+" + candidate.module_rva_text;
        return candidate;
    }

    // How many byte tokens a rendered pattern holds.
    std::size_t pattern_length(const std::string& pattern)
    {
        return static_cast<std::size_t>(std::count(pattern.begin(), pattern.end(), ' ')) + 1;
    }

    // `std::byte` has no implicit conversion from `int`, so a literal list has
    // to be cast one element at a time.
    std::vector<std::byte> byte_list(std::initializer_list<int> values)
    {
        std::vector<std::byte> bytes;
        bytes.reserve(values.size());
        for (const int value : values)
        {
            bytes.push_back(static_cast<std::byte>(value));
        }
        return bytes;
    }
} // namespace

TEST_CASE("pattern_text wildcards exactly the given byte ranges", "[script][hook]")
{
    const std::vector<std::byte>            bytes = byte_list({0x48, 0x8B, 0x05, 0xF7, 0x02, 0x00, 0x00});
    const std::vector<disasm::AddressBytes> wildcards {
        {3, 4, false}
    };

    CHECK(script::pattern_text(bytes, wildcards) == "48 8B 05 ?? ?? ?? ??");
}

TEST_CASE("pattern_text never wildcards a byte outside a field", "[script][hook]")
{
    const std::vector<std::byte> bytes = byte_list({0x00, 0x00, 0xFF});

    CHECK(script::pattern_text(bytes, {}) == "00 00 FF");
    CHECK(script::pattern_text(bytes,
                               std::vector<disasm::AddressBytes> {
                                   {0, 1, false}
    })
          == "?? 00 FF");
}

TEST_CASE("the hook window covers whole instructions until a near jump fits", "[script][hook]")
{
    // SUB RSP, 0x28 (4 bytes); MOV RAX, RBX (3); ADD RSP, 0x28 (4); RET (1).
    const Listing listing({0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x83, 0xC4,
                           0x28, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                          0x1000);
    REQUIRE(listing.rows.size() >= 4);

    std::string              reason;
    const script::HookTarget target = *script::build_hook(candidate_at(listing, 0), reason);

    CHECK(target.instruction_length == 4);
    CHECK(target.original.size() == 7); // the 4-byte instruction and its 3-byte successor
    CHECK(target.covered.size() == 2);
    CHECK(target.covered[0].address == 0x1000);
    CHECK(target.covered[1].address == 0x1004);
}

TEST_CASE("a single instruction long enough for a jump stands alone", "[script][hook]")
{
    // MOV RAX, 0x1122334455667788 (10 bytes), then RET.
    const Listing listing({0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
                           0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                          0x1000);
    REQUIRE(listing.rows.size() >= 2);

    std::string              reason;
    const script::HookTarget target = *script::build_hook(candidate_at(listing, 0), reason);

    CHECK(target.original.size() == 10);
    CHECK(target.covered.size() == 1);
}

TEST_CASE("a window that would leave the decoded bytes is refused", "[script][hook]")
{
    // A lone 4-byte instruction has no successor to reach five bytes.
    const Listing listing({0x48, 0x83, 0xEC, 0x28}, 0x1000);

    std::string reason;
    CHECK_FALSE(script::build_hook(candidate_at(listing, 0), reason).has_value());
    CHECK(reason.find("runs past") != std::string::npos);
}

TEST_CASE("a row that is not a decoded instruction is refused", "[script][hook]")
{
    const Listing listing({0x06, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                           0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                          0x1000);
    REQUIRE_FALSE(listing.rows.empty());
    REQUIRE_FALSE(listing.rows.front().valid);

    std::string reason;
    CHECK_FALSE(script::build_hook(candidate_at(listing, 0), reason).has_value());
    CHECK(reason.find("not a decoded instruction") != std::string::npos);
}

TEST_CASE("the pattern starts at the window and reaches at least sixteen bytes", "[script][hook]")
{
    const Listing listing({0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x83, 0xC4,
                           0x28, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                          0x1000);

    std::string              reason;
    const script::HookTarget target = *script::build_hook(candidate_at(listing, 0), reason);

    // The window is 7 bytes; the span adds the two following instructions and
    // keeps widening until the signature is long enough.
    CHECK(target.pattern.rfind("48 83 EC 28 48 89 D8 48 83 C4 28 C3", 0) == 0);
    CHECK(target.pattern.find("??") == std::string::npos);
    CHECK(pattern_length(target.pattern) >= 16);
    CHECK(target.pattern_offset == 0);
}

TEST_CASE("only absolute address fields are wildcarded", "[script][hook]")
{
    const Listing listing(
        {0x48, 0x8B, 0x05, 0xF7, 0x02, 0x00, 0x00,       // MOV RAX, [rip+2F7]: the displacement stays literal
         0x48, 0x83, 0xEC, 0x28,                         // SUB RSP, 0x28
         0x48, 0x8B, 0x04, 0x25, 0x00, 0x20, 0x40, 0x00, // MOV RAX, [00402000]: the absolute is wildcarded
         0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
        0x1000);

    std::string              reason;
    const script::HookTarget target = *script::build_hook(candidate_at(listing, 0), reason);

    const std::size_t wildcard = target.pattern.find("??");
    REQUIRE(wildcard != std::string::npos);
    // The literal rip-relative displacement sits before the first wildcard.
    CHECK(target.pattern.substr(0, wildcard).find("F7") != std::string::npos);
    // Four wildcard bytes for the absolute disp32, and no others.
    CHECK(target.pattern.substr(wildcard) == "?? ?? ?? ??");
}

TEST_CASE("address operands are re-emitted position-independently", "[script][hook]")
{
    SECTION("an absolute memory operand yields a placeholder and a positive delta")
    {
        // MOV RAX, [00402000] at 0x1000, then filler for the window.
        const Listing listing({0x48, 0x8B, 0x04, 0x25, 0x00, 0x20, 0x40, 0x00, 0x90, 0x90,
                               0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                              0x1000);

        std::string              reason;
        const script::HookTarget target = *script::build_hook(candidate_at(listing, 0), reason);

        REQUIRE(target.trampoline_lines.size() == 1);
        CHECK(target.trampoline_lines[0] == "MOV RAX, [0x%X]");
        REQUIRE(target.covered[0].address_args.size() == 1);
        CHECK(target.covered[0].address_args[0] == 0x401000);
    }

    SECTION("an immediate is a value and is copied verbatim")
    {
        // MOV RAX, 1122334455667788 at 0x1000: the decoder prints it as a value,
        // not an address, so the trampoline keeps it as it is.
        const Listing listing({0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
                               0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                              0x1000);

        std::string              reason;
        const script::HookTarget target = *script::build_hook(candidate_at(listing, 0), reason);

        REQUIRE(target.trampoline_lines.size() == 1);
        CHECK(target.trampoline_lines[0] == "MOV RAX, 1122334455667788");
        CHECK(target.covered[0].address_args.empty());
    }

    SECTION("a rip-relative operand uses the absolute the decoder computed")
    {
        const Listing listing({0x48, 0x89, 0xD8,                         // MOV RAX, RBX (3) - copied verbatim
                               0x48, 0x8B, 0x05, 0xF7, 0x02, 0x00, 0x00, // MOV RAX, [rip+2F7] at 0x1003 -> 0x1301
                               0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                               0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                              0x1000);

        std::string              reason;
        const script::HookTarget target = *script::build_hook(candidate_at(listing, 0), reason);

        CHECK(target.trampoline_lines[0] == "MOV RAX, RBX");
        CHECK(target.covered[0].address_args.empty());
        CHECK(target.trampoline_lines[1] == "MOV RAX, [0x%X]");
        REQUIRE(target.covered[1].address_args.size() == 1);
        CHECK(target.covered[1].address_args[0] == 0x301);
    }

    SECTION("a backward branch yields a negative delta")
    {
        // JMP 0x0F00 at 0x1000 (rel32 0xFFFFFEFB), then filler for the window.
        const Listing listing({0xE9, 0xFB, 0xFE, 0xFF, 0xFF, 0x90, 0x90, 0x90, 0x90, 0x90,
                               0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                              0x1000);

        std::string              reason;
        const script::HookTarget target = *script::build_hook(candidate_at(listing, 0), reason);

        CHECK(target.trampoline_lines[0] == "JMP 0x%X");
        REQUIRE(target.covered[0].address_args.size() == 1);
        CHECK(target.covered[0].address_args[0] == -0x100);
    }
}

TEST_CASE("a trampoline that does not re-encode is refused", "[script][hook]")
{
    const Listing listing({0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0xC3, 0x90, 0x90,
                           0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                          0x1000);

    std::vector<script::HookRow> rows = listing.rows;
    rows[1].text                      = "not_an_instruction";

    script::HookCandidate candidate;
    candidate.rows     = rows;
    candidate.selected = 0;

    std::string reason;
    CHECK_FALSE(script::build_hook(candidate, reason).has_value());
    CHECK(reason.find("cannot be re-encoded") != std::string::npos);
}

TEST_CASE("the rendered script carries the generated facts", "[script][hook]")
{
    const Listing listing({0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x83, 0xC4,
                           0x28, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                          0x1000);

    std::string              reason;
    const script::HookTarget target = *script::build_hook(candidate_at(listing, 0), reason);
    const std::string        source = script::render(target);

    // The facts table.
    CHECK(source.find("local kHook = {") != std::string::npos);
    CHECK(source.find("\"hook_site_1a2b40\"") != std::string::npos);
    CHECK(source.find("\"hook_cave_1a2b40\"") != std::string::npos);
    CHECK(source.find("\"game.exe\"") != std::string::npos);
    CHECK(source.find("\"" + target.pattern + "\"") != std::string::npos);
    CHECK(source.find("string.char(0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8)") != std::string::npos);
    CHECK(source.find("SUB RSP, 28") != std::string::npos);
    CHECK(source.find("MOV RAX, RBX") != std::string::npos);

    // The two one-line lifecycle hooks, with the install mechanics moved into
    // the runtime.
    CHECK(source.find("function activate()\n    return hook.install(kHook)\nend") != std::string::npos);
    CHECK(source.find("function deactivate()\n    return hook.remove(kHook)\nend") != std::string::npos);
    CHECK(source.find("aobscan") == std::string::npos);
    CHECK(source.find("dealloc") == std::string::npos);
    CHECK(script::check_syntax(source).has_value());
}

TEST_CASE("the rendered script lists the trampoline arguments", "[script][hook]")
{
    // MOV RAX, [00402000] at 0x1000, then filler for the window.
    const Listing listing({0x48, 0x8B, 0x04, 0x25, 0x00, 0x20, 0x40, 0x00, 0x90, 0x90,
                           0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                          0x1000);

    std::string              reason;
    const script::HookTarget target = *script::build_hook(candidate_at(listing, 0), reason);
    const std::string        source = script::render(target);

    CHECK(source.find("trampoline_args") != std::string::npos);
    CHECK(source.find("{ 0x401000 }") != std::string::npos);
    CHECK(script::check_syntax(source).has_value());
}

TEST_CASE("a module-less target scans everywhere and drops the module text", "[script][hook]")
{
    const Listing listing({0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x83, 0xC4,
                           0x28, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                          0x7F3A1B2C);

    std::string              reason;
    const script::HookTarget target = *script::build_hook(candidate_at(listing, 0, ""), reason);
    const std::string        source = script::render(target);

    CHECK(source.find("module") == std::string::npos);
    CHECK(source.find("\"hook_site_7f3a1b2c\"") != std::string::npos);
    CHECK(script::check_syntax(source).has_value());
}

TEST_CASE("the generated script activates and deactivates against a target", "[script][hook]")
{
    ScriptFixture fixture(0x200);
    // The bytes the listing decodes and the bytes the target holds have to be
    // the same, or the generated pattern would not match at activation.
    fixture.fake.put(0x10, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x83, 0xC4,
                            0x28, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90});

    const Listing listing({0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x83, 0xC4,
                           0x28, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                          fixture.fake.address(0x10));
    REQUIRE(listing.rows.size() >= 4);

    script::HookCandidate candidate;
    candidate.rows            = listing.rows;
    candidate.selected        = 0;
    candidate.module_name     = "test.so";
    candidate.module_rva_text = "10";
    candidate.description     = "test.so+10";

    std::string                             reason;
    const std::optional<script::HookTarget> target = script::build_hook(candidate, reason);
    REQUIRE(target.has_value());
    const std::string source = script::render(*target);
    REQUIRE(script::check_syntax(source).has_value());

    const std::uint64_t site = fixture.fake.address(0x10);

    const script::LifecycleResult activated = fixture.engine.run_lifecycle(source, "activate");
    INFO(activated.message);
    REQUIRE(activated.ok);
    CHECK(activated.message.empty());

    // Exactly one cave was mapped, near the site, sized from the payload
    // (1 + 7 + 5 rounded up).
    REQUIRE(fixture.fake.allocations.size() == 1);
    const std::uint64_t cave = fixture.fake.allocations.begin()->first;
    CHECK(fixture.fake.allocations.begin()->second == 16);
    REQUIRE_FALSE(fixture.fake.allocate_requests.empty());
    CHECK(fixture.fake.allocate_requests.front().first == site);
    CHECK(fixture.fake.write_count == 2);

    // The site holds a near jump into the cave and the payload is NOP-padded.
    const auto site_offset = static_cast<std::size_t>(site - kScriptBase);
    CHECK(fixture.fake.bytes[site_offset] == std::byte {0xE9});
    for (std::size_t offset = 5; offset < target->original.size(); ++offset)
    {
        CHECK(fixture.fake.bytes[site_offset + offset] == std::byte {0x90});
    }

    // The cave holds the stub, the re-encoded instructions and the jump back.
    const auto cave_offset = static_cast<std::size_t>(cave - kScriptBase);
    CHECK(fixture.fake.bytes[cave_offset] == std::byte {0x90});     // the `nop` stub
    CHECK(fixture.fake.bytes[cave_offset + 1] == std::byte {0x48}); // SUB RSP, 0x28
    CHECK(fixture.fake.bytes[cave_offset + 5] == std::byte {0x48}); // MOV RAX, RBX
    CHECK(fixture.fake.bytes[cave_offset + 8] == std::byte {0xE9}); // the jump back

    const script::LifecycleResult deactivated = fixture.engine.run_lifecycle(source, "deactivate");
    REQUIRE(deactivated.ok);
    CHECK(deactivated.message.empty());

    // The original bytes are back and the mapping is gone.
    for (std::size_t offset = 0; offset < target->original.size(); ++offset)
    {
        CHECK(fixture.fake.bytes[site_offset + offset] == target->original[offset]);
    }
    CHECK(fixture.fake.allocations.empty());
}

TEST_CASE("deactivate restores the hooked site, not the pattern match", "[script][hook]")
{
    // A hook on a row with decoded neighbours on both sides: the window starts
    // seven bytes into the match, so restoring at the match address would
    // corrupt the bytes before the site and leave the jump behind.
    const std::initializer_list<int> bytes = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x83, 0xC4,
                                              0x28, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

    const auto restores_at_the_site = [&bytes](const std::string& module)
    {
        ScriptFixture fixture(0x200);
        fixture.fake.put(0x10, bytes);

        constexpr std::size_t        base_offset  = 0x10;
        constexpr std::size_t        listing_size = 20;
        const std::vector<std::byte> before(fixture.fake.bytes.begin() + static_cast<std::ptrdiff_t>(base_offset),
                                            fixture.fake.bytes.begin()
                                                + static_cast<std::ptrdiff_t>(base_offset + listing_size));

        const Listing listing(bytes, fixture.fake.address(0x10));
        REQUIRE(listing.rows.size() >= 4);

        script::HookCandidate candidate = candidate_at(listing, 2, module);

        std::string                             reason;
        const std::optional<script::HookTarget> target = script::build_hook(candidate, reason);
        REQUIRE(target.has_value());
        // The premise the old template got wrong: the match is not the site.
        REQUIRE(target->pattern_offset > 0);

        const std::string source = script::render(*target);
        REQUIRE(script::check_syntax(source).has_value());

        const std::uint64_t match       = fixture.fake.address(0x10);
        const std::uint64_t site        = match + target->pattern_offset;
        const auto          site_offset = static_cast<std::size_t>(site - kScriptBase);

        const script::LifecycleResult activated = fixture.engine.run_lifecycle(source, "activate");
        INFO(activated.message);
        REQUIRE(activated.ok);

        // The jump sits at the site and the match keeps its first byte.
        CHECK(fixture.fake.bytes[site_offset] == std::byte {0xE9});
        CHECK(fixture.fake.bytes[base_offset] == std::byte {0x48});
        for (std::size_t offset = 5; offset < target->original.size(); ++offset)
        {
            CHECK(fixture.fake.bytes[site_offset + offset] == std::byte {0x90});
        }
        REQUIRE(fixture.fake.allocations.size() == 1);
        REQUIRE_FALSE(fixture.fake.allocate_requests.empty());
        CHECK(fixture.fake.allocate_requests.front().first == site);

        const script::LifecycleResult deactivated = fixture.engine.run_lifecycle(source, "deactivate");
        INFO(deactivated.message);
        REQUIRE(deactivated.ok);
        // The whole listing region is byte-identical to the snapshot: the write
        // landed on the window, not on the bytes between the match and the site.
        for (std::size_t index = 0; index < before.size(); ++index)
        {
            CHECK(fixture.fake.bytes[base_offset + index] == before[index]);
        }
        CHECK(fixture.fake.allocations.empty());
    };

    SECTION("a module-qualified target")
    {
        restores_at_the_site("test.so");
    }

    SECTION("a module-less target")
    {
        restores_at_the_site("");
    }
}

TEST_CASE("a refused activation frees the cave and drops the site label", "[script][hook]")
{
    ScriptFixture fixture(0x200);
    fixture.fake.put(0x10, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x83, 0xC4,
                            0x28, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90});

    const Listing listing({0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x83, 0xC4,
                           0x28, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                          fixture.fake.address(0x10));
    REQUIRE(listing.rows.size() >= 4);

    script::HookCandidate candidate = candidate_at(listing, 0, "test.so");

    std::string        reason;
    script::HookTarget target = *script::build_hook(candidate, reason);
    // The trampoline is copied verbatim into the script, so an instruction the
    // assembler cannot encode makes the helper refuse before it maps or patches
    // anything.
    REQUIRE_FALSE(target.trampoline_lines.empty());
    target.trampoline_lines.front() = "frobnicate";

    const std::string source = script::render(target);

    const script::LifecycleResult activated = fixture.engine.run_lifecycle(source, "activate");
    INFO(activated.message);
    CHECK_FALSE(activated.ok);
    CHECK(activated.message.find("the trampoline:") != std::string::npos);
    // The cleanup: no mapping is left behind and the site was never touched.
    CHECK(fixture.fake.allocations.empty());
    CHECK(fixture.fake.write_count == 0);

    const script::LifecycleResult deactivated = fixture.engine.run_lifecycle(source, "deactivate");
    INFO(deactivated.message);
    CHECK_FALSE(deactivated.ok);
    CHECK(deactivated.message.find("this hook's site is not known anymore") != std::string::npos);
}

TEST_CASE("activate refuses a cave that is out of the site's reach", "[script][hook]")
{
    ScriptFixture fixture(0x200);
    fixture.fake.put(0x10, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x83, 0xC4,
                            0x28, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90});

    const Listing listing({0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x83, 0xC4,
                           0x28, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                          fixture.fake.address(0x10));
    REQUIRE(listing.rows.size() >= 4);

    script::HookCandidate candidate;
    candidate.rows            = listing.rows;
    candidate.selected        = 0;
    candidate.module_name     = "test.so";
    candidate.module_rva_text = "10";
    candidate.description     = "test.so+10";

    std::string                             reason;
    const std::optional<script::HookTarget> target = script::build_hook(candidate, reason);
    REQUIRE(target.has_value());

    // Force the cave tens of TB away, past the ±2 GB a near jump reaches.
    fixture.fake.forced_allocation = 0x7F0000000000ull;

    const std::string   source = script::render(*target);
    // The same chunk string keeps the labels across the two runs.
    const std::string   probe  = source + "\nfunction cave_still_labeled() return label(kCaveName) ~= 0 end\n"
                               + "function site_still_labeled() return label(kSiteName) ~= 0 end\n";
    const std::uint64_t site   = fixture.fake.address(0x10);

    const script::LifecycleResult activated = fixture.engine.run_lifecycle(probe, "activate");
    INFO(activated.message);
    CHECK_FALSE(activated.ok);
    CHECK(activated.message.find("out of the site's jump reach") != std::string::npos);

    // The site still holds its original bytes and the cave was freed.
    const auto site_offset = static_cast<std::size_t>(site - kScriptBase);
    for (std::size_t offset = 0; offset < target->original.size(); ++offset)
    {
        CHECK(fixture.fake.bytes[site_offset + offset] == target->original[offset]);
    }
    CHECK(fixture.fake.allocations.empty());

    // Both labels are gone: a probe reports `true` only while it resolves.
    const script::LifecycleResult cave_labeled = fixture.engine.run_lifecycle(probe, "cave_still_labeled");
    CHECK_FALSE(cave_labeled.ok);
    const script::LifecycleResult site_labeled = fixture.engine.run_lifecycle(probe, "site_still_labeled");
    CHECK_FALSE(site_labeled.ok);
}

TEST_CASE("a hook script saved in the old shape still activates and deactivates", "[script][hook]")
{
    ScriptFixture fixture(0x200);
    fixture.fake.put(0x10,
                     {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90});

    // The self-installing shape a `.skt` hook script written before the `hook`
    // helper looks like: every global it used is still there, unchanged.
    const std::string source = R"(
local kPattern  = "48 83 EC 28 48 89 D8 48"
local kOriginal = string.char(0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48)
local kSiteName = "hook_site_legacy"
local kCaveName = "hook_cave_legacy"

function activate()
    local found, site = aobscan(kSiteName, kPattern)
    if not found then
        return false, "the hook pattern was not found"
    end
    local cave = alloc(kCaveName, 0x40, site)
    local ok, hook_len = assemble(cave, "nop")
    if not ok then
        dealloc(kCaveName)
        ulabel(kCaveName)
        ulabel(kSiteName)
        return false, "the hook code: " .. hook_len
    end
    local ok2 = assemble(site, "jmp 0x%X", cave)
    if not ok2 then
        dealloc(kCaveName)
        ulabel(kCaveName)
        ulabel(kSiteName)
        return false, "the jump to the cave"
    end
    return true
end

function deactivate()
    local site = label(kSiteName)
    local cave = label(kCaveName)
    if site == 0 then
        return false, "this hook's site is not known anymore"
    end
    mem.write_bytes(site, kOriginal)
    if cave ~= 0 then
        dealloc(kCaveName)
        ulabel(kCaveName)
    end
    ulabel(kSiteName)
    return true
end
)";

    const script::LifecycleResult activated = fixture.engine.run_lifecycle(source, "activate");
    INFO(activated.message);
    REQUIRE(activated.ok);
    CHECK(fixture.fake.allocations.size() == 1);

    const script::LifecycleResult deactivated = fixture.engine.run_lifecycle(source, "deactivate");
    INFO(deactivated.message);
    REQUIRE(deactivated.ok);
    CHECK(fixture.fake.allocations.empty());
}
