#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <ranges>
#include <string_view>
#include <vector>

#include "support/memory_view_helpers.hpp"
#include "ui/components/disassembly_document.hpp"

namespace
{
    using slopkit::ui::LiveReading;
    using slopkit::ui::LiveRequest;
    using slopkit::ui::components::CodePatch;
    using slopkit::ui::components::CodePatchTable;
    using slopkit::ui::components::CopyFormat;
    using slopkit::ui::components::DisassemblyDocument;

    // A code window at an 8 KiB boundary, so the cursor sits at its start.
    constexpr std::uint64_t kCode = 0x2000;

    struct DocFixture
    {
        FakeAccess                       access;
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target = attached_target();
        CodePatchTable                   patches;
        DisassemblyDocument              document {worker, target, patches};

        explicit DocFixture(std::uint64_t first = kCode, std::size_t rows = 8)
        {
            attach_session(worker);
            document.set_visible(true);
            document.set_view(first, rows);
        }

        void put(std::uint64_t address, std::initializer_list<unsigned> values)
        {
            std::size_t index = 0;
            for (const unsigned value : values)
            {
                access.memory->bytes[address + index] = static_cast<std::byte>(value);
                ++index;
            }
        }

        void pass()
        {
            const auto               requests = document.next_live_request();
            std::vector<LiveReading> readings;
            readings.reserve(requests.size());
            for (const LiveRequest& request : requests)
            {
                readings.push_back(reading(request.id, *access.memory, request.address, request.size));
            }
            document.apply_live_readings(readings);
        }
    };

    [[nodiscard]] bool logged(std::span<const slopkit::log::Record> records, std::string_view needle)
    {
        return std::ranges::any_of(records,
                                   [needle](const slopkit::log::Record& record)
                                   {
                                       return record.message.find(needle) != std::string::npos;
                                   });
    }
} // namespace

TEST_CASE("the disassembly document requests one aligned window", "[ui]")
{
    DocFixture fixture;

    const auto requests = fixture.document.next_live_request();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].id == DisassemblyDocument::kIdBase);
    CHECK(requests[0].address == kCode);
    CHECK(requests[0].size == DisassemblyDocument::kWindowSize);
    CHECK(fixture.document.window_base() == kCode);

    // A cursor inside the window does not move the request base.
    fixture.document.set_view(kCode + 0x1234, 8);
    CHECK(fixture.document.window_base() == kCode);
    const auto moved = fixture.document.next_live_request();
    REQUIRE(moved.size() == 1);
    CHECK(moved[0].address == kCode);
}

TEST_CASE("the disassembly document asks for nothing while hidden or detached", "[ui]")
{
    DocFixture fixture;

    fixture.document.set_visible(false);
    CHECK(fixture.document.next_live_request().empty());

    fixture.document.set_visible(true);
    fixture.target.session_live = false;
    CHECK(fixture.document.next_live_request().empty());
}

TEST_CASE("the disassembly document decodes rows from the window base", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x55, 0x48, 0x89, 0xE5, 0xC3});
    fixture.pass();
    fixture.document.ensure_rows(3);

    REQUIRE(fixture.document.row_count() >= 3);
    const auto push = fixture.document.row(0);
    CHECK(push.address == kCode);
    CHECK(push.readable);
    CHECK(push.bytes == QStringLiteral("55"));
    CHECK(push.text == QStringLiteral("PUSH RBP"));

    const auto mov = fixture.document.row(1);
    CHECK(mov.address == kCode + 1);
    CHECK(mov.bytes == QStringLiteral("48 89 E5"));
    CHECK(mov.text == QStringLiteral("MOV RBP, RSP"));

    const auto ret = fixture.document.row(2);
    CHECK(ret.address == kCode + 4);
    CHECK(ret.text == QStringLiteral("RET"));

    // The widest decoded instruction drives the byte column width.
    CHECK(fixture.document.bytes_width() == 8);
}

