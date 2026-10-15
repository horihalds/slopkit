#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QRect>
#include <QScrollBar>
#include <QStringList>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "reclass/node.hpp"
#include "reclass/project.hpp"
#include "support/fake_process.hpp"
#include "support/reclass_helpers.hpp"
#include "support/reclass_live_helpers.hpp"
#include "support/ui_helpers.hpp"
#include "ui/components/reclass_view.hpp"
#include "ui/fonts.hpp"
#include "ui/models/reclass_proxy_model.hpp"
#include "ui/models/reclass_structure_model.hpp"
#include "ui/models/reclass_value_model.hpp"
#include "ui/theme.hpp"

namespace
{
    using slopkit::process::AccessWorker;
    using slopkit::process::AttachedTarget;
    using slopkit::reclass::Class;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::Project;
    using slopkit::test::attach_session;
    using slopkit::test::drain_until_idle;
    using slopkit::test::FakeAccess;
    using slopkit::test::module_image;
    using slopkit::test::node_of;
    using slopkit::test::reading_for;
    using slopkit::test::region_of;
    using slopkit::test::stage_bytes;
    using slopkit::ui::LiveReading;
    using slopkit::ui::LiveRequest;
    using slopkit::ui::components::ReClassView;
    using slopkit::ui::models::ReclassProxyModel;
    using slopkit::ui::models::ReclassRow;
    using slopkit::ui::models::ReclassStructureModel;
    using slopkit::ui::models::ReclassValueModel;

    constexpr std::uint64_t kAppBase = 0x400000;

    // Player { int8 tag; class_pointer next } and Point { int32 x; int32 y },
    // with Player pinned at app+0x10.
    class Fixture
    {
    public:
        Fixture()
        {
            target = slopkit::test::fake_target();
            attach_session(worker);
            values.set_modules(std::vector<slopkit::process::ModuleInfo> {module_image("app", kAppBase, 0x1000)},
                               std::vector<slopkit::process::RegionInfo> {
                                   region_of(kAppBase, 0x1000, "/app", true),
                               });
            values.set_structure(structure);

            project.add_class("Point", "app+0x20");
            project.add_class("Player", "app+0x10");

            Class& point = project.classes()[0];
            point.nodes.push_back(node_of(project, NodeKind::int32, "x"));
            point.nodes.push_back(node_of(project, NodeKind::int32, "y"));

            Class& player = project.classes()[1];
            player.nodes.push_back(node_of(project, NodeKind::int8, "tag"));
            slopkit::reclass::NodePtr next = node_of(project, NodeKind::class_pointer, "next");
            next->class_name               = "Point";
            player.nodes.push_back(std::move(next));

            structure.set_project(project);
            structure.set_class(1);
        }

        [[nodiscard]] std::uint64_t player_address() const
        {
            return kAppBase + 0x10;
        }

        [[nodiscard]] std::size_t node_id(std::size_t index) const
        {
            return project.classes()[1].nodes[index]->id;
        }

        // The model rebuilds its rows whenever the structure changes, which
        // frees the objects an index points at, so an index is taken fresh each
        // time it is used.
        [[nodiscard]] QModelIndex cell(std::size_t node_id, int column) const
        {
            const QModelIndex index = structure.index_for_node(node_id);
            return index.isValid() ? index.sibling(index.row(), column) : QModelIndex();
        }

        void resolve()
        {
            values.maybe_resolve_addresses();
            REQUIRE(drain_until_idle(worker));
        }

        void put(std::uint64_t address, std::initializer_list<unsigned> values)
        {
            stage_bytes(access, address, values);
        }

        // One live pass: the reads the grid would ask for, answered from the
        // fake memory the way the live values pass answers them.
        void pass()
        {
            const std::vector<LiveRequest> requests = values.requests();
            std::vector<LiveReading>       readings;
            readings.reserve(requests.size());
            for (const LiveRequest& request : requests)
            {
                readings.push_back(reading_for(access, request.id, request.address, request.size));
            }
            values.apply_readings(readings);
        }

        [[nodiscard]] bool holds(std::uint64_t address, std::initializer_list<unsigned> values) const
        {
            std::uint64_t offset = 0;
            for (const unsigned value : values)
            {
                const auto entry = access.memory->bytes.find(address + offset);
                if (entry == access.memory->bytes.end() || entry->second != static_cast<std::byte>(value))
                {
                    return false;
                }
                ++offset;
            }
            return true;
        }

        FakeAccess            access;
        AccessWorker          worker {access};
        AttachedTarget        target;
        Project               project;
        ReclassStructureModel structure {project};
        ReclassValueModel     values {worker, target};
        ReclassProxyModel     proxy {structure, values};
    };

    // The component paints its rows itself, so a test can hand them an image
    // instead of hoping a viewport render reaches them.
    class PaintedView : public ReClassView
    {
    public:
        using ReClassView::ReClassView;

