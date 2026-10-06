#include <catch2/catch.hpp>

#include <QFontMetrics>

#include "support/memory_view_helpers.hpp"
#include "ui/fonts.hpp"

TEST_CASE("the memory view auto-fits its rows to the widget", "[ui]")
{
    application();
    Fixture    fixture;
    MemoryView view(fixture.document);
    view.resize(800, 600);
    view.show();

    const std::size_t wide = view.bytes_per_row();
    REQUIRE(wide > 0);
    CHECK(view.horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);

    // A narrower widget fits fewer bytes per row.
    view.resize(360, 600);
    const std::size_t narrow = view.bytes_per_row();
    REQUIRE(narrow > 0);
    CHECK(narrow <= wide);

    // Hiding the text column gives its space back to the value cells.
    const std::size_t with_text = view.bytes_per_row();
    view.set_text_column_visible(false);
    CHECK_FALSE(view.text_column_visible());
    CHECK(view.bytes_per_row() >= with_text);

    // The row width is always a whole number of format units.
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int32, .hex = true});
    view.relayout();
    CHECK(view.bytes_per_row() % 4 == 0);

    view.hide();
}

TEST_CASE("a resize, format, encoding or text-column change keeps the top address exactly", "[ui]")
{
    application();
    Fixture    fixture;
    MemoryView view(fixture.document);
    view.show();

    // A one-byte row width parks the anchor on an address that no wider row
    // width divides, so every re-fit below would move it without the fix.
    view.resize(60, 600);
    REQUIRE(view.bytes_per_row() == 1);
    view.set_first_byte(0x1807);
    REQUIRE(view.first_byte() == 0x1807);

    // A wider widget re-fits the row width and leaves the anchor alone.
    view.resize(800, 600);
    REQUIRE(view.bytes_per_row() > 1);
    CHECK(view.first_byte() == 0x1807);

    // A narrower widget does too.
    view.resize(360, 600);
    REQUIRE(view.bytes_per_row() > 1);
    CHECK(view.first_byte() == 0x1807);

    // A value-format change re-fits the width the same way.
    view.resize(800, 600);
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int32, .hex = true});
    view.relayout();
    CHECK(view.bytes_per_row() % 4 == 0);
    CHECK(view.first_byte() == 0x1807);

    // An encoding change does too.
    fixture.document.set_encoding(TextEncoding::utf8);
    view.relayout();
    CHECK(view.first_byte() == 0x1807);

    // Hiding the text column gives its space back without moving the anchor.
    view.set_text_column_visible(false);
    CHECK_FALSE(view.text_column_visible());
    CHECK(view.first_byte() == 0x1807);

    view.hide();
}

TEST_CASE("the memory view document keeps an unaligned top address and still windows it", "[ui]")
{
    Fixture fixture;
    fill(*fixture.access.memory, kBase - kExtent, 3 * kExtent, std::byte {0x5A});

    const std::uint64_t anchor = kBase + 8; // Not a multiple of the row width.
    fixture.document.set_view(anchor, kRowBytes, kRows);
    CHECK(fixture.document.first_byte() == anchor);

    // The three slots are contiguous and their union still covers the visible
    // range, so every painted cell falls inside a requested block.
    const std::vector<slopkit::ui::LiveRequest> requests = fixture.document.next_live_request();
    REQUIRE(requests.size() == 3);
    CHECK(requests[1].address == requests[0].address + requests[0].size);
    CHECK(requests[2].address == requests[1].address + requests[1].size);
    CHECK(requests[0].address <= anchor);
    CHECK(requests[2].address + requests[2].size >= anchor + kExtent);

    fixture.pass();
    CHECK(fixture.document.cell(anchor).readable);
    CHECK(fixture.document.cell(anchor).text == QStringLiteral("5A"));
}