TEST_CASE("the disassembly document wraps a row's bytes into lines", "[ui]")
{
    DocFixture fixture;
    // 15 bytes: redundant operand-size prefixes in front of `MOV RAX, imm64`.
    fixture.put(kCode, {0x66, 0x66, 0x66, 0x66, 0x66, 0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11});
    fixture.pass();
    fixture.document.ensure_rows(1);

    REQUIRE(fixture.document.row_count() == 1);
    const auto    row   = fixture.document.row(0);
    const QString bytes = row.bytes;
    REQUIRE(row.length == 15);
    CHECK(bytes == QStringLiteral("66 66 66 66 66 48 B8 88 77 66 55 44 33 22 11"));

    // One byte per line: every token gets its own line, none is split.
    CHECK(fixture.document.line_count(0, 1) == 15);
    CHECK(fixture.document.byte_line(0, 0, 1) == QStringLiteral("66"));
    CHECK(fixture.document.byte_line(0, 14, 1) == QStringLiteral("11"));
    CHECK(fixture.document.byte_line(0, 15, 1).isEmpty());

    // Eight per line: a full first line and the remainder.
    CHECK(fixture.document.line_count(0, 8) == 2);
    CHECK(fixture.document.byte_line(0, 0, 8) == QStringLiteral("66 66 66 66 66 48 B8 88"));
    CHECK(fixture.document.byte_line(0, 1, 8) == QStringLiteral("77 66 55 44 33 22 11"));

    // Four per line: 4, 4, 4, 3 with no padding of the last line.
    CHECK(fixture.document.line_count(0, 4) == 4);
    CHECK(fixture.document.byte_line(0, 0, 4) == QStringLiteral("66 66 66 66"));
    CHECK(fixture.document.byte_line(0, 3, 4) == QStringLiteral("33 22 11"));
    CHECK(fixture.document.byte_line(0, 4, 4).isEmpty());

    // The natural width fits the row on a single line.
    CHECK(fixture.document.line_count(0, 15) == 1);
    CHECK(fixture.document.byte_line(0, 0, 15) == bytes);

    // `per_line == 0` behaves as one, so the column is never empty.
    CHECK(fixture.document.line_count(0, 0) == 15);
    CHECK(fixture.document.byte_line(0, 0, 0) == QStringLiteral("66"));

    // Out-of-range rows and lines hand out nothing.
    CHECK(fixture.document.line_count(1, 4) == 0);
    CHECK(fixture.document.byte_line(1, 0, 4).isEmpty());
}

TEST_CASE("the disassembly document wraps a `??` and a `.byte` row on one line", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0xC3, 0x06});
    fixture.pass();
    fixture.document.ensure_rows(2);

    REQUIRE(fixture.document.row_count() == 2);
    CHECK(fixture.document.row(1).text == QStringLiteral(".byte 06"));
    CHECK(fixture.document.line_count(1, 1) == 1);
    CHECK(fixture.document.line_count(1, 15) == 1);
    CHECK(fixture.document.byte_line(1, 0, 1) == QStringLiteral("06"));
    CHECK(fixture.document.byte_line(1, 1, 1).isEmpty());

    // A refused window masks the rows as `??`, one line each again.
    [[maybe_unused]] const auto submitted = fixture.document.next_live_request();
    LiveReading                 failed;
    failed.id       = DisassemblyDocument::kIdBase;
    failed.readable = false;
    fixture.document.apply_live_readings(std::vector<LiveReading> {failed});
    REQUIRE(fixture.document.row_count() == 2);
    CHECK(fixture.document.line_count(0, 1) == 1);
    CHECK(fixture.document.line_count(0, 15) == 1);
    CHECK(fixture.document.byte_line(0, 0, 1) == QStringLiteral("??"));
    CHECK(fixture.document.byte_line(0, 1, 1).isEmpty());
}

