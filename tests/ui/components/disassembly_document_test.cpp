#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "support/memory_view_helpers.hpp"
#include "ui/components/disassembly_document.hpp"

namespace
{
    using slopkit::ui::LiveReading;
    using slopkit::ui::LiveRequest;
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
