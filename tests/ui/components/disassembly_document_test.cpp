#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <vector>

#include "support/memory_view_helpers.hpp"
#include "ui/components/disassembly_document.hpp"

namespace
{
    using slopkit::ui::LiveReading;
    using slopkit::ui::LiveRequest;
    using slopkit::ui::components::CopyFormat;
    using slopkit::ui::components::DisassemblyDocument;

    // A code window at an 8 KiB boundary, so the cursor sits at its start.
    constexpr std::uint64_t kCode = 0x2000;

    struct DocFixture
    {
        FakeAccess                       access;
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target = attached_target();
        DisassemblyDocument              document {worker, target};

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
    CHECK(fixture.document.row(1).text == QStringLiteral(".byte 0x06"));
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
    CHECK(fixture.document.row(1).text == QStringLiteral(".byte 0x06"));
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
    CHECK(fixture.document.row(0).text == QStringLiteral("MOV RAX, 0x01"));

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
    CHECK(fixture.document.address_text(kCode + 0x10) == QStringLiteral("0x0000000000002010"));

    fixture.document.set_modules({module_image("app", kCode, 0x1000)});
    CHECK(fixture.document.address_text(kCode) == QStringLiteral("app+0"));
    CHECK(fixture.document.address_text(kCode + 0x10) == QStringLiteral("app+10"));

    fixture.document.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(fixture.document.address_text(kCode + 0x10) == QStringLiteral("0x0000000000002010"));
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
    CHECK(document.copy_text(0, CopyFormat::absolute) == QStringLiteral("0x2000"));
    CHECK(document.copy_text(0, CopyFormat::bytes) == QStringLiteral("55"));
    CHECK(document.copy_text(0, CopyFormat::instruction) == QStringLiteral("PUSH RBP"));
    CHECK(document.copy_text(0, CopyFormat::address_and_bytes) == QStringLiteral("app+0: 55"));
    CHECK(document.copy_text(0, CopyFormat::address_and_instruction) == QStringLiteral("app+0: PUSH RBP"));
    CHECK(document.copy_text(0, CopyFormat::address_bytes_instruction) == QStringLiteral("app+0: 55  PUSH RBP"));

    // The combined forms follow the current display mode for the address.
    fixture.document.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(document.copy_text(0, CopyFormat::address_and_bytes) == QStringLiteral("0x0000000000002000: 55"));

    // Out-of-range rows hand out nothing.
    CHECK(document.copy_text(fixture.document.row_count(), CopyFormat::bytes).isEmpty());
}