TEST_CASE("the disassembly document steps its window by aligned pages", "[ui]")
{
    DocFixture fixture;
    // Two adjacent windows of NOPs, so a full sweep consumes each 8 KiB page.
    fill(*fixture.access.memory, kCode, DisassemblyDocument::kWindowSize, std::byte {0x90});
    fill(*fixture.access.memory,
         kCode + DisassemblyDocument::kWindowSize,
         DisassemblyDocument::kWindowSize,
         std::byte {0x90});

    // The live window is the cursor's 8 KiB page, a multiple of 4096.
    const auto requests = fixture.document.next_live_request();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].address == kCode);
    CHECK(requests[0].address % 4096 == 0);
    CHECK(fixture.document.window_base() == kCode);

    fixture.pass();
    fixture.document.ensure_rows(std::numeric_limits<std::size_t>::max());
    CHECK(fixture.document.row_count() == DisassemblyDocument::kWindowSize);
    CHECK(fixture.document.window_exhausted());

    const std::uint64_t next    = kCode + DisassemblyDocument::kWindowSize;
    int                 changes = 0;
    QObject::connect(&fixture.document,
                     &DisassemblyDocument::rowsChanged,
                     [&changes]
                     {
                         ++changes;
                     });

    // A whole-window step keeps every base a multiple of 4096.
    fixture.document.set_view(next, 8);
    CHECK(fixture.document.window_base() == next);
    CHECK(fixture.document.window_base() % 4096 == 0);
    CHECK(fixture.document.window_exhausted() == false); // the new payload is in flight

    // A byte-identical (all-NOP) window is still re-decoded, keyed on the base.
    fixture.pass();
    fixture.document.ensure_rows(1);
    CHECK(changes == 1);
    CHECK(fixture.document.row(0).address == next);

    // Stepping back up returns to the previous aligned base.
    fixture.document.set_view(kCode, 8);
    CHECK(fixture.document.window_base() == kCode);
    fixture.pass();
    fixture.document.ensure_rows(1);
    CHECK(changes == 2);
    CHECK(fixture.document.row(0).address == kCode);
}

TEST_CASE("the disassembly document invalidates the decode only when the bytes change", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x55, 0xC3});
    fixture.pass();
    fixture.document.ensure_rows(2);
    REQUIRE(fixture.document.row_count() == 2);

    int changes = 0;
    QObject::connect(&fixture.document,
                     &DisassemblyDocument::rowsChanged,
                     [&]
                     {
                         ++changes;
                     });

    // An identical window is neither re-decoded nor repainted.
    fixture.pass();
    CHECK(changes == 0);
    CHECK(fixture.document.row_count() == 2);

    // One byte moves: the stream is re-decoded from the window base.
    fixture.access.memory->bytes[kCode] = std::byte {0x90};
    fixture.pass();
    CHECK(changes == 1);
    fixture.document.ensure_rows(1);
    CHECK(fixture.document.row(0).text == QStringLiteral("NOP"));
}

TEST_CASE("the disassembly document drops stale and foreign readings", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x55});

    // The window moves before the pass lands.
    [[maybe_unused]] const auto submitted = fixture.document.next_live_request();
    fixture.document.set_view(kCode + DisassemblyDocument::kWindowSize, 8);
    fixture.document.apply_live_readings(
        std::vector<LiveReading> {reading(DisassemblyDocument::kIdBase, *fixture.access.memory, kCode, 16)});
    CHECK(fixture.document.row_count() == 0);

    // A reading from another target is dropped as well.
    fixture.document.set_view(kCode, 8);
    [[maybe_unused]] const auto second = fixture.document.next_live_request();
    fixture.target.pid                 = 7;
    fixture.document.apply_live_readings(
        std::vector<LiveReading> {reading(DisassemblyDocument::kIdBase, *fixture.access.memory, kCode, 16)});
    CHECK(fixture.document.row_count() == 0);
    fixture.target.pid = 42;

    // A reading for another surface's id is ignored.
    [[maybe_unused]] const auto third = fixture.document.next_live_request();
    fixture.document.apply_live_readings(std::vector<LiveReading> {reading(0, *fixture.access.memory, kCode, 16)});
    CHECK(fixture.document.row_count() == 0);
}

TEST_CASE("the disassembly document masks a refused window as ??", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x55, 0xC3});
    fixture.pass();
    fixture.document.ensure_rows(2);
    REQUIRE(fixture.document.row_count() == 2);
    CHECK(fixture.document.row(0).readable);

    // A refused window keeps the rows but renders no instruction text.
    [[maybe_unused]] const auto submitted = fixture.document.next_live_request();
    LiveReading                 failed;
    failed.id       = DisassemblyDocument::kIdBase;
    failed.readable = false;
    fixture.document.apply_live_readings(std::vector<LiveReading> {failed});
    REQUIRE(fixture.document.row_count() == 2);
    CHECK_FALSE(fixture.document.row(0).readable);
    CHECK(fixture.document.row(0).bytes == QStringLiteral("??"));
    CHECK(fixture.document.row(0).text.isEmpty());

    // A later good pass restores the decoded text.
    fixture.pass();
    fixture.document.ensure_rows(2);
    CHECK(fixture.document.row(0).readable);
    CHECK(fixture.document.row(0).text == QStringLiteral("PUSH RBP"));
    CHECK(fixture.document.row(0).bytes == QStringLiteral("55"));
}

