#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMenu>
#include <QWheelEvent>

#include "support/memory_view_helpers.hpp"
#include "ui/components/disassembly_view.hpp"

namespace
{
    using slopkit::ui::LiveReading;
    using slopkit::ui::LiveRequest;
    using slopkit::ui::components::CopyFormat;
    using slopkit::ui::components::DisassemblyDocument;
    using slopkit::ui::components::DisassemblyView;

    constexpr std::uint64_t kCode = 0x2000; // A multiple of the 8 KiB window.

    struct ViewFixture
    {
        FakeAccess                       access;
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target = attached_target();
        DisassemblyDocument              document {worker, target};

        ViewFixture()
        {
            attach_session(worker);
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

    QAction* action(QMenu& menu, const QString& text)
    {
        for (QAction* candidate : menu.actions())
        {
            if (candidate->text() == text)
            {
                return candidate;
            }
        }
        return nullptr;
    }

    QMenu* submenu(QMenu& menu, const QString& text)
    {
        if (QAction* entry = action(menu, text); entry != nullptr)
        {
            return entry->menu();
        }
        return nullptr;
    }
} // namespace

TEST_CASE("the disassembly view fits its rows to the viewport", "[ui]")
{
    application();
    ViewFixture     fixture;
    DisassemblyView view(fixture.document);
    view.resize(800, 600);
    view.show();

    REQUIRE(view.visible_rows() > 0);
    CHECK(view.horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);

    const std::size_t tall = view.visible_rows();
    view.resize(800, 300);
    CHECK(view.visible_rows() <= tall);

    view.hide();
}

TEST_CASE("the disassembly view paints the decoded rows", "[ui]")
{
    application();
    ViewFixture fixture;
    fixture.put(kCode, {0x55, 0x48, 0x89, 0xE5, 0xC3, 0x74, 0x15});

    DisassemblyView view(fixture.document);
    view.resize(800, 600);
    view.show();
    view.set_first_address(kCode);
    fixture.pass();

    CHECK(view.first_address() == kCode);
    CHECK(view.row_text(0) == QStringLiteral("PUSH RBP"));
    CHECK(view.row_text(1) == QStringLiteral("MOV RBP, RSP"));
    CHECK(view.row_text(2) == QStringLiteral("RET"));
    CHECK(fixture.document.row(1).bytes == QStringLiteral("48 89 E5"));

    // A branch target is painted through the module map like the address column.
    fixture.document.set_modules({module_image("app", kCode, 0x1000)});
    CHECK(view.row_text(3) == QStringLiteral("JZ app+1C"));

    view.hide();
}

TEST_CASE("the disassembly view scrolls by whole instructions and clamps", "[ui]")
{
    application();
    ViewFixture fixture;
    fill(*fixture.access.memory, kCode, DisassemblyDocument::kWindowSize, std::byte {0x90}); // NOP

    DisassemblyView view(fixture.document);
    view.resize(800, 600);
    view.show();
    view.set_first_address(kCode);
    fixture.pass();

    const std::uint64_t start = view.first_address();
    REQUIRE(view.row_text(0) == QStringLiteral("NOP"));

    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(&view, &down);
    CHECK(view.first_address() == start + 1);

    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QApplication::sendEvent(&view, &up);
    CHECK(view.first_address() == start);

    QKeyEvent page_down(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
    QApplication::sendEvent(&view, &page_down);
    CHECK(view.first_address() == start + view.visible_rows());

    QKeyEvent page_up(QEvent::KeyPress, Qt::Key_PageUp, Qt::NoModifier);
    QApplication::sendEvent(&view, &page_up);
    CHECK(view.first_address() == start);

    // The wheel steps three instruction rows.
    const QPointF centre(view.viewport()->rect().center());
    QWheelEvent   wheel(centre,
                        QPointF(view.viewport()->mapToGlobal(centre.toPoint())),
                        QPoint(),
                        QPoint(0, -120),
                        Qt::NoButton,
                        Qt::NoModifier,
                        Qt::NoScrollPhase,
                        false);
    QApplication::sendEvent(view.viewport(), &wheel);
    CHECK(view.first_address() == kCode + 3);

    // Scrolling up at the window start steps to the previous aligned window.
    view.set_first_address(kCode);
    REQUIRE(view.first_address() == kCode);
    QApplication::sendEvent(&view, &up);
    CHECK(view.first_address() == kCode - DisassemblyDocument::kWindowSize);

    view.hide();
}

TEST_CASE("the disassembly view jumps to the window bounds", "[ui]")
{
    application();
    ViewFixture fixture;
    fill(*fixture.access.memory, kCode, DisassemblyDocument::kWindowSize, std::byte {0x90});

    DisassemblyView view(fixture.document);
    view.resize(800, 600);
    view.show();
    view.set_first_address(kCode + 4);
    fixture.pass();
    REQUIRE(view.first_address() == kCode + 4);

    QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    QApplication::sendEvent(&view, &home);
    CHECK(view.first_address() == kCode);

    QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
    QApplication::sendEvent(&view, &end);
    CHECK(view.first_address() == kCode + DisassemblyDocument::kWindowSize - 1);

    view.hide();
}

TEST_CASE("the disassembly view goes to absolute, module and module+offset text", "[ui]")
{
    application();
    ViewFixture fixture;
    fill(*fixture.access.memory, kCode, DisassemblyDocument::kWindowSize, std::byte {0x90});

    DisassemblyView view(fixture.document);
    view.resize(800, 600);
    view.show();
    fixture.document.set_modules({module_image("app", kCode, 0x1000)});

    CHECK(view.go_to(QStringLiteral("0x2010")));
    CHECK(view.first_address() == kCode + 0x10);

    CHECK(view.go_to(QStringLiteral("app")));
    CHECK(view.first_address() == kCode);

    CHECK(view.go_to(QStringLiteral("app+20")));
    CHECK(view.first_address() == kCode + 0x20);

    CHECK_FALSE(view.go_to(QStringLiteral("not-an-address")));
    CHECK(view.first_address() == kCode + 0x20);

    view.hide();
}

TEST_CASE("the disassembly view keeps its cursor across a resize", "[ui]")
{
    application();
    ViewFixture fixture;
    fill(*fixture.access.memory, kCode, DisassemblyDocument::kWindowSize, std::byte {0x90});

    DisassemblyView view(fixture.document);
    view.resize(800, 600);
    view.show();
    view.set_first_address(kCode + 8);
    fixture.pass();
    REQUIRE(view.row_text(0) == QStringLiteral("NOP"));

    view.resize(500, 300);
    CHECK(view.first_address() == kCode + 8);
    CHECK(view.row_text(0) == QStringLiteral("NOP"));

    view.hide();
}

TEST_CASE("the disassembly listing wraps the bytes column when the pane narrows", "[ui]")
{
    application();
    ViewFixture fixture;
    // 15 bytes: redundant operand-size prefixes in front of `MOV RAX, imm64`.
    fixture.put(kCode, {0x66, 0x66, 0x66, 0x66, 0x66, 0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11});

    DisassemblyView view(fixture.document);
    view.resize(1400, 600);
    view.show();
    view.set_first_address(kCode);
    fixture.pass();

    // A wide pane fits the whole instruction on one line, like before.
    REQUIRE(view.bytes_per_line() >= 15);
    CHECK(view.row_lines(0) == 1);

    // The narrowest useful pane still shows every byte: one token per line.
    view.resize(200, 600);
    CHECK(view.bytes_per_line() == 1);
    CHECK(view.row_lines(0) == 15);

    // Widening gives the bytes column its room back and unwraps the row.
    view.resize(1400, 600);
    CHECK(view.row_lines(0) == 1);
    CHECK(view.bytes_per_line() >= 15);

    view.hide();
}

TEST_CASE("the wrapped listing keeps the cursor and pages by whole rows", "[ui]")
{
    application();
    ViewFixture fixture;
    // 15 bytes: redundant operand-size prefixes in front of `MOV RAX, imm64`.
    fixture.put(kCode, {0x66, 0x66, 0x66, 0x66, 0x66, 0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11});

    DisassemblyView view(fixture.document);
    view.resize(1400, 600);
    view.show();
    view.set_first_address(kCode);
    fixture.pass();

    REQUIRE(fixture.document.row(0).address == kCode);
    const QString top = view.row_text(0);

    // Narrowing wraps the bytes but keeps the top instruction and the anchor.
    view.resize(200, 600);
    CHECK(view.bytes_per_line() == 1);
    CHECK(view.row_lines(0) == 15);
    CHECK(view.first_address() == kCode);
    CHECK(view.row_text(0) == top);
    CHECK(fixture.document.window_base() == kCode);

    // PageDown moves by exactly the post-wrap rows that fit.
    const std::size_t page = view.visible_rows();
    REQUIRE(page >= 1);
    REQUIRE(page < fixture.document.row_count());
    const std::uint64_t next = fixture.document.row(page).address;

    QKeyEvent page_down(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
    QApplication::sendEvent(&view, &page_down);
    CHECK(view.first_address() == next);
    CHECK(view.row_text(0) == fixture.document.row(page).text);

    view.hide();
}

TEST_CASE("the wrapped listing walks past a window boundary", "[ui]")
{
    application();
    ViewFixture fixture;
    fill(*fixture.access.memory, kCode, DisassemblyDocument::kWindowSize, std::byte {0x90});
    fill(*fixture.access.memory,
         kCode + DisassemblyDocument::kWindowSize,
         DisassemblyDocument::kWindowSize,
         std::byte {0x90});

    DisassemblyView view(fixture.document);
    view.resize(800, 600);
    view.show();
    view.set_first_address(kCode);
    fixture.pass();
    REQUIRE(view.first_address() == kCode);

    // Walk to the window's own end, then one row further: the step lands on the
    // next page-aligned window instead of dead-ending.
    QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
    QApplication::sendEvent(&view, &end);
    CHECK(view.first_address() == kCode + DisassemblyDocument::kWindowSize - 1);

    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(&view, &down);
    CHECK(view.first_address() == kCode + DisassemblyDocument::kWindowSize);

    // The next live pass asks for the next aligned window.
    const auto forward = fixture.document.next_live_request();
    REQUIRE(forward.size() == 1);
    CHECK(forward[0].address == kCode + DisassemblyDocument::kWindowSize);
    CHECK(forward[0].address % 4096 == 0);

    fixture.pass();
    REQUIRE(view.first_address() == kCode + DisassemblyDocument::kWindowSize);
    CHECK(view.row_text(0) == QStringLiteral("NOP"));

    // Stepping back up seats the previous aligned base, then its last row.
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QApplication::sendEvent(&view, &up);
    CHECK(view.first_address() == kCode);

    const auto backward = fixture.document.next_live_request();
    REQUIRE(backward.size() == 1);
    CHECK(backward[0].address == kCode);
    fixture.pass();
    CHECK(view.first_address() == kCode + DisassemblyDocument::kWindowSize - 1);

    view.hide();
}

TEST_CASE("the disassembly view menu offers Go To and a Copy submenu", "[ui]")
{
    application();
    ViewFixture fixture;
    fill(*fixture.access.memory, kCode, DisassemblyDocument::kWindowSize, std::byte {0x90});

    DisassemblyView view(fixture.document);
    view.resize(800, 600);
    view.show();
    view.set_first_address(kCode + 2);
    fixture.pass();

    QMenu menu;
    view.populate_menu(menu, 0);

    QAction* go_to = action(menu, QStringLiteral("Go To..."));
    REQUIRE(go_to != nullptr);
    CHECK(go_to == view.goto_action());
    // The byte view owns the window's single Ctrl+G, so the listing has none.
    CHECK(go_to->shortcut().isEmpty());

    bool asked = false;
    QObject::connect(&view,
                     &DisassemblyView::gotoRequested,
                     &view,
                     [&asked]
                     {
                         asked = true;
                     });
    go_to->trigger();
    CHECK(asked);

    QMenu* copy = submenu(menu, QStringLiteral("Copy"));
    REQUIRE(copy != nullptr);
    REQUIRE(copy->actions().size() == 7);

    const auto check = [&](const QString& label, CopyFormat format)
    {
        QAction* entry = action(*copy, label);
        REQUIRE(entry != nullptr);
        QApplication::clipboard()->setText(QStringLiteral("sentinel"));
        entry->trigger();
        CHECK(QApplication::clipboard()->text() == fixture.document.copy_text(0, format));
    };
    check(QStringLiteral("Address (module + RVA)"), CopyFormat::module_relative);
    check(QStringLiteral("Address (absolute)"), CopyFormat::absolute);
    check(QStringLiteral("Bytes"), CopyFormat::bytes);
    check(QStringLiteral("Instruction"), CopyFormat::instruction);
    check(QStringLiteral("Address + bytes"), CopyFormat::address_and_bytes);
    check(QStringLiteral("Address + instruction"), CopyFormat::address_and_instruction);
    check(QStringLiteral("Address + bytes + instruction"), CopyFormat::address_bytes_instruction);

    view.hide();
}

TEST_CASE("the disassembly view Back returns to the previous top address", "[ui]")
{
    application();
    ViewFixture     fixture;
    DisassemblyView view(fixture.document);
    view.resize(800, 600);
    view.show();
    view.set_first_address(kCode);
    fixture.pass();

    // A fresh view has nothing to undo: Back is present but disabled.
    QMenu fresh;
    view.populate_menu(fresh, 0);
    QAction* fresh_back = action(fresh, QStringLiteral("Back"));
    REQUIRE(fresh_back != nullptr);
    CHECK_FALSE(fresh_back->isEnabled());
    CHECK_FALSE(view.can_go_back());
    CHECK_FALSE(view.back());

    view.navigate_to(kCode + 0x400);
    REQUIRE(view.can_go_back());
    CHECK(view.first_address() == kCode + 0x400);

    QMenu after;
    view.populate_menu(after, 0);
    QAction* back = action(after, QStringLiteral("Back"));
    REQUIRE(back != nullptr);
    CHECK(back->isEnabled());
    back->trigger();
    CHECK(view.first_address() == kCode);
    CHECK_FALSE(view.can_go_back());

    // Scrolling (set_first_address) is not navigation and never records.
    view.set_first_address(kCode + 0x100);
    CHECK_FALSE(view.can_go_back());

    view.hide();
}

TEST_CASE("the disassembly view follows an instruction's referenced address", "[ui]")
{
    application();
    ViewFixture     fixture;
    DisassemblyView view(fixture.document);
    view.resize(800, 600);
    view.show();
    view.set_first_address(kCode);
    // CALL rel32 (target kCode+5), then a `.byte 0x06`.
    fixture.put(kCode, {0xE8, 0x00, 0x00, 0x00, 0x00, 0x06});
    fixture.pass();
    fixture.document.ensure_rows(2);

    // The call row offers both follows; a row with no reference offers neither
    // but still a Back.
    QMenu referenced;
    view.populate_menu(referenced, 0);
    QAction* follow        = action(referenced, QStringLiteral("Follow"));
    QAction* follow_memory = action(referenced, QStringLiteral("Follow in Memory View"));
    REQUIRE(follow != nullptr);
    REQUIRE(follow_memory != nullptr);
    QAction* back = action(referenced, QStringLiteral("Back"));
    REQUIRE(back != nullptr);
    CHECK_FALSE(back->isEnabled());

    QMenu plain;
    view.populate_menu(plain, 1);
    CHECK(action(plain, QStringLiteral("Follow")) == nullptr);
    CHECK(action(plain, QStringLiteral("Follow in Memory View")) == nullptr);
    CHECK(action(plain, QStringLiteral("Back")) != nullptr);

    // "Follow in Memory View" only announces the address; the listing stays put.
    std::uint64_t requested = 0;
    QObject::connect(&view,
                     &DisassemblyView::followInMemoryViewRequested,
                     &view,
                     [&requested](std::uint64_t address)
                     {
                         requested = address;
                     });
    follow_memory->trigger();
    CHECK(requested == kCode + 5);
    CHECK(view.first_address() == kCode);
    CHECK_FALSE(view.can_go_back());

    // "Follow" moves the listing onto the target and enables Back.
    follow->trigger();
    CHECK(view.first_address() == kCode + 5);
    REQUIRE(view.can_go_back());

    // Following the address already at the top records nothing.
    view.clear_history();
    view.navigate_to(view.first_address());
    CHECK_FALSE(view.can_go_back());

    view.hide();
}

TEST_CASE("showing and hiding the disassembly view toggles the document visibility", "[ui]")
{
    application();
    ViewFixture fixture;

    DisassemblyView view(fixture.document);
    view.resize(800, 600);
    view.show();
    CHECK(fixture.document.visible());
    CHECK(fixture.document.next_live_request().size() == 1);

    view.hide();
    CHECK_FALSE(fixture.document.visible());
    CHECK(fixture.document.next_live_request().empty());
}