TEST_CASE("the memory view scrolls by whole rows and clamps at zero", "[ui]")
{
    application();
    Fixture    fixture;
    MemoryView view(fixture.document);
    view.resize(800, 600);

    // Go To and row scrolling keep the anchor offset instead of snapping it to
    // the row grid.
    view.set_first_byte(0x1808);
    CHECK(view.first_byte() == 0x1808);
    const std::size_t   bpr   = view.bytes_per_row();
    const std::uint64_t start = view.first_byte();
    REQUIRE(bpr > 1);

    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(&view, &down);
    CHECK(view.first_byte() == start + bpr);

    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QApplication::sendEvent(&view, &up);
    CHECK(view.first_byte() == start);

    QKeyEvent page_down(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
    QApplication::sendEvent(&view, &page_down);
    CHECK(view.first_byte() == start + view.visible_rows() * bpr);

    QKeyEvent page_up(QEvent::KeyPress, Qt::Key_PageUp, Qt::NoModifier);
    QApplication::sendEvent(&view, &page_up);
    CHECK(view.first_byte() == start);

    // Scrolling up at the start of the address space clamps at zero.
    view.set_first_byte(0);
    QApplication::sendEvent(&view, &up);
    CHECK(view.first_byte() == 0);

    // The wheel scrolls three rows per notch, offset and all.
    view.set_first_byte(start);
    const QPointF centre(view.viewport()->rect().center());
    QWheelEvent   wheel_down(centre,
                             QPointF(view.viewport()->mapToGlobal(centre.toPoint())),
                             QPoint(),
                             QPoint(0, -120),
                             Qt::NoButton,
                             Qt::NoModifier,
                             Qt::NoScrollPhase,
                             false);
    QApplication::sendEvent(view.viewport(), &wheel_down);
    CHECK(view.first_byte() == start + 3 * bpr);

    // Scrolling down saturates at the user-space ceiling.
    view.set_first_byte(slopkit::scan::kMaxUserAddress - 8);
    QApplication::sendEvent(&view, &down);
    CHECK(view.first_byte() == slopkit::scan::kMaxUserAddress);

    view.hide();
}

TEST_CASE("the memory view edits a cell inline", "[ui]")
{
    application();
    Fixture fixture;
    fill(*fixture.access.memory, kBase - 0x1000, 0x3000, std::byte {0x11});

    MemoryView view(fixture.document);
    view.resize(800, 600);
    view.show();
    view.set_first_byte(kBase);
    fixture.pass(); // Fill the window the view just asked for.

    const std::uint64_t        cell_address = view.first_byte();
    const std::optional<QRect> cell         = view.cell_rect(cell_address);
    REQUIRE(cell.has_value());
    CHECK(view.editor() == nullptr);

    const auto double_click_at = [&view](const QPoint& point)
    {
        QMouseEvent event(QEvent::MouseButtonDblClick,
                          QPointF(point),
                          QPointF(view.viewport()->mapToGlobal(point)),
                          Qt::LeftButton,
                          Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &event);
    };

    double_click_at(cell->center());
    REQUIRE(view.editor() != nullptr);
    CHECK(view.editor()->isVisible());
    CHECK(view.editor()->text() == QStringLiteral("11"));
    CHECK(view.editor()->geometry() == *cell);

    view.editor()->setText(QStringLiteral("0xEF"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(view.editor(), &enter);
    CHECK_FALSE(view.editor()->isVisible());
    REQUIRE(pump_worker(fixture.worker,
                        [&]
                        {
                            return fixture.access.memory->at(cell_address) == std::byte {0xEF};
                        }));

    // Escape cancels the second edit without writing.
    double_click_at(cell->center());
    REQUIRE(view.editor()->isVisible());
    view.editor()->setText(QStringLiteral("0x01"));
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(view.editor(), &escape);
    CHECK_FALSE(view.editor()->isVisible());
    CHECK(fixture.access.memory->at(cell_address) == std::byte {0xEF});

    view.hide();
}

TEST_CASE("the memory view options menu switches the display", "[ui]")
{
    application();
    Fixture    fixture;
    MemoryView view(fixture.document);
    view.resize(800, 600);
    view.show();

    QMenu menu;
    view.populate_options_menu(menu);

    const auto submenu = [](QMenu* parent, const QString& title) -> QMenu*
    {
        for (QAction* action : parent->actions())
        {
            if (action->menu() != nullptr && action->menu()->title() == title)
            {
                return action->menu();
            }
        }
        return nullptr;
    };
    const auto action = [](QMenu* parent, const QString& text) -> QAction*
    {
        for (QAction* candidate : parent->actions())
        {
            if (candidate->text() == text)
            {
                return candidate;
            }
        }
        return nullptr;
    };

    QMenu* format_menu = submenu(&menu, QStringLiteral("Value format"));
    REQUIRE(format_menu != nullptr);
    for (const QString& type :
         {QStringLiteral("Byte"), QStringLiteral("2 Bytes"), QStringLiteral("4 Bytes"), QStringLiteral("8 Bytes")})
    {
        QMenu* type_menu = submenu(format_menu, type);
        REQUIRE(type_menu != nullptr);
        CHECK(action(type_menu, QStringLiteral("Hex")) != nullptr);
        CHECK(action(type_menu, QStringLiteral("Decimal")) != nullptr);
    }
    CHECK(action(format_menu, QStringLiteral("Float")) != nullptr);
    CHECK(action(format_menu, QStringLiteral("Double")) != nullptr);

    // The active format and encoding are checked.
    QMenu* byte_menu = submenu(format_menu, QStringLiteral("Byte"));
    REQUIRE(byte_menu != nullptr);
    CHECK(action(byte_menu, QStringLiteral("Hex"))->isChecked());

    QMenu* encoding_menu = submenu(&menu, QStringLiteral("Text encoding"));
    REQUIRE(encoding_menu != nullptr);
    CHECK(action(encoding_menu, QStringLiteral("ASCII"))->isChecked());
    CHECK(action(encoding_menu, QStringLiteral("UTF-8"))->isCheckable());
    CHECK(action(encoding_menu, QStringLiteral("UTF-16"))->isCheckable());

    QAction* show_text = action(&menu, QStringLiteral("Show text column"));
    REQUIRE(show_text != nullptr);
    CHECK(show_text->isCheckable());
    CHECK(show_text->isChecked());

    // The top entry asks the dialog to prompt for an address and carries Ctrl+G.
    QAction* go_to = action(&menu, QStringLiteral("Go To..."));
    REQUIRE(go_to != nullptr);
    CHECK(go_to == view.goto_action());
    CHECK(go_to->shortcut() == QKeySequence(QStringLiteral("Ctrl+G")));
    bool asked = false;
    QObject::connect(&view,
                     &MemoryView::gotoRequested,
                     &view,
                     [&asked]
                     {
                         asked = true;
                     });
    go_to->trigger();
    CHECK(asked);

    // Choosing 4 Bytes (hex) re-lays out and keeps the top byte on screen.
    const std::uint64_t anchor = view.first_byte();
    action(submenu(format_menu, QStringLiteral("4 Bytes")), QStringLiteral("Hex"))->trigger();
    CHECK(fixture.document.format().type == slopkit::scan::ValueType::int32);
    CHECK(fixture.document.format().hex);
    CHECK(view.bytes_per_row() % 4 == 0);
    CHECK(view.first_byte() <= anchor);
    CHECK(anchor < view.first_byte() + view.bytes_per_row());

    action(encoding_menu, QStringLiteral("UTF-8"))->trigger();
    CHECK(fixture.document.encoding() == TextEncoding::utf8);

    const std::size_t with_text = view.bytes_per_row();
    show_text->trigger();
    CHECK_FALSE(view.text_column_visible());
    CHECK(view.bytes_per_row() >= with_text);

    view.hide();
}

TEST_CASE("the memory view Back returns to the previous top address", "[ui]")
{
    application();
    Fixture    fixture;
    MemoryView view(fixture.document);
    view.resize(800, 600);
    view.show();

    const auto action = [](QMenu* parent, const QString& text) -> QAction*
    {
        for (QAction* candidate : parent->actions())
        {
            if (candidate->text() == text)
            {
                return candidate;
            }
        }
        return nullptr;
    };

    // A fresh view has nothing to undo: Back is present but disabled.
    QMenu fresh;
    view.populate_options_menu(fresh);
    QAction* fresh_back = action(&fresh, QStringLiteral("Back"));
    REQUIRE(fresh_back != nullptr);
    CHECK_FALSE(fresh_back->isEnabled());
    CHECK_FALSE(view.can_go_back());
    CHECK_FALSE(view.back());

    const std::uint64_t anchor = view.first_byte();
    view.navigate_to(0x1800);
    CHECK(view.first_byte() == 0x1800);
    REQUIRE(view.can_go_back());

    QMenu after;
    view.populate_options_menu(after);
    QAction* back = action(&after, QStringLiteral("Back"));
    REQUIRE(back != nullptr);
    CHECK(back->isEnabled());

    CHECK(view.back());
    CHECK(view.first_byte() == anchor);
    CHECK_FALSE(view.can_go_back());
    CHECK_FALSE(view.back());

    // Scrolling (set_first_byte) is not navigation and never records.
    view.set_first_byte(0x1900);
    CHECK_FALSE(view.can_go_back());

    // Re-navigating to the address already at the top records nothing.
    view.navigate_to(0x1900);
    CHECK_FALSE(view.can_go_back());

    view.hide();
}

TEST_CASE("the memory view header labels each column with its offset", "[ui]")
{
    application();
    Fixture    fixture;
    MemoryView view(fixture.document);
    view.resize(800, 600);
    view.show();

    // A byte view: two hex digits per column, no `0x` prefix.
    REQUIRE(view.bytes_per_row() >= 4);
    CHECK(view.column_offset_text(0) == QStringLiteral("00"));
    CHECK(view.column_offset_text(1) == QStringLiteral("01"));
    CHECK(view.column_offset_text(view.bytes_per_row() - 1).size() == 2);

    // A 4-byte view steps by four.
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int32, .hex = true});
    view.relayout();
    REQUIRE(view.bytes_per_row() >= 8);
    CHECK(view.column_offset_text(0) == QStringLiteral("00"));
    CHECK(view.column_offset_text(1) == QStringLiteral("04"));
    CHECK(view.column_offset_text(2) == QStringLiteral("08"));

    view.hide();
}

TEST_CASE("the memory view reveals the full address on hover over a shortened module", "[ui]")
{
    application();
    Fixture    fixture;
    MemoryView view(fixture.document);
    view.resize(800, 600);
    view.show();
    view.set_first_byte(kBase);

    fixture.document.set_modules({module_image("DyingLightGame_TheBeast_x64_rwdi.exe", kBase, kExtent)});

    // The first painted row's y and a point inside the address column.
    const QFontMetrics metrics(slopkit::ui::mono_font());
    const QPoint       address_hover(5, 4 + (metrics.height() + 4) + 2);

    CHECK(view.hover_full_text(address_hover) == QStringLiteral("DyingLightGame_TheBeast_x64_rwdi.exe+0"));
    // The value cells hold values, not addresses, so a hover there reveals nothing.
    CHECK(view.hover_full_text(QPoint(500, address_hover.y())).isEmpty());
    // The header band and a point below every row reveal nothing.
    CHECK(view.hover_full_text(QPoint(5, 2)).isEmpty());
    CHECK(view.hover_full_text(QPoint(5, 6000)).isEmpty());

    // A module name that fits shows nothing new.
    fixture.document.set_modules({module_image("app", kBase, kExtent)});
    CHECK(view.hover_full_text(address_hover).isEmpty());

    view.hide();
}