TEST_CASE("the disassembly document renders an undecodable byte as .byte", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0xC3, 0x06, 0xC3});
    fixture.pass();
    fixture.document.ensure_rows(3);

    REQUIRE(fixture.document.row_count() >= 3);
    CHECK(fixture.document.row(0).text == QStringLiteral("RET"));
    CHECK(fixture.document.row(1).bytes == QStringLiteral("06"));
    CHECK(fixture.document.row(1).text == QStringLiteral(".byte 06"));
    CHECK(fixture.document.row(2).text == QStringLiteral("RET"));
}

TEST_CASE("the disassembly document finds the row covering an address", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x48, 0x89, 0xE5});
    fixture.pass();
    fixture.document.ensure_rows(1);

    const auto inside = fixture.document.row_at(kCode + 2);
    REQUIRE(inside.has_value());
    CHECK(*inside == 0);
    CHECK(fixture.document.row_at(kCode).has_value());
    CHECK_FALSE(fixture.document.row_at(kCode - 1).has_value());
    CHECK_FALSE(fixture.document.row_at(kCode + 3).has_value()); // Past the only decoded row.
}

TEST_CASE("the disassembly document clears the decode when the machine mode changes", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, 0xC3});
    fixture.pass();
    fixture.document.ensure_rows(1);
    CHECK(fixture.document.row(0).length == 7);
    CHECK(fixture.document.row(0).text == QStringLiteral("MOV RAX, 01"));

    fixture.document.set_machine_mode(slopkit::disasm::MachineMode::legacy_32);
    fixture.document.ensure_rows(1);
    CHECK(fixture.document.row(0).length == 1);
    CHECK(fixture.document.row(0).text == QStringLiteral("DEC EAX"));
}

TEST_CASE("the disassembly document clamps the cursor at the user-space ceiling", "[ui]")
{
    DocFixture fixture;
    fixture.document.set_view(slopkit::scan::kMaxUserAddress, 4);

    CHECK(fixture.document.first_address() <= slopkit::scan::kMaxUserAddress);
    CHECK(fixture.document.window_base() <= slopkit::scan::kMaxUserAddress);

    const auto requests = fixture.document.next_live_request();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].address <= slopkit::scan::kMaxUserAddress);
    CHECK(requests[0].address + requests[0].size - 1 <= slopkit::scan::kMaxUserAddress);
}

TEST_CASE("the disassembly document renders addresses through the module map", "[ui]")
{
    application();

    DocFixture fixture;
    CHECK(fixture.document.address_text(kCode + 0x10) == QStringLiteral("0000000000002010"));

    fixture.document.set_modules({module_image("app", kCode, 0x1000)});
    CHECK(fixture.document.address_text(kCode) == QStringLiteral("app+0"));
    CHECK(fixture.document.address_text(kCode + 0x10) == QStringLiteral("app+10"));

    fixture.document.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(fixture.document.address_text(kCode + 0x10) == QStringLiteral("0000000000002010"));
}

TEST_CASE("the disassembly document renders every copy form of a row", "[ui]")
{
    application();

    DocFixture fixture;
    fixture.put(kCode, {0x55, 0x48, 0x89, 0xE5, 0xC3});
    fixture.pass();
    fixture.document.ensure_rows(2);
    fixture.document.set_modules({module_image("app", kCode, 0x1000)});

    const DisassemblyDocument& document = fixture.document;

    CHECK(document.copy_text(0, CopyFormat::module_relative) == QStringLiteral("app+0"));
    CHECK(document.copy_text(0, CopyFormat::absolute) == QStringLiteral("2000"));
    CHECK(document.copy_text(0, CopyFormat::bytes) == QStringLiteral("55"));
    CHECK(document.copy_text(0, CopyFormat::instruction) == QStringLiteral("PUSH RBP"));
    CHECK(document.copy_text(0, CopyFormat::address_and_bytes) == QStringLiteral("app+0: 55"));
    CHECK(document.copy_text(0, CopyFormat::address_and_instruction) == QStringLiteral("app+0: PUSH RBP"));
    CHECK(document.copy_text(0, CopyFormat::address_bytes_instruction) == QStringLiteral("app+0: 55  PUSH RBP"));

    // The combined forms follow the current display mode for the address.
    fixture.document.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(document.copy_text(0, CopyFormat::address_and_bytes) == QStringLiteral("0000000000002000: 55"));

    // Out-of-range rows hand out nothing.
    CHECK(document.copy_text(fixture.document.row_count(), CopyFormat::bytes).isEmpty());
}