        void paint_into(QPaintDevice& device) const
        {
            QPainter painter(&device);
            paint_rows(painter);
        }
    };

    void mouse_event(ReClassView&          view,
                     QEvent::Type          type,
                     const QPoint&         position,
                     Qt::MouseButton       button,
                     Qt::MouseButtons      buttons,
                     Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QMouseEvent event(
            type, QPointF(position), QPointF(view.viewport()->mapToGlobal(position)), button, buttons, modifiers);
        QApplication::sendEvent(view.viewport(), &event);
    }

    void click(ReClassView& view, const QModelIndex& index, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        const QPoint point = view.cell_rect(index).value().center();
        mouse_event(view, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, modifiers);
        mouse_event(view, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton, modifiers);
    }

    void double_click(ReClassView& view, const QModelIndex& index)
    {
        const QPoint point = view.cell_rect(index).value().center();
        mouse_event(view, QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
    }

    std::optional<std::size_t> node_at(const ReclassStructureModel& structure, const QModelIndex& index)
    {
        const ReclassRow* row = structure.row_for(index);
        return row == nullptr ? std::nullopt : std::optional<std::size_t> {row->node_id};
    }

    std::vector<std::optional<std::size_t>> selected_nodes(const ReClassView&           view,
                                                           const ReclassStructureModel& structure)
    {
        std::vector<std::optional<std::size_t>> ids;
        for (const QModelIndex& index : view.selected_rows())
        {
            ids.push_back(node_at(structure, index));
        }
        return ids;
    }

    // Whether a painted band holds a pixel within `tolerance` of `ink`: a glyph's
    // antialiased edge blends toward the surface, so only its core passes.
    bool paints_ink(const QImage& image, const QRect& band, const QColor& ink, int tolerance = 12)
    {
        for (int y = band.top(); y <= band.bottom(); ++y)
        {
            for (int x = band.left(); x <= band.right(); ++x)
            {
                const QColor sample = image.pixelColor(x, y);
                if (std::abs(sample.red() - ink.red()) <= tolerance
                    && std::abs(sample.green() - ink.green()) <= tolerance
                    && std::abs(sample.blue() - ink.blue()) <= tolerance)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // The x range a band's ink spans, ignoring the row's own tint (sampled at the
    // band's corner); absent when the band holds only background.
    std::optional<std::pair<int, int>> ink_span(const QImage& image, const QRect& band)
    {
        const QColor        background = image.pixelColor(band.left(), band.top());
        std::pair<int, int> span {-1, -1};
        for (int x = band.left(); x <= band.right(); ++x)
        {
            for (int y = band.top(); y <= band.bottom(); ++y)
            {
                if (image.pixelColor(x, y) != background)
                {
                    if (span.first < 0)
                    {
                        span.first = x;
                    }
                    span.second = x;
                }
            }
        }
        return span.first < 0 ? std::nullopt : std::optional<std::pair<int, int>> {span};
    }
}

TEST_CASE("the class view holds fixed columns and paints no header", "[reclass][ui][view]")
{
    application();

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(1000, 200);
    view.show();
    QCoreApplication::processEvents();

    using slopkit::ui::models::ReclassStructureModel;

    CHECK(view.model() == &fixture.proxy);
    CHECK(view.structure() == &fixture.structure);
    CHECK(view.row_count() == 2);

    // The fields stand in the order the row reads: offset, address, ascii, type,
    // name, value, comment, hints, pointer.
    CHECK(ReclassStructureModel::offset < ReclassStructureModel::address);
    CHECK(ReclassStructureModel::address < ReclassStructureModel::ascii);
    CHECK(ReclassStructureModel::ascii < ReclassStructureModel::type);
    CHECK(ReclassStructureModel::type < ReclassStructureModel::name);
    CHECK(ReclassStructureModel::name < ReclassStructureModel::value);
    CHECK(ReclassStructureModel::value < ReclassStructureModel::comment);
    CHECK(ReclassStructureModel::comment < ReclassStructureModel::hints);
    CHECK(ReclassStructureModel::hints < ReclassStructureModel::pointer);

    // Every field starts where the one before it ended, so the one gutter each
    // column was measured with is all the space between them.
    for (int column = 1; column < ReclassStructureModel::column_count; ++column)
    {
        CHECK(view.column_x(column) == view.column_x(column - 1) + view.column_width(column - 1));
    }

    CHECK(view.column_width(ReclassStructureModel::offset) > 0);
    CHECK(view.column_width(ReclassStructureModel::address) > 0);
    CHECK(view.column_width(ReclassStructureModel::ascii) > 0);
    CHECK(view.column_width(ReclassStructureModel::type) > 0);
    CHECK(view.column_width(ReclassStructureModel::name) > 0);
    CHECK(view.column_width(ReclassStructureModel::value) > 0);
    CHECK(view.column_width(ReclassStructureModel::comment) > 0);

    // The class draws no hex cell, so the hints field collapses to nothing; the
    // pointer field is the width the class's pointer row needs.
    CHECK(view.column_width(ReclassStructureModel::hints) == 0);
    CHECK(view.column_x(ReclassStructureModel::pointer) == view.column_x(ReclassStructureModel::hints));
    CHECK(view.column_width(ReclassStructureModel::pointer) > 0);

    // A row with no note draws the bare marker.
    CHECK(view.cell_text(fixture.cell(fixture.node_id(0), ReclassStructureModel::comment)).toStdString() == "//");

    // The pane's leftover width trails after the pointer field instead of being
    // swallowed by the value cell.
    const int end = view.column_x(ReclassStructureModel::pointer) + view.column_width(ReclassStructureModel::pointer);
    CHECK(end < view.viewport()->width());

    const QModelIndex name_cell = fixture.cell(fixture.node_id(0), ReclassStructureModel::name);
    const int         address_x = view.column_x(ReclassStructureModel::address);
    const int         address_w = view.column_width(ReclassStructureModel::address);
    const int         comment_x = view.column_x(ReclassStructureModel::comment);
    const int         value_w   = view.column_width(ReclassStructureModel::value);
    const int         first_top = view.cell_rect(name_cell).value().top();

    CHECK(view.cell_text(name_cell).toStdString() == "tag");

    // The widths follow the class, never the pane: a wider pane only moves the
    // slack.
    view.resize(1400, 200);
    QCoreApplication::processEvents();

    CHECK(view.column_x(ReclassStructureModel::address) == address_x);
    CHECK(view.column_width(ReclassStructureModel::address) == address_w);
    CHECK(view.column_width(ReclassStructureModel::value) == value_w);
    CHECK(view.column_x(ReclassStructureModel::comment) == comment_x);

    // No header band: the first row starts at the view's own top margin.
    CHECK(view.cell_rect(name_cell).value().top() == first_top);
    CHECK(first_top <= 8);
}

TEST_CASE("the narrow columns are sized to the text the class draws", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;

    // `tail` sits at offset 0x11, so the class draws a two-digit offset beside
    // `tag`'s one-digit one and the per-row gap can be compared.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex8, "tail"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    PaintedView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    const QFontMetrics mono(slopkit::ui::mono_font());
    const QFontMetrics prose(slopkit::ui::ui_font());
    // Player holds `tag` and a `Point` pointer: the offset, type and name
    // columns must fit those texts and leave one column gap after them, not the
    // room the widest kind in the language and a full-width name would need.
    constexpr int      kAllowedGap = 24;

    const int offset_widest = mono.horizontalAdvance(QStringLiteral("1"));
    CHECK(view.column_width(ReclassStructureModel::offset) >= offset_widest);
    CHECK(view.column_width(ReclassStructureModel::offset) <= offset_widest + kAllowedGap);

    // The type column also carries the disclosure triangle of the pointer row.
    const int type_widest = prose.horizontalAdvance(QStringLiteral("Pointer <Point>"));
    CHECK(view.column_width(ReclassStructureModel::type) >= type_widest);
    CHECK(view.column_width(ReclassStructureModel::type) <= type_widest + kAllowedGap);

    const int name_widest = prose.horizontalAdvance(QStringLiteral("next"));
    CHECK(view.column_width(ReclassStructureModel::name) >= name_widest);
    CHECK(view.column_width(ReclassStructureModel::name) <= name_widest + kAllowedGap);

    // The widths follow the rows the class draws: a longer name widens its
    // column when the tree is rebuilt...
    const int name_width                        = view.column_width(ReclassStructureModel::name);
    fixture.project.classes()[1].nodes[0]->name = "a_much_longer_field_name";
    fixture.structure.rebuild();
    QCoreApplication::processEvents();
    CHECK(view.column_width(ReclassStructureModel::name) > name_width);

    // ...while a live pass repaints without re-measuring anything.
    const int settled = view.column_width(ReclassStructureModel::name);
    fixture.resolve();
    fixture.put(
        fixture.player_address(),
        {1, 0x20, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.pass();
    QCoreApplication::processEvents();
    CHECK(view.column_width(ReclassStructureModel::name) == settled);

    // The reading that arrived on the live pass fits its own field, which is
    // sized for the widest reading a hex cell can draw rather than for what the
    // class happens to read now.
    const QModelIndex raw_hint = fixture.cell(fixture.node_id(2), ReclassStructureModel::hints);
    CHECK_FALSE(view.cell_text(raw_hint).isEmpty());
    CHECK(view.hover_full_text(view.cell_rect(raw_hint).value().center()).isEmpty());

    // The offset and ascii columns sit on their content box, so the one-digit
    // offset of `tag` and the two-digit one of `tail` leave the same gap before
    // the address text - and the same holds for ascii before the type.
    QImage image(view.viewport()->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    view.paint_into(image);

    const auto gap_after = [&](std::size_t node_id, int column, int next_column) -> std::optional<int>
    {
        const auto ink = ink_span(image, view.cell_rect(fixture.cell(node_id, column)).value());
        if (!ink.has_value())
        {
            return std::nullopt;
        }
        // Measured to the next column's text origin (its padding), so a different
        // first glyph there cannot move the gap.
        return view.column_x(next_column) + 4 - ink->second;
    };

    const auto tag_offset =
        gap_after(fixture.node_id(0), ReclassStructureModel::offset, ReclassStructureModel::address);
    const auto tail_offset =
        gap_after(fixture.node_id(3), ReclassStructureModel::offset, ReclassStructureModel::address);
    REQUIRE(tag_offset.has_value());
    REQUIRE(tail_offset.has_value());
    CHECK(*tag_offset == *tail_offset);
    CHECK(*tag_offset > 0);

    const auto tag_ascii  = gap_after(fixture.node_id(0), ReclassStructureModel::ascii, ReclassStructureModel::type);
    const auto tail_ascii = gap_after(fixture.node_id(3), ReclassStructureModel::ascii, ReclassStructureModel::type);
    REQUIRE(tag_ascii.has_value());
    REQUIRE(tail_ascii.has_value());
    CHECK(*tag_ascii == *tail_ascii);
    CHECK(*tag_ascii > 0);

    // The wider offset starts further left, so the two right edges line up.
    const auto tag_ink =
        ink_span(image, view.cell_rect(fixture.cell(fixture.node_id(0), ReclassStructureModel::offset)).value());
    const auto tail_ink =
        ink_span(image, view.cell_rect(fixture.cell(fixture.node_id(3), ReclassStructureModel::offset)).value());
    REQUIRE(tag_ink.has_value());
    REQUIRE(tail_ink.has_value());
    CHECK(tail_ink->first < tag_ink->first);
    CHECK(tail_ink->second == tag_ink->second);
}

TEST_CASE("a long note keeps its full width and the pane scrolls instead", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    const QFontMetrics prose(slopkit::ui::ui_font());
    const int          header = prose.horizontalAdvance(QStringLiteral("Comment"));

    // With no notes the column is just its header word wide.
    const int bare = view.column_width(ReclassStructureModel::comment);
    CHECK(bare >= header);
    CHECK(bare <= header + 24);

    // A note widens it to fit the prefixed text.
    fixture.project.classes()[1].nodes[0]->comment = "the health field";
    fixture.structure.rebuild();
    QCoreApplication::processEvents();

    const int needed = prose.horizontalAdvance(QStringLiteral("// the health field"));
    CHECK(view.column_width(ReclassStructureModel::comment) >= needed);

    // A note longer than a third of the pane keeps its full width instead of
    // being clamped, and the row overflowing the pane makes it scroll.
    fixture.project.classes()[1].nodes[0]->comment =
        "a note so long that it would otherwise swallow the whole value column and then some";
    fixture.structure.rebuild();
    QCoreApplication::processEvents();

    const int note = prose.horizontalAdvance(
        QStringLiteral("// a note so long that it would otherwise swallow the whole value column and then some"));
    CHECK(view.column_width(ReclassStructureModel::comment) >= note);
    CHECK(view.column_width(ReclassStructureModel::comment) > view.viewport()->width() / 3);
    CHECK(view.column_width(ReclassStructureModel::value) > 0);
    CHECK(view.horizontalScrollBar()->maximum() > 0);
}

TEST_CASE("clicking a row selects it and shift extends the selection", "[reclass][ui][view]")
{
    application();

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    int changes = 0;
    QObject::connect(&view,
                     &ReClassView::selectionChanged,
                     [&changes]
                     {
                         ++changes;
                     });

    const int tag_column = slopkit::ui::models::ReclassStructureModel::name;

    click(view, fixture.cell(fixture.node_id(0), tag_column));
    CHECK(changes == 1);
    CHECK(node_at(fixture.structure, view.currentIndex()) == fixture.node_id(0));
    CHECK(selected_nodes(view, fixture.structure) == std::vector<std::optional<std::size_t>> {fixture.node_id(0)});

    // The keyboard moves the cursor and selects the row it lands on.
    QKeyEvent down {QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier};
    QApplication::sendEvent(&view, &down);
    CHECK(node_at(fixture.structure, view.currentIndex()) == fixture.node_id(1));

    QKeyEvent up {QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier};
    QApplication::sendEvent(&view, &up);
    CHECK(node_at(fixture.structure, view.currentIndex()) == fixture.node_id(0));

    // Shift extends from the cursor, Control toggles one row.
    click(view, fixture.cell(fixture.node_id(1), tag_column), Qt::ShiftModifier);
    CHECK(selected_nodes(view, fixture.structure)
          == std::vector<std::optional<std::size_t>> {fixture.node_id(0), fixture.node_id(1)});

    click(view, fixture.cell(fixture.node_id(0), tag_column), Qt::ControlModifier);
    CHECK(selected_nodes(view, fixture.structure) == std::vector<std::optional<std::size_t>> {fixture.node_id(1)});
}

TEST_CASE("a right-click selects the row it was opened over", "[reclass][ui][view]")
{
    application();

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    const int tag_column = slopkit::ui::models::ReclassStructureModel::name;

    int menus = 0;
    QObject::connect(&view,
                     &ReClassView::rowMenuRequested,
                     [&menus](const QPoint&)
                     {
                         ++menus;
                     });

    click(view, fixture.cell(fixture.node_id(1), tag_column));
    CHECK(node_at(fixture.structure, view.currentIndex()) == fixture.node_id(1));

    // The reference moves the selection to the node the menu was opened over, so
    // the commands it carries act on that row rather than on a stale selection.
    const QPoint      point = view.cell_rect(fixture.cell(fixture.node_id(0), tag_column)).value().center();
    QContextMenuEvent menu {QContextMenuEvent::Mouse, point, view.viewport()->mapToGlobal(point)};
    QApplication::sendEvent(view.viewport(), &menu);

    CHECK(menus == 1);
    CHECK(node_at(fixture.structure, view.currentIndex()) == fixture.node_id(0));
    CHECK(selected_nodes(view, fixture.structure) == std::vector<std::optional<std::size_t>> {fixture.node_id(0)});

    // A multiple selection is left alone, so a command can still act on all of it.
    click(view, fixture.cell(fixture.node_id(1), tag_column), Qt::ControlModifier);
    REQUIRE(selected_nodes(view, fixture.structure).size() == 2);
    QApplication::sendEvent(view.viewport(), &menu);

    CHECK(menus == 2);
    CHECK(selected_nodes(view, fixture.structure).size() == 2);
}

TEST_CASE("toggling a reference row inlines its class and keeps the cursor", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    const QModelIndex pointer = fixture.cell(fixture.node_id(1), ReclassStructureModel::name);
    CHECK_FALSE(view.isExpanded(pointer));
    CHECK(view.row_count() == 2);

    view.setCurrentIndex(pointer);
    REQUIRE(node_at(fixture.structure, view.currentIndex()) == fixture.node_id(1));

    // The disclosure triangle inlines the class the node names.
    const QPoint triangle(view.column_x(ReclassStructureModel::type) + 10,
                          view.cell_rect(pointer).value().center().y());
    mouse_event(view, QEvent::MouseButtonPress, triangle, Qt::LeftButton, Qt::LeftButton);
    QCoreApplication::processEvents();

    // The rebuild replaced every row object; the cursor stayed on the node.
    CHECK(view.isExpanded(fixture.cell(fixture.node_id(1), ReclassStructureModel::name)));
    CHECK(view.row_count() == 4);
    CHECK(node_at(fixture.structure, view.currentIndex()) == fixture.node_id(1));
    CHECK(selected_nodes(view, fixture.structure) == std::vector<std::optional<std::size_t>> {fixture.node_id(1)});

    const QModelIndex inlined = fixture.structure.index(0, ReclassStructureModel::name, fixture.structure.index(1, 0));
    CHECK(view.cell_text(inlined).toStdString() == "x");

    // A double-click on a cell that is not typed into collapses it again; the
    // name cell of this row now belongs to the name editor.
    double_click(view, fixture.cell(fixture.node_id(1), ReclassStructureModel::offset));
    QCoreApplication::processEvents();

    CHECK_FALSE(view.isExpanded(fixture.cell(fixture.node_id(1), ReclassStructureModel::name)));
    CHECK(view.row_count() == 2);
    CHECK(node_at(fixture.structure, view.currentIndex()) == fixture.node_id(1));
}

TEST_CASE("a double-click on a value cell edits the plain value and writes it", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    fixture.put(fixture.player_address(), {0x2A, 0xAA, 0xAA, 0xAA});
    fixture.resolve();
    fixture.pass();

    QStringList messages;
    QObject::connect(&fixture.values,
                     &ReclassValueModel::statusChanged,
                     [&messages](const QString& message, bool)
                     {
                         messages.push_back(message);
                     });

    const QModelIndex value_cell = fixture.cell(fixture.node_id(0), ReclassStructureModel::value);
    REQUIRE(fixture.values.editable(*fixture.structure.row_for(value_cell)));
    CHECK(view.cell_text(value_cell).toStdString() == "= 42  0x2A");

    // The value cell of an editable node offers itself to the editor, and so
    // does the node's own name.
    CHECK(fixture.proxy.flags(fixture.proxy.index(0, ReclassStructureModel::value)).testFlag(Qt::ItemIsEditable));
    CHECK(fixture.proxy.flags(fixture.proxy.index(0, ReclassStructureModel::name)).testFlag(Qt::ItemIsEditable));

    double_click(view, value_cell);
    QCoreApplication::processEvents();

    QLineEdit* editor = view.editor();
    REQUIRE(editor != nullptr);
    CHECK(editor->isVisible());
    // The editor starts from the value a write parses, not from the readings
    // the cell draws beside it.
    CHECK(editor->text().toStdString() == "42");

    editor->setText(QStringLiteral("-7"));
    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QApplication::sendEvent(editor, &enter);

    REQUIRE(drain_until_idle(fixture.worker));
    REQUIRE(messages.size() == 1);
    CHECK(messages.first().toStdString() == "Value written.");
    CHECK_FALSE(editor->isVisible());
    CHECK(fixture.holds(fixture.player_address(), {0xF9, 0xAA, 0xAA, 0xAA}));

    fixture.pass();
    CHECK(view.cell_text(value_cell).toStdString() == "= -7  0xF9");
}

TEST_CASE("a double-click on a name cell renames the node in place", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    QStringList messages;
    QObject::connect(&fixture.structure,
                     &ReclassStructureModel::statusChanged,
                     [&messages](const QString& message, bool)
                     {
                         messages.push_back(message);
                     });

    Class& player = fixture.project.classes()[1];

    // A name costs no target access, so the cell is typed into whether or not a
    // target is attached; the row's other columns are not.
    const QModelIndex name_cell   = fixture.cell(fixture.node_id(0), ReclassStructureModel::name);
    const QModelIndex offset_cell = fixture.cell(fixture.node_id(0), ReclassStructureModel::offset);
    CHECK(fixture.proxy.flags(name_cell).testFlag(Qt::ItemIsEditable));
    CHECK_FALSE(fixture.proxy.flags(offset_cell).testFlag(Qt::ItemIsEditable));

    const int name_width = view.column_width(ReclassStructureModel::name);

    double_click(view, name_cell);
    QCoreApplication::processEvents();

    QLineEdit* editor = view.editor();
    REQUIRE(editor != nullptr);
    CHECK(editor->isVisible());
    // The editor holds the name itself, not the readings a value cell draws.
    CHECK(editor->text().toStdString() == "tag");

    editor->setText(QStringLiteral("health"));
    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();

    CHECK_FALSE(editor->isVisible());
    CHECK(player.nodes[0]->name == "health");
    REQUIRE(messages.size() == 1);
    CHECK(messages.first().toStdString() == "Node renamed.");
    CHECK(view.cell_text(fixture.cell(fixture.node_id(0), ReclassStructureModel::name)).toStdString() == "health");
    // A longer name widens the column it draws in.
    CHECK(view.column_width(ReclassStructureModel::name) > name_width);

    // Escape leaves the node as it was.
    double_click(view, fixture.cell(fixture.node_id(0), ReclassStructureModel::name));
    QCoreApplication::processEvents();
    editor->setText(QStringLiteral("discarded"));
    QKeyEvent escape {QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};
    QApplication::sendEvent(editor, &escape);
    CHECK_FALSE(editor->isVisible());
    CHECK(player.nodes[0]->name == "health");
    CHECK(messages.size() == 1);

    // An empty name is refused with its reason, and the editor stays open for
    // the correction.
    double_click(view, fixture.cell(fixture.node_id(0), ReclassStructureModel::name));
    QCoreApplication::processEvents();
    editor->setText(QStringLiteral("   "));
    QApplication::sendEvent(editor, &enter);
    CHECK(editor->isVisible());
    CHECK(player.nodes[0]->name == "health");
    REQUIRE(messages.size() == 2);
    CHECK(messages.last().toStdString() == "A node name is required.");

    QApplication::sendEvent(editor, &escape);

    // F2 opens the name of the cursor row, which needs no attached target.
    view.setCurrentIndex(fixture.cell(fixture.node_id(0), ReclassStructureModel::name));
    QKeyEvent f2 {QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier};
    QApplication::sendEvent(&view, &f2);
    QCoreApplication::processEvents();
    CHECK(editor->isVisible());
    CHECK(editor->text().toStdString() == "health");
}

TEST_CASE("a value keeps its measured width and the pane scrolls instead", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;

    // A hex64 node after Player's own fields, its dump the widest value the
    // class draws.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(1600, 200);
    view.show();
    QCoreApplication::processEvents();

    fixture.resolve();
    fixture.put(fixture.player_address() + 9, {0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89});
    fixture.pass();

    const QModelIndex value_cell = fixture.cell(fixture.node_id(2), ReclassStructureModel::value);
    CHECK(view.cell_text(value_cell).toStdString() == "AB CD EF 01 23 45 67 89");

    // A pane wide enough for the class shows no bar and reads the value whole.
    CHECK(view.horizontalScrollBar()->maximum() == 0);
    CHECK(view.hover_full_text(view.cell_rect(value_cell).value().center()).isEmpty());

    // A pane too narrow for the row no longer takes the value field down: the
    // field keeps the width the class measured and the pane scrolls instead.
    const int value_width = view.column_width(ReclassStructureModel::value);
    view.resize(900, 200);
    QCoreApplication::processEvents();

    CHECK(view.column_width(ReclassStructureModel::value) == value_width);
    CHECK(view.horizontalScrollBar()->maximum() > 0);
}

TEST_CASE("the pane scrolls the whole row without narrowing a field", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;

    // A hex64 row makes the class wider than the pane.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    QScrollBar* bar = view.horizontalScrollBar();
    REQUIRE(bar->maximum() > 0);

    // Every field starts where the one before it ended, at every scroll offset.
    const auto tiles = [&view]
    {
        for (int column = 1; column < ReclassStructureModel::column_count; ++column)
        {
            REQUIRE(view.column_x(column) == view.column_x(column - 1) + view.column_width(column - 1));
        }
    };

    std::vector<int> widths;
    for (int column = 0; column < ReclassStructureModel::column_count; ++column)
    {
        widths.push_back(view.column_width(column));
    }
    const int left = view.column_x(ReclassStructureModel::offset);
    tiles();

    // Narrowing the pane past the row leaves every width alone and only grows
    // the bar's range.
    const int range = bar->maximum();
    view.resize(500, 200);
    QCoreApplication::processEvents();
    CHECK(bar->maximum() > range);
    for (int column = 0; column < ReclassStructureModel::column_count; ++column)
    {
        CHECK(view.column_width(column) == widths[static_cast<std::size_t>(column)]);
    }

    // Scrolling shifts all nine fields together.
    bar->setValue(bar->maximum() / 2);
    QCoreApplication::processEvents();
    CHECK(view.column_x(ReclassStructureModel::offset) < left);
    tiles();

    // At the far end the trailing pointer field sits fully inside the pane, so
    // the end of the row stays reachable.
    bar->setValue(bar->maximum());
    QCoreApplication::processEvents();
    tiles();
    CHECK(view.column_x(ReclassStructureModel::pointer) >= 0);
    CHECK(view.column_x(ReclassStructureModel::pointer) + view.column_width(ReclassStructureModel::pointer)
          <= view.viewport()->width());

    // A click on the scrolled pointer cell lands on the row it covers.
    click(view, fixture.cell(fixture.node_id(2), ReclassStructureModel::pointer));
    QCoreApplication::processEvents();
    CHECK(node_at(fixture.structure, view.currentIndex()) == fixture.node_id(2));

    // A pane wide enough for the class hides the bar and left-aligns the row.
    view.resize(2000, 200);
    QCoreApplication::processEvents();
    CHECK(bar->maximum() == 0);
    CHECK(view.column_x(ReclassStructureModel::offset) == left);
}

TEST_CASE("scrolling the pane closes the inline editor", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;
    Class&  player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    // The name cell is visible at offset zero, and opens its editor.
    double_click(view, fixture.cell(fixture.node_id(0), ReclassStructureModel::name));
    QCoreApplication::processEvents();
    REQUIRE(view.editor() != nullptr);
    REQUIRE(view.editor()->isVisible());

    // A horizontal scroll re-places every cell, so the editor closes rather than
    // float over the wrong field.
    QScrollBar* bar = view.horizontalScrollBar();
    REQUIRE(bar->maximum() > 0);
    bar->setValue(bar->maximum() / 2);
    QCoreApplication::processEvents();
    CHECK_FALSE(view.editor()->isVisible());
}

TEST_CASE("the grid stripes its rows and colours the cells the model colours", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture     fixture;
    PaintedView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(500, 160);
    view.show();
    QCoreApplication::processEvents();

    QImage image(view.viewport()->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    view.paint_into(image);

    const QModelIndex first  = fixture.cell(fixture.node_id(0), ReclassStructureModel::name);
    const QModelIndex second = fixture.cell(fixture.node_id(1), ReclassStructureModel::name);

    // Every other row is striped, so the rows read apart without a grid line.
    const QColor even = image.pixelColor(2, view.cell_rect(first).value().center().y());
    const QColor odd  = image.pixelColor(2, view.cell_rect(second).value().center().y());
    CHECK(even.rgb() == slopkit::ui::active_theme().surface.rgb());
    CHECK(odd.rgb() == slopkit::ui::active_theme().surface_hover.rgb());

    // The rows paint the colour the model states and leave the rest to the grid.
    const auto colour_of = [&fixture](std::size_t node_id, int column)
    {
        return fixture.proxy.data(fixture.cell(node_id, column), Qt::ForegroundRole);
    };
    CHECK(colour_of(fixture.node_id(0), ReclassStructureModel::offset).value<QColor>()
          == slopkit::ui::active_theme().grid_offset);
    CHECK(colour_of(fixture.node_id(0), ReclassStructureModel::type).value<QColor>()
          == slopkit::ui::active_theme().datatype_signed);
    CHECK_FALSE(colour_of(fixture.node_id(0), ReclassStructureModel::name).isValid());
}

TEST_CASE("a moved hex byte pair warns while its neighbour keeps the grid colour", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;

    // A hex16 node after Player's own fields, so the pair sits on its own row.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex16, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    PaintedView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(1000, 200);
    view.show();
    QCoreApplication::processEvents();

    fixture.resolve();
    fixture.put(fixture.player_address(), {0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0F, 0x10, 0});
    fixture.pass();

    // Only the second byte moves.
    fixture.put(fixture.player_address() + 10, {0x00});
    fixture.pass();
    QCoreApplication::processEvents();

    const QModelIndex value_cell = fixture.cell(fixture.node_id(2), ReclassStructureModel::value);
    const QRect       cell       = view.cell_rect(value_cell).value();

    QImage image(view.viewport()->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    view.paint_into(image);

    const QFontMetrics mono(slopkit::ui::mono_font());
    const int          first_w = mono.horizontalAdvance(QStringLiteral("0F"));
    const QRect        first(cell.left() + 4, cell.top(), first_w, cell.height());
    const QRect        second(
        cell.left() + 4 + first_w, cell.top(), mono.horizontalAdvance(QStringLiteral(" 00")), cell.height());

    const slopkit::ui::Theme& theme = slopkit::ui::active_theme();
    CHECK(paints_ink(image, second, theme.warning));
    CHECK_FALSE(paints_ink(image, second, theme.text));
    CHECK(paints_ink(image, first, theme.text));
    CHECK_FALSE(paints_ink(image, first, theme.warning));

    // A selected row paints its value whole in the on-accent colour instead.
    click(view, value_cell);
    QCoreApplication::processEvents();
    image.fill(Qt::transparent);
    view.paint_into(image);
    CHECK(paints_ink(image, second, theme.on_accent));
    CHECK_FALSE(paints_ink(image, second, theme.warning));
}

TEST_CASE("the hint field paints a hex node's readings in the readout colour", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;
    Class&  player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex32, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    PaintedView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(1600, 200);
    view.show();
    QCoreApplication::processEvents();

    fixture.resolve();
    fixture.put(fixture.player_address(), {0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1A, 0x2B, 0x3C, 0x4D});
    fixture.pass();
    // The hex32's second byte moves.
    fixture.put(fixture.player_address() + 10, {0x00});
    fixture.pass();
    QCoreApplication::processEvents();

    const QModelIndex value_cell = fixture.cell(fixture.node_id(2), ReclassStructureModel::value);
    const QModelIndex hint_cell  = fixture.cell(fixture.node_id(2), ReclassStructureModel::hints);

    QImage image(view.viewport()->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    view.paint_into(image);

    const slopkit::ui::Theme& theme = slopkit::ui::active_theme();

    // The moved byte warns in the value field, while the readings sit apart from
    // it in the readout colour and take no part in the warning.
    CHECK(paints_ink(image, view.cell_rect(value_cell).value(), theme.warning));
    CHECK(paints_ink(image, view.cell_rect(hint_cell).value(), theme.datatype_readout));
    CHECK_FALSE(paints_ink(image, view.cell_rect(hint_cell).value(), theme.warning));
}

TEST_CASE("a kind with no byte pairs warns its whole value", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;
    fixture.structure.set_class(0);

    PaintedView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    const std::size_t x_node = fixture.project.classes()[0].nodes[0]->id;

    // Point's int32 x moves, so the whole `= 43  0x0000002B` is a moved value.
    fixture.resolve();
    fixture.put(kAppBase + 0x20, {0x2A, 0, 0, 0, 0, 0, 0, 0});
    fixture.pass();
    fixture.put(kAppBase + 0x20, {0x2B, 0, 0, 0, 0, 0, 0, 0});
    fixture.pass();
    QCoreApplication::processEvents();

    const QModelIndex value_cell = fixture.cell(x_node, ReclassStructureModel::value);
    CHECK(view.cell_text(value_cell).toStdString() == "= 43  0x0000002B");

    const QRect cell = view.cell_rect(value_cell).value();
    QImage      image(view.viewport()->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    view.paint_into(image);

    const QRect               band(cell.left() + 4, cell.top(), cell.width() - 4, cell.height());
    const slopkit::ui::Theme& theme = slopkit::ui::active_theme();
    CHECK(paints_ink(image, band, theme.warning));
    CHECK_FALSE(paints_ink(image, band, theme.text));
}