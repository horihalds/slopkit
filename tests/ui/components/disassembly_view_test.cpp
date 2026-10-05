#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
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
    fixture.put(kCode, {0x55, 0x48, 0x89, 0xE5, 0xC3});

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

    // Scrolling up at the window start clamps instead of leaving the window.
    QApplication::sendEvent(&view, &up);
    CHECK(view.first_address() == kCode);

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

TEST_CASE("the disassembly view menu offers Go To and Copy address", "[ui]")
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
    view.populate_menu(menu);

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

    QAction* copy = action(menu, QStringLiteral("Copy address"));
    REQUIRE(copy != nullptr);
    copy->trigger();
    CHECK(QApplication::clipboard()->text() == fixture.document.address_text(view.first_address()));

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