TEST_CASE("the disassembly document renders a branch target through the module map", "[ui]")
{
    application();

    DocFixture fixture;
    fixture.put(kCode, {0x74, 0x15}); // JZ +0x15 -> kCode + 0x17
    fixture.pass();
    fixture.document.ensure_rows(1);

    // Without a module map the decoded padded text stays.
    CHECK(fixture.document.row(0).text == QStringLiteral("JZ 0000000000002017"));

    fixture.document.set_modules({module_image("app", kCode, 0x1000)});
    const auto jump = fixture.document.row(0);
    CHECK(jump.text == QStringLiteral("JZ app+17"));
    CHECK(jump.length == 2);
    CHECK(jump.bytes == QStringLiteral("74 15"));

    // Absolute mode restores the decoded text, and switching back re-renders live.
    fixture.document.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(fixture.document.row(0).text == QStringLiteral("JZ 0000000000002017"));
    fixture.document.set_address_mode(slopkit::ui::AddressMode::module_relative);
    CHECK(fixture.document.row(0).text == QStringLiteral("JZ app+17"));
}

TEST_CASE("the disassembly document renders a memory operand's address through the module map", "[ui]")
{
    application();

    DocFixture fixture;
    fixture.put(kCode, {0x48, 0x8B, 0x05, 0xF7, 0x02, 0x00, 0x00}); // MOV RAX, [RIP+0x2F7]
    fixture.pass();
    fixture.document.ensure_rows(1);
    fixture.document.set_modules({module_image("app", kCode, 0x1000)});

    // kCode + 7 + 0x2F7 = kCode + 0x2FE.
    CHECK(fixture.document.row(0).text == QStringLiteral("MOV RAX, [app+2FE]"));

    fixture.document.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(fixture.document.row(0).text == QStringLiteral("MOV RAX, [00000000000022FE]"));
}

TEST_CASE("the disassembly document keeps an out-of-module address decoded", "[ui]")
{
    application();

    DocFixture fixture;
    fixture.put(kCode, {0x48, 0x8B, 0x04, 0x25, 0x00, 0x20, 0x40, 0x00}); // MOV RAX, [0x402000]
    fixture.pass();
    fixture.document.ensure_rows(1);
    fixture.document.set_modules({module_image("app", kCode, 0x1000)});

    CHECK(fixture.document.row(0).text == QStringLiteral("MOV RAX, [0000000000402000]"));
}

TEST_CASE("the disassembly document copies the rendered instruction text", "[ui]")
{
    application();

    DocFixture fixture;
    fixture.put(kCode, {0x74, 0x15});
    fixture.pass();
    fixture.document.ensure_rows(1);
    fixture.document.set_modules({module_image("app", kCode, 0x1000)});

    const DisassemblyDocument& document = fixture.document;
    CHECK(document.copy_text(0, CopyFormat::module_relative) == QStringLiteral("app+0"));
    CHECK(document.copy_text(0, CopyFormat::absolute) == QStringLiteral("2000"));
    CHECK(document.copy_text(0, CopyFormat::bytes) == QStringLiteral("74 15"));
    CHECK(document.copy_text(0, CopyFormat::instruction) == QStringLiteral("JZ app+17"));
    CHECK(document.copy_text(0, CopyFormat::address_and_instruction) == QStringLiteral("app+0: JZ app+17"));
    CHECK(document.copy_text(0, CopyFormat::address_bytes_instruction) == QStringLiteral("app+0: 74 15  JZ app+17"));
}

TEST_CASE("the disassembly document exposes a row's referenced addresses", "[ui]")
{
    application();

    DocFixture fixture;
    fixture.put(kCode, {0x74, 0x15, 0x06, 0xC3}); // JZ +0x15, .byte 0x06, RET
    fixture.pass();
    fixture.document.ensure_rows(3);

    // The branch row references its target; a `.byte` or operand-less row
    // references nothing.
    const auto jump = fixture.document.row_addresses(0);
    REQUIRE(jump.size() == 1);
    CHECK(jump.front().address == kCode + 0x17);
    CHECK(fixture.document.row_addresses(1).empty()); // .byte 0x06
    CHECK(fixture.document.row_addresses(2).empty()); // RET

    // An out-of-range row hands out an empty span.
    CHECK(fixture.document.row_addresses(fixture.document.row_count()).empty());

    // A refused window masks every row, which then references nothing either.
    [[maybe_unused]] const auto submitted = fixture.document.next_live_request();
    LiveReading                 failed;
    failed.id       = DisassemblyDocument::kIdBase;
    failed.readable = false;
    fixture.document.apply_live_readings(std::vector<LiveReading> {failed});
    CHECK(fixture.document.row_addresses(0).empty());
}

TEST_CASE("the disassembly document NOPs a whole instruction and restores it", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x55, 0x48, 0x89, 0xE5, 0xC3}); // PUSH RBP; MOV RBP, RSP; RET
    fixture.pass();
    fixture.document.ensure_rows(3);

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    CHECK(fixture.document.patch_at(1) == nullptr);
    CHECK(fixture.document.row_annotation(1).isEmpty());

    CHECK(fixture.document.nop_instruction(1));
    REQUIRE(pump_worker(fixture.worker,
                        [&]
                        {
                            return !fixture.patches.empty();
                        }));

    // The whole three-byte instruction became three 0x90 bytes; its neighbours
    // are untouched.
    CHECK(fixture.access.memory->bytes.at(kCode) == std::byte {0x55});
    CHECK(fixture.access.memory->bytes.at(kCode + 1) == std::byte {0x90});
    CHECK(fixture.access.memory->bytes.at(kCode + 2) == std::byte {0x90});
    CHECK(fixture.access.memory->bytes.at(kCode + 3) == std::byte {0x90});
    CHECK(fixture.access.memory->bytes.at(kCode + 4) == std::byte {0xC3});
    REQUIRE(fixture.patches.size() == 1);

    // The listing re-decoded what the target holds: three plain NOP rows.
    fixture.document.ensure_rows(5);

    const CodePatch* patch = fixture.document.patch_at(1);
    REQUIRE(patch != nullptr);
    CHECK(patch->begin == kCode + 1);
    CHECK(patch->length == 3);
    CHECK(patch->original_text == QStringLiteral("MOV RBP, RSP"));
    CHECK(patch->original_bytes == std::vector<std::byte> {std::byte {0x48}, std::byte {0x89}, std::byte {0xE5}});

    // Every row inside the replaced instruction carries the patch; the marker is
    // on its first row only.
    CHECK(fixture.document.patch_at(2) == patch);
    CHECK(fixture.document.patch_at(3) == patch);
    CHECK(fixture.document.patch_at(0) == nullptr);
    CHECK(fixture.document.patch_at(4) == nullptr);
    CHECK(fixture.document.row_annotation(1) == QStringLiteral("NOPed: MOV RBP, RSP"));
    CHECK(fixture.document.row_annotation(2).isEmpty());

    CHECK(fixture.document.row(0).text == QStringLiteral("PUSH RBP"));
    CHECK(fixture.document.row(1).text == QStringLiteral("NOP"));
    CHECK(fixture.document.row(1).bytes == QStringLiteral("90"));
    CHECK(fixture.document.row(2).text == QStringLiteral("NOP"));
    CHECK(fixture.document.row(3).text == QStringLiteral("NOP"));
    CHECK(fixture.document.row(4).text == QStringLiteral("RET"));

    CHECK(logged(records, "disassembly nop: 3 byte(s) at 0x2001 replaced with NOP"));

    // Restoring from a row inside the region writes the original bytes back and
    // forgets the record.
    records.clear();
    CHECK(fixture.document.restore_instruction(kCode + 2));
    REQUIRE(pump_worker(fixture.worker,
                        [&]
                        {
                            return fixture.patches.empty();
                        }));
    CHECK(fixture.access.memory->bytes.at(kCode + 1) == std::byte {0x48});
    CHECK(fixture.access.memory->bytes.at(kCode + 2) == std::byte {0x89});
    CHECK(fixture.access.memory->bytes.at(kCode + 3) == std::byte {0xE5});
    CHECK(fixture.document.patch_at(1) == nullptr);
    CHECK(fixture.document.row_annotation(1).isEmpty());
    CHECK(logged(records, "disassembly restore: 3 byte(s) at 0x2001 written back"));

    fixture.document.ensure_rows(3);
    CHECK(fixture.document.row(1).text == QStringLiteral("MOV RBP, RSP"));
}

TEST_CASE("the disassembly document restores a one-byte instruction", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x55, 0xC3}); // PUSH RBP; RET
    fixture.pass();
    fixture.document.ensure_rows(2);

    CHECK(fixture.document.nop_instruction(0));
    REQUIRE(pump_worker(fixture.worker,
                        [&]
                        {
                            return !fixture.patches.empty();
                        }));
    CHECK(fixture.access.memory->bytes.at(kCode) == std::byte {0x90});
    REQUIRE(fixture.patches.size() == 1);
    fixture.document.ensure_rows(2);

    const CodePatch* patch = fixture.document.patch_at(0);
    REQUIRE(patch != nullptr);
    CHECK(patch->length == 1);
    CHECK(patch->original_bytes.size() == 1);
    CHECK(fixture.document.row_annotation(0) == QStringLiteral("NOPed: PUSH RBP"));

    CHECK(fixture.document.restore_instruction(kCode));
    REQUIRE(pump_worker(fixture.worker,
                        [&]
                        {
                            return fixture.patches.empty();
                        }));
    CHECK(fixture.access.memory->bytes.at(kCode) == std::byte {0x55});
    CHECK(fixture.document.patch_at(0) == nullptr);

    fixture.document.ensure_rows(2);
    CHECK(fixture.document.row(0).text == QStringLiteral("PUSH RBP"));
    CHECK(fixture.document.row(1).text == QStringLiteral("RET"));
}

TEST_CASE("the disassembly document refuses a NOP it cannot apply", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x55, 0x06, 0xC3}); // PUSH RBP; .byte 0x06; RET
    fixture.pass();
    fixture.document.ensure_rows(3);

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    // A `.byte` row is not a decoded instruction, so nothing is written.
    CHECK_FALSE(fixture.document.nop_instruction(1));
    CHECK(logged(records, "disassembly nop refused"));
    CHECK(fixture.patches.empty());
    CHECK(fixture.access.memory->bytes.at(kCode + 1) == std::byte {0x06});

    // A detached target refuses before anything else.
    records.clear();
    fixture.target.session_live = false;
    CHECK_FALSE(fixture.document.nop_instruction(0));
    CHECK(logged(records, "disassembly nop refused: no target attached"));
    fixture.target.session_live = true;

    // One write at a time: a second NOP is refused while the first is in flight.
    records.clear();
    CHECK(fixture.document.nop_instruction(0));
    CHECK_FALSE(fixture.document.nop_instruction(2));
    CHECK(logged(records, "disassembly nop refused: a code patch write is already in flight"));
    REQUIRE(pump_worker(fixture.worker,
                        [&]
                        {
                            return !fixture.patches.empty();
                        }));
    CHECK(fixture.access.memory->bytes.at(kCode) == std::byte {0x90});
    CHECK(fixture.access.memory->bytes.at(kCode + 2) == std::byte {0xC3}); // the refused row is untouched
}

TEST_CASE("the disassembly document edits an instruction and records the original", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x55, 0x48, 0x89, 0xE5, 0xC3}); // PUSH RBP; MOV RBP, RSP; RET
    fixture.pass();
    fixture.document.ensure_rows(3);

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    // The box starts from the instruction the listing prints.
    const auto source = fixture.document.edit_source(1);
    REQUIRE(source.has_value());
    CHECK(*source == QStringLiteral("MOV RBP, RSP"));
    CHECK(fixture.document.edit_error(1, "XOR EAX, EAX").empty());

    // A shorter instruction is padded with NOPs out to the original length.
    CHECK(fixture.document.edit_instruction(1, "XOR EAX, EAX"));
    REQUIRE(pump_worker(fixture.worker,
                        [&]
                        {
                            return !fixture.patches.empty();
                        }));
    CHECK(fixture.access.memory->bytes.at(kCode + 1) == std::byte {0x31});
    CHECK(fixture.access.memory->bytes.at(kCode + 2) == std::byte {0xC0});
    CHECK(fixture.access.memory->bytes.at(kCode + 3) == std::byte {0x90});
    CHECK(fixture.access.memory->bytes.at(kCode) == std::byte {0x55});
    CHECK(fixture.access.memory->bytes.at(kCode + 4) == std::byte {0xC3});

    REQUIRE(fixture.patches.size() == 1);
    fixture.document.ensure_rows(4);
    const CodePatch* patch = fixture.document.patch_at(1);
    REQUIRE(patch != nullptr);
    CHECK(patch->begin == kCode + 1);
    CHECK(patch->nop == false);
    CHECK(patch->length == 3);
    CHECK(patch->original_text == QStringLiteral("MOV RBP, RSP"));
    CHECK(fixture.document.row_annotation(1) == QStringLiteral("Edited: MOV RBP, RSP"));
    CHECK(logged(records, "disassembly edit: 2 byte(s) at 0x2001 replace a 3 byte instruction"));

    CHECK(fixture.document.row(1).text == QStringLiteral("XOR EAX, EAX"));
    CHECK(fixture.document.row(2).text == QStringLiteral("NOP"));
    CHECK(fixture.document.row(3).text == QStringLiteral("RET"));

    // A patched row is no longer editable until it is restored.
    CHECK_FALSE(fixture.document.edit_source(1).has_value());
    CHECK_FALSE(fixture.document.edit_source(2).has_value()); // the NOP pad

    // Restore writes the original instruction back.
    CHECK(fixture.document.restore_instruction(kCode + 1));
    REQUIRE(pump_worker(fixture.worker,
                        [&]
                        {
                            return fixture.patches.empty();
                        }));
    CHECK(fixture.access.memory->bytes.at(kCode + 1) == std::byte {0x48});
    CHECK(fixture.document.patch_at(1) == nullptr);
}

TEST_CASE("the disassembly document refuses an edit that does not fit", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x55, 0xC3}); // PUSH RBP; RET
    fixture.pass();
    fixture.document.ensure_rows(2);

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    CHECK_FALSE(fixture.document.edit_error(0, "MOV RAX, 0x1122334455667788").empty());
    CHECK_FALSE(fixture.document.edit_instruction(0, "MOV RAX, 0x1122334455667788"));
    CHECK(logged(records, "disassembly edit refused"));
    CHECK(fixture.patches.empty());
    CHECK(fixture.access.memory->bytes.at(kCode) == std::byte {0x55});

    // Text that does not assemble is refused before any write.
    records.clear();
    CHECK_FALSE(fixture.document.edit_error(0, "FROBNICATE").empty());
    CHECK_FALSE(fixture.document.edit_instruction(0, "FROBNICATE"));
    CHECK(fixture.patches.empty());
}

TEST_CASE("a `.byte` row is not editable", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x06, 0xC3}); // .byte 0x06; RET
    fixture.pass();
    fixture.document.ensure_rows(2);

    CHECK_FALSE(fixture.document.edit_source(0).has_value());
    CHECK_FALSE(fixture.document.edit_error(0, "RET").empty());
    CHECK_FALSE(fixture.document.edit_instruction(0, "RET"));
    CHECK(fixture.patches.empty());
    CHECK(fixture.access.memory->bytes.at(kCode) == std::byte {0x06});
}

TEST_CASE("re-accepting the unchanged source is not an edit", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x48, 0x89, 0xE5, 0xC3}); // MOV RBP, RSP; RET
    fixture.pass();
    fixture.document.ensure_rows(2);

    const auto source = fixture.document.edit_source(0);
    REQUIRE(source.has_value());
    CHECK(fixture.document.edit_error(0, source->toStdString()).empty());
    CHECK(fixture.document.edit_instruction(0, source->toStdString()));
    CHECK(fixture.patches.empty());
    CHECK(fixture.access.memory->bytes.at(kCode) == std::byte {0x48});
}

TEST_CASE("the disassembly document edits a rip-relative instruction in place", "[ui]")
{
    DocFixture fixture;
    fixture.put(kCode, {0x48, 0x8B, 0x05, 0xF7, 0x02, 0x00, 0x00, 0xC3}); // MOV RAX, [RIP+0x2F7]; RET
    fixture.pass();
    fixture.document.ensure_rows(2);

    // The listing prints the absolute target; re-accepting it keeps the short
    // rip-relative form, so nothing is written.
    const auto source = fixture.document.edit_source(0);
    REQUIRE(source.has_value());
    CHECK(*source == QStringLiteral("MOV RAX, [00000000000022FE]"));
    CHECK(fixture.document.edit_error(0, source->toStdString()).empty());
    CHECK(fixture.document.edit_instruction(0, source->toStdString()));
    CHECK(fixture.patches.empty());
    CHECK(fixture.access.memory->bytes.at(kCode + 5) == std::byte {0x00});
}
