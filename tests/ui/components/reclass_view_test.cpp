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
#include "reclass/nodes/text.hpp"
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
    using slopkit::reclass::NodeReading;
    using slopkit::reclass::Project;
    using slopkit::test::array_of;
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

    void double_click_at(ReClassView& view, const QPoint& point)
    {
        mouse_event(view, QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
    }

    void click_at(ReClassView& view, const QPoint& point, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        mouse_event(view, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, modifiers);
        mouse_event(view, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton, modifiers);
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

TEST_CASE("the grid holds its three measured fields and one content slot", "[reclass][ui][view]")
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

    // The fields keep the order the row reads in: offset, address, ascii, then
    // the row's own content and the notes that annotate it.
    CHECK(ReclassStructureModel::offset < ReclassStructureModel::address);
    CHECK(ReclassStructureModel::address < ReclassStructureModel::ascii);
    CHECK(ReclassStructureModel::ascii < ReclassStructureModel::type);
    CHECK(ReclassStructureModel::type < ReclassStructureModel::name);
    CHECK(ReclassStructureModel::name < ReclassStructureModel::value);
    CHECK(ReclassStructureModel::value < ReclassStructureModel::comment);
    CHECK(ReclassStructureModel::comment < ReclassStructureModel::hints);
    CHECK(ReclassStructureModel::hints < ReclassStructureModel::pointer);

    const std::size_t tag  = fixture.node_id(0);
    const auto        rect = [&view, &fixture](std::size_t node, int column)
    {
        return view.cell_rect(fixture.cell(node, column)).value();
    };

    // The three leading fields are measured from the class and tile each other.
    CHECK(rect(tag, ReclassStructureModel::offset).width() > 0);
    CHECK(rect(tag, ReclassStructureModel::address).width() > 0);
    CHECK(rect(tag, ReclassStructureModel::ascii).width() > 0);
    CHECK(rect(tag, ReclassStructureModel::address).left()
          == rect(tag, ReclassStructureModel::offset).left() + rect(tag, ReclassStructureModel::offset).width());
    CHECK(rect(tag, ReclassStructureModel::ascii).left()
          == rect(tag, ReclassStructureModel::address).left() + rect(tag, ReclassStructureModel::address).width());

    // The row's own content starts at one slot right after the ascii field, and
    // its fields flow one behind the other, an empty field leaving no room and
    // no gap: the tag row draws no readings and no pointer.
    const int head =
        rect(tag, ReclassStructureModel::ascii).left() + rect(tag, ReclassStructureModel::ascii).width() + 4;
    CHECK(rect(tag, ReclassStructureModel::type).left() == head);
    CHECK(rect(tag, ReclassStructureModel::name).left()
          == rect(tag, ReclassStructureModel::type).left() + rect(tag, ReclassStructureModel::type).width() + 12);
    CHECK(rect(tag, ReclassStructureModel::value).left()
          == rect(tag, ReclassStructureModel::name).left() + rect(tag, ReclassStructureModel::name).width() + 12);
    CHECK(rect(tag, ReclassStructureModel::comment).left()
          == rect(tag, ReclassStructureModel::value).left() + rect(tag, ReclassStructureModel::value).width() + 6);
    CHECK(rect(tag, ReclassStructureModel::hints).width() == 0);
    CHECK(rect(tag, ReclassStructureModel::pointer).width() == 0);
    CHECK(rect(tag, ReclassStructureModel::pointer).left() == rect(tag, ReclassStructureModel::hints).left());

    // A row with no note draws the bare marker, and the pane's leftover width
    // trails after the row instead of being swallowed by a value cell.
    CHECK(view.cell_text(fixture.cell(tag, ReclassStructureModel::comment)).toStdString() == "//");
    const int end =
        rect(tag, ReclassStructureModel::comment).left() + rect(tag, ReclassStructureModel::comment).width();
    CHECK(end < view.viewport()->width());

    const QModelIndex name_cell = fixture.cell(tag, ReclassStructureModel::name);
    CHECK(view.cell_text(name_cell).toStdString() == "tag");
    const int address_x = rect(tag, ReclassStructureModel::address).left();
    const int ascii_x   = rect(tag, ReclassStructureModel::ascii).left();
    const int note_x    = rect(tag, ReclassStructureModel::comment).left();
    const int first_top = view.cell_rect(name_cell).value().top();

    // The geometry follows the class, never the pane: a wider pane only moves
    // the slack.
    view.resize(1400, 200);
    QCoreApplication::processEvents();

    CHECK(rect(tag, ReclassStructureModel::address).left() == address_x);
    CHECK(rect(tag, ReclassStructureModel::ascii).left() == ascii_x);
    CHECK(rect(tag, ReclassStructureModel::comment).left() == note_x);

    // No header band: the first row starts at the view's own top margin.
    CHECK(view.cell_rect(name_cell).value().top() == first_top);
    CHECK(first_top <= 8);
}

TEST_CASE("the leading fields are sized to the class and the row flows after them", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;

    // `tail` sits at offset 0x11, so the class draws a two-digit offset beside
    // `tag`'s one-digit one; `raw` is the widest row the class draws.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, ""));
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex8, "tail"));
    player.nodes[2]->comment = "raw bytes";
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    PaintedView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    const QFontMetrics mono(slopkit::ui::mono_font());
    // Player holds `tag` and a `Point` pointer: the leading fields must fit the
    // texts this class draws and leave one gutter after them, not the room the
    // widest kind in the language and a full-width address would need.
    constexpr int      kAllowedGap = 24;

    const std::size_t tag  = fixture.node_id(0);
    const std::size_t raw  = fixture.node_id(2);
    const std::size_t tail = fixture.node_id(3);
    const auto        rect = [&view, &fixture](std::size_t node, int column)
    {
        return view.cell_rect(fixture.cell(node, column)).value();
    };

    const int offset_widest = mono.horizontalAdvance(QStringLiteral("11"));
    CHECK(rect(tail, ReclassStructureModel::offset).width() >= offset_widest);
    CHECK(rect(tail, ReclassStructureModel::offset).width() <= offset_widest + kAllowedGap);

    // The ascii field is sized for the bytes the kinds draw, not for the text a
    // row happens to hold.
    const int ascii_widest = mono.horizontalAdvance(QStringLiteral("WWWWWWWW"));
    CHECK(rect(tag, ReclassStructureModel::ascii).width() >= ascii_widest);
    CHECK(rect(tag, ReclassStructureModel::ascii).width() <= ascii_widest + kAllowedGap);

    // An unnamed hex row draws no type and no name, so its dump starts at the
    // head of the content slot - the same x the defined row's type starts at.
    CHECK(rect(raw, ReclassStructureModel::type).width() == 0);
    CHECK(rect(raw, ReclassStructureModel::name).width() == 0);
    CHECK(rect(raw, ReclassStructureModel::value).left() == rect(tag, ReclassStructureModel::type).left());
    // A named hex row draws its name at that head and its dump behind the name.
    CHECK(rect(tail, ReclassStructureModel::name).left() == rect(tag, ReclassStructureModel::type).left());
    CHECK(rect(tail, ReclassStructureModel::value).left()
          == rect(tail, ReclassStructureModel::name).left() + rect(tail, ReclassStructureModel::name).width() + 12);

    const int ascii_left = rect(raw, ReclassStructureModel::ascii).left();
    const int slot_left  = rect(raw, ReclassStructureModel::value).left();

    // `raw` reads a pointer into the image, `tail` a plain byte, so the two rows
    // carry values of plainly different lengths.
    fixture.resolve();
    fixture.put(fixture.player_address(),
                {1, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42});
    fixture.pass();
    QCoreApplication::processEvents();

    const QModelIndex raw_value = fixture.cell(raw, ReclassStructureModel::value);
    const QModelIndex raw_hint  = fixture.cell(raw, ReclassStructureModel::hints);
    CHECK(view.cell_text(raw_value).toStdString() == "00 01 40 00 00 00 00 00");
    CHECK(view.cell_text(fixture.cell(tail, ReclassStructureModel::value)).toStdString() == "42");

    // The note follows the row's own value, the readings follow the note and the
    // pointer follows the readings, so a row reads as one group.
    const QRect raw_value_rect = rect(raw, ReclassStructureModel::value);
    const QRect raw_note       = rect(raw, ReclassStructureModel::comment);
    const QRect raw_readings   = rect(raw, ReclassStructureModel::hints);
    const QRect raw_pointer    = rect(raw, ReclassStructureModel::pointer);
    CHECK(raw_note.left() == raw_value_rect.left() + raw_value_rect.width() + 6);
    CHECK(raw_readings.left() == raw_note.left() + raw_note.width() + 6);
    CHECK(raw_pointer.left() == raw_readings.left() + raw_readings.width() + 6);
    CHECK(view.cell_text(raw_hint).startsWith(QStringLiteral("(")));

    // A longer value carries that row's note further right than the short one's.
    const QRect tail_value = rect(tail, ReclassStructureModel::value);
    const QRect tail_note  = rect(tail, ReclassStructureModel::comment);
    CHECK(tail_value.width() < raw_value_rect.width());
    CHECK(tail_note.left() == tail_value.left() + tail_value.width() + 6);
    CHECK(tail_note.left() < raw_note.left());

    // The reading fits its own run, which is sized for the widest reading a hex
    // cell can draw rather than for what the class happens to read now.
    CHECK(view.hover_full_text(view.cell_rect(raw_hint).value().center()).isEmpty());

    // A live pass repaints the row without re-measuring: the leading fields and
    // the head of the content slot keep their place.
    fixture.put(fixture.player_address(),
                {1, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43});
    fixture.pass();
    QCoreApplication::processEvents();
    CHECK(rect(raw, ReclassStructureModel::ascii).left() == ascii_left);
    CHECK(rect(raw, ReclassStructureModel::value).left() == slot_left);

    // Widening a row's own value shifts that row's note with it, while the range
    // the scrollbar spans stays put: the widest drawn row did not move.
    const int range       = view.horizontalScrollBar()->maximum();
    const int note_before = rect(tag, ReclassStructureModel::comment).left();
    fixture.put(fixture.player_address(), {0x7F, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x01, 0x40, 0, 0, 0, 0, 0, 0x42});
    fixture.pass();
    QCoreApplication::processEvents();
    CHECK(rect(tag, ReclassStructureModel::comment).left() > note_before);
    CHECK(view.horizontalScrollBar()->maximum() == range);

    // The class's own text sizes the row: a longer name widens that row's run.
    const int name_width                        = rect(tag, ReclassStructureModel::name).width();
    fixture.project.classes()[1].nodes[0]->name = "a_much_longer_field_name";
    fixture.structure.rebuild();
    QCoreApplication::processEvents();
    CHECK(rect(tag, ReclassStructureModel::name).width() > name_width);

    // The offset field sits on its content box, so the one-digit offset of `tag`
    // and the two-digit one of `tail` end at the same right edge.
    QImage image(view.viewport()->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    view.paint_into(image);

    const auto tag_ink  = ink_span(image, view.cell_rect(fixture.cell(tag, ReclassStructureModel::offset)).value());
    const auto tail_ink = ink_span(image, view.cell_rect(fixture.cell(tail, ReclassStructureModel::offset)).value());
    REQUIRE(tag_ink.has_value());
    REQUIRE(tail_ink.has_value());
    CHECK(tail_ink->first < tag_ink->first);
    CHECK(tail_ink->second == tag_ink->second);
}

TEST_CASE("the address field is sized to the class's addresses and right-aligned", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;
    // The window repaints the live fields through the proxy's cache, which the
    // dialog wires up the same way.
    QObject::connect(
        &fixture.values, &ReclassValueModel::valuesChanged, &fixture.proxy, &ReclassProxyModel::refresh_values);

    PaintedView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    const QFontMetrics mono(slopkit::ui::mono_font());
    constexpr int      kAllowedGap = 24;

    // While nothing is read the column holds the model's own wording, sized for
    // it and not for the 16 padded digits the old form reserved.
    const std::size_t tag           = fixture.node_id(0);
    const std::size_t x_node        = fixture.project.classes()[0].nodes[0]->id;
    // The address field's measured width, which every row shares.
    const auto        address_width = [&view, &fixture, tag]
    {
        return view.cell_rect(fixture.cell(tag, ReclassStructureModel::address)).value().width();
    };
    CHECK(view.cell_text(fixture.cell(tag, ReclassStructureModel::address)) == QStringLiteral("not resolved yet"));
    const int placeholder = mono.horizontalAdvance(QStringLiteral("not resolved yet"));
    const int unresolved  = address_width();
    CHECK(unresolved >= placeholder);
    CHECK(unresolved <= placeholder + kAllowedGap);
    CHECK(unresolved < mono.horizontalAdvance(QStringLiteral("0000000000000000")) + kAllowedGap);

    // The repaint cache is primed while the addresses are still unknown, the
    // way the first pass after the class is shown primes it; the address that
    // only resolves afterwards then re-measures the column.
    fixture.proxy.refresh_values();
    fixture.resolve();
    QCoreApplication::processEvents();

    const int widest  = mono.horizontalAdvance(QStringLiteral("400011"));
    const int settled = address_width();
    CHECK(view.cell_text(fixture.cell(tag, ReclassStructureModel::address)) == QStringLiteral("400010"));
    CHECK(settled >= widest);
    CHECK(settled <= widest + kAllowedGap);
    CHECK(settled < unresolved);

    // Point's own fields sit wherever `next` points, so a pointer block that
    // retargets to a one-digit address makes the class draw rows of plainly
    // different address lengths.
    fixture.put(fixture.player_address(), {0x2A, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.pass();
    fixture.structure.set_expanded(fixture.node_id(1), true);
    fixture.pass();
    QCoreApplication::processEvents();

    REQUIRE(fixture.cell(x_node, ReclassStructureModel::address).isValid());
    CHECK(view.cell_text(fixture.cell(x_node, ReclassStructureModel::address)) == QStringLiteral("1"));
    CHECK(address_width() == settled);

    QImage image(view.viewport()->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    view.paint_into(image);

    const auto ink_of = [&](std::size_t node_id) -> std::optional<std::pair<int, int>>
    {
        return ink_span(image, view.cell_rect(fixture.cell(node_id, ReclassStructureModel::address)).value());
    };

    // The shorter address starts further right, so the two low ends line up.
    const auto tag_ink   = ink_of(tag);
    const auto child_ink = ink_of(x_node);
    REQUIRE(tag_ink.has_value());
    REQUIRE(child_ink.has_value());
    CHECK(child_ink->first > tag_ink->first);
    CHECK(child_ink->second == tag_ink->second);

    // An address that resolves to a longer text than the measured one grows the
    // column to fit it.
    fixture.put(fixture.player_address(), {0x2A, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.pass();
    QCoreApplication::processEvents();

    CHECK(view.cell_text(fixture.cell(x_node, ReclassStructureModel::address)) == QStringLiteral("60000000"));
    CHECK(address_width() >= mono.horizontalAdvance(QStringLiteral("60000000")));
    CHECK(address_width() > settled);
}

TEST_CASE("the ascii field fits the bytes a class draws from the kinds alone", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;

    // `raw`'s eight bytes are the widest ascii run the class draws, and the
    // hidden string is far wider still: a row that draws nothing must not widen
    // the field.
    Class&                    player  = fixture.project.classes()[1];
    slopkit::reclass::NodePtr skipped = node_of(fixture.project, NodeKind::utf8_text, "skipped");
    static_cast<slopkit::reclass::UTF8TextNode*>(skipped.get())->length = 64;
    skipped->hidden                                                     = true;
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    player.nodes.push_back(std::move(skipped));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    PaintedView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    const QFontMetrics mono(slopkit::ui::mono_font());
    constexpr int      kAllowedGap = 24;

    // The width comes from the kinds, not from what a row happens to draw right
    // now, so the widest run already fits with nothing read yet.
    const auto ascii_width = [&view, &fixture]
    {
        return view.cell_rect(fixture.cell(fixture.node_id(0), ReclassStructureModel::ascii)).value().width();
    };
    const int widest = mono.horizontalAdvance(QStringLiteral("WWWWWWWW"));
    CHECK(ascii_width() >= widest);
    CHECK(ascii_width() <= widest + kAllowedGap);

    // The bytes that arrive on the live pass are drawn whole rather than elided,
    // and the hidden array never widened the field.
    fixture.resolve();
    fixture.put(fixture.player_address(),
                {0x41, 0, 0, 0, 0, 0, 0, 0, 0, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49});
    fixture.pass();
    QCoreApplication::processEvents();

    const QModelIndex raw_ascii = fixture.cell(fixture.node_id(2), ReclassStructureModel::ascii);
    CHECK(view.cell_text(raw_ascii) == QStringLiteral("BCDEFGHI"));
    CHECK(view.hover_full_text(view.cell_rect(raw_ascii).value().center()).isEmpty());
    CHECK(ascii_width() <= widest + kAllowedGap);
}

TEST_CASE("an ascii cell draws eight characters and hovers the whole reading", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;

    // A string field of sixteen bytes: twice the eight characters a cell draws.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::utf8_text, "title"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    fixture.resolve();
    fixture.put(fixture.player_address(), {0x41, 0,    0,    0,    0,    0,    0,    0,    0,    0x48, 0x65, 0x6C,
                                           0x6C, 0x6F, 0x20, 0x57, 0x6F, 0x72, 0x6C, 0x64, 0x21, 0,    0,    0});
    fixture.pass();
    QCoreApplication::processEvents();

    const QFontMetrics mono(slopkit::ui::mono_font());
    const std::size_t  title = fixture.node_id(2);

    // The cell draws the first eight characters, and the field is measured for
    // those eight rather than for the sixteen the node holds.
    CHECK(view.cell_text(fixture.cell(title, ReclassStructureModel::ascii)) == QStringLiteral("Hello Wo"));
    CHECK(view.cell_rect(fixture.cell(title, ReclassStructureModel::ascii)).value().width()
          <= mono.horizontalAdvance(QStringLiteral("WWWWWWWW")) + 24);

    // Hovering it offers the whole reading, including what the cap hides.
    CHECK(view.hover_full_text(view.cell_rect(fixture.cell(title, ReclassStructureModel::ascii)).value().center())
          == QStringLiteral("Hello World!...."));

    // The bytes the cell still draws keep their own rects; a byte the cap hides
    // has none, and no byte-editable kind is wider than the eight a cell draws
    // in the first place, so the cap never takes a clickable byte away.
    REQUIRE(view.byte_rect(fixture.cell(title, ReclassStructureModel::ascii), 0).has_value());
    REQUIRE(view.byte_rect(fixture.cell(title, ReclassStructureModel::ascii), 7).has_value());
    CHECK_FALSE(view.byte_rect(fixture.cell(title, ReclassStructureModel::ascii), 8).has_value());

    // A row of eight bytes or fewer is unaffected: its whole reading fits the
    // cell, so it needs no hover.
    const QModelIndex pointer = fixture.cell(fixture.node_id(1), ReclassStructureModel::ascii);
    CHECK(view.cell_text(pointer).size() == 8);
    CHECK(view.hover_full_text(view.cell_rect(pointer).value().center()).isEmpty());

    // The per-byte Ascii editor is untouched: a scalar's drawn character still
    // opens over that byte.
    const QModelIndex tag = fixture.cell(fixture.node_id(0), ReclassStructureModel::ascii);
    double_click_at(view, view.byte_rect(tag, 0).value().center());
    QCoreApplication::processEvents();

    QLineEdit* editor = view.editor();
    REQUIRE(editor != nullptr);
    CHECK(editor->isVisible());
    CHECK(editor->text().toStdString() == "41");
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

    const std::size_t tag  = fixture.node_id(0);
    const auto        note = [&view, &fixture, tag]
    {
        return view.cell_rect(fixture.cell(tag, ReclassStructureModel::comment)).value();
    };

    // With no notes the row draws the bare marker and nothing reserves the room
    // a `Comment` header would have.
    const int header = prose.horizontalAdvance(QStringLiteral("Comment"));
    CHECK(note().width() == prose.horizontalAdvance(QStringLiteral("//")));
    CHECK(note().width() < header);

    // A note widens the row's own run to fit the prefixed text.
    fixture.project.classes()[1].nodes[0]->comment = "the health field";
    fixture.structure.rebuild();
    QCoreApplication::processEvents();

    const int needed = prose.horizontalAdvance(QStringLiteral("// the health field"));
    CHECK(note().width() >= needed);
    const int settled = note().left();

    // A note longer than a third of the pane keeps its full width instead of
    // being clamped, and the row overflowing the pane makes it scroll.
    fixture.project.classes()[1].nodes[0]->comment =
        "a note so long that it would otherwise swallow the whole value column and then some";
    fixture.structure.rebuild();
    QCoreApplication::processEvents();

    CHECK(note().width() >= prose.horizontalAdvance(QStringLiteral(
              "// a note so long that it would otherwise swallow the whole value column and then some")));
    CHECK(note().width() > view.viewport()->width() / 3);
    CHECK(view.cell_rect(fixture.cell(tag, ReclassStructureModel::value)).value().width() > 0);
    CHECK(note().left() == settled);
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
    const QRect  type_rect = view.cell_rect(fixture.cell(fixture.node_id(1), ReclassStructureModel::type)).value();
    const QPoint triangle(type_rect.left() - 4, type_rect.center().y());
    mouse_event(view, QEvent::MouseButtonPress, triangle, Qt::LeftButton, Qt::LeftButton);
    QCoreApplication::processEvents();

    // The rebuild replaced every row object; the cursor stayed on the node.
    CHECK(view.isExpanded(fixture.cell(fixture.node_id(1), ReclassStructureModel::name)));
    CHECK(view.row_count() == 4);
    CHECK(node_at(fixture.structure, view.currentIndex()) == fixture.node_id(1));
    CHECK(selected_nodes(view, fixture.structure) == std::vector<std::optional<std::size_t>> {fixture.node_id(1)});

    const QModelIndex inlined = fixture.structure.index(0, ReclassStructureModel::name, fixture.structure.index(1, 0));
    CHECK(view.cell_text(inlined).toStdString() == "x");

    // A quick second click on the triangle is swallowed: the press already
    // folded the row once, so it does not fold back.
    double_click_at(view, triangle);
    QCoreApplication::processEvents();
    CHECK(view.isExpanded(fixture.cell(fixture.node_id(1), ReclassStructureModel::name)));
    CHECK(view.row_count() == 4);

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
    CHECK(view.cell_text(value_cell).toStdString() == "= 42 (0x2A)");

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
    CHECK(view.cell_text(value_cell).toStdString() == "= -7 (0xF9)");
}

TEST_CASE("a hex row's dump is its own field and not the name field", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;

    // A named hex32 row after Player's own fields: its name draws at the head of
    // the content slot and the dump follows it.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex32, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    fixture.resolve();
    fixture.put(fixture.player_address(), {0, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44});
    fixture.pass();
    QCoreApplication::processEvents();

    const QModelIndex value_cell = fixture.cell(fixture.node_id(2), ReclassStructureModel::value);
    const QModelIndex name_cell  = fixture.cell(fixture.node_id(2), ReclassStructureModel::name);
    CHECK(view.cell_text(value_cell).toStdString() == "11 22 33 44");
    CHECK(view.cell_text(name_cell).toStdString() == "raw");
    CHECK(view.cell_rect(value_cell).value().left()
          == view.cell_rect(name_cell).value().left() + view.cell_rect(name_cell).value().width() + 12);

    // A hex row's whole value is not typed into, so a double-click on its dump
    // opens the editor over the one byte drawn under the point rather than over
    // the cell - and the cell keeps its text.
    double_click(view, value_cell);
    QCoreApplication::processEvents();
    REQUIRE(view.editor() != nullptr);
    CHECK(view.editor()->isVisible());
    CHECK(view.editor()->width() < view.cell_rect(value_cell).value().width());
    QKeyEvent escape {QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};
    QApplication::sendEvent(view.editor(), &escape);

    double_click(view, name_cell);
    QCoreApplication::processEvents();

    QLineEdit* editor = view.editor();
    REQUIRE(editor != nullptr);
    CHECK(editor->isVisible());
    CHECK(editor->text().toStdString() == "raw");
}

TEST_CASE("a byte of a hex dump or of the Ascii column is typed into where it is drawn", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;

    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex32, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    fixture.resolve();
    fixture.put(fixture.player_address(), {0, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44});
    fixture.pass();
    QCoreApplication::processEvents();

    QStringList messages;
    QObject::connect(&fixture.values,
                     &ReclassValueModel::statusChanged,
                     [&messages](const QString& message, bool)
                     {
                         messages.push_back(message);
                     });

    const QModelIndex dump  = fixture.cell(fixture.node_id(2), ReclassStructureModel::value);
    const QModelIndex ascii = fixture.cell(fixture.node_id(2), ReclassStructureModel::ascii);
    REQUIRE(view.cell_text(dump).toStdString() == "11 22 33 44");

    const ReclassRow* raw_row = fixture.structure.row_for(dump);
    REQUIRE(raw_row != nullptr);
    REQUIRE(fixture.values.byte_editable(*raw_row));
    const std::optional<std::uint64_t> raw = fixture.values.node_address(*raw_row);
    REQUIRE(raw.has_value());

    // The third pair of the dump: the editor opens over that pair alone, holding
    // the byte the row draws, bare and upper case.
    double_click_at(view, view.byte_rect(dump, 2).value().center());
    QCoreApplication::processEvents();

    QLineEdit* editor = view.editor();
    REQUIRE(editor != nullptr);
    CHECK(editor->isVisible());
    CHECK(editor->text().toStdString() == "33");
    CHECK(editor->width() < view.cell_rect(dump).value().width());

    editor->setText(QStringLiteral("ff"));
    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QApplication::sendEvent(editor, &enter);
    REQUIRE(drain_until_idle(fixture.worker));
    QCoreApplication::processEvents();

    CHECK_FALSE(editor->isVisible());
    CHECK(messages == QStringList {QStringLiteral("Byte written.")});
    // One byte reached the target at the node's own offset, and no other.
    CHECK(fixture.holds(*raw, {0x11, 0x22, 0xFF, 0x44}));
    // What was written shows at once instead of on the next live pass.
    CHECK(view.cell_text(dump).toStdString() == "11 22 FF 44");

    // A character of the Ascii column edits the byte that character stands for.
    double_click_at(view, view.byte_rect(ascii, 0).value().center());
    QCoreApplication::processEvents();

    REQUIRE(editor->isVisible());
    CHECK(editor->text().toStdString() == "11");

    editor->setText(QStringLiteral("#65"));
    QApplication::sendEvent(editor, &enter);
    REQUIRE(drain_until_idle(fixture.worker));
    CHECK(fixture.holds(*raw, {0x41, 0x22, 0xFF, 0x44}));

    // A click past the dump's last pair belongs to that last byte.
    const QRect cell_rect = view.cell_rect(dump).value();
    double_click_at(view, QPoint {cell_rect.right() + 2, cell_rect.center().y()});
    QCoreApplication::processEvents();

    REQUIRE(editor->isVisible());
    CHECK(editor->text().toStdString() == "44");

    // A malformed byte is refused with its reason, the node's bytes are left as
    // they were and the editor stays open with the typo selected.
    messages.clear();
    editor->setText(QStringLiteral("ZZ"));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();

    CHECK(editor->isVisible());
    CHECK(editor->selectedText().toStdString() == "ZZ");
    CHECK(messages == QStringList {QStringLiteral("Byte: expected one hex byte such as 1A.")});
    CHECK(fixture.holds(*raw, {0x41, 0x22, 0xFF, 0x44}));

    QKeyEvent escape {QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};
    QApplication::sendEvent(editor, &escape);
    CHECK_FALSE(editor->isVisible());

    // A row whose value cannot be written - here because the target is gone -
    // offers no byte editor at all.
    const QPoint first_byte = view.byte_rect(dump, 0).value().center();
    fixture.target.clear();
    double_click_at(view, first_byte);
    QCoreApplication::processEvents();
    CHECK_FALSE(editor->isVisible());
}

TEST_CASE("the Type cell's bracketed number is edited in place", "[reclass][ui][view]")
{
    application();

    Fixture fixture;

    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::utf16_text, "title"));
    player.nodes.push_back(node_of(fixture.project, NodeKind::int32, "score"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    QStringList messages;
    QObject::connect(&fixture.structure,
                     &ReclassStructureModel::statusChanged,
                     [&messages](const QString& message, bool)
                     {
                         messages.push_back(message);
                     });

    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QKeyEvent escape {QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};

    CHECK(view.cell_text(fixture.cell(fixture.node_id(2), ReclassStructureModel::type)).toStdString()
          == "Utf16Text [16]");

    // The run is offered where the cell draws it, inside the cell's own box.
    const std::optional<QRect> run = view.extent_rect(fixture.cell(fixture.node_id(2), ReclassStructureModel::type));
    REQUIRE(run.has_value());
    const QRect title_rect = view.cell_rect(fixture.cell(fixture.node_id(2), ReclassStructureModel::type)).value();
    CHECK(title_rect.contains(run->center()));
    CHECK(run->right() <= title_rect.right());

    // A double-click on the run opens the editor over it, holding the bare
    // number and no brackets.
    double_click_at(view, run->center());
    QCoreApplication::processEvents();

    QLineEdit* editor = view.editor();
    REQUIRE(editor != nullptr);
    CHECK(editor->isVisible());
    CHECK(editor->text().toStdString() == "16");

    editor->setText(QStringLiteral("32"));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();

    CHECK_FALSE(editor->isVisible());
    CHECK(messages == QStringList {QStringLiteral("Length set to 32.")});
    CHECK(view.cell_text(fixture.cell(fixture.node_id(2), ReclassStructureModel::type)).toStdString()
          == "Utf16Text [32]");

    // Esc discards what was typed and the node keeps its length.
    double_click_at(view,
                    view.extent_rect(fixture.cell(fixture.node_id(2), ReclassStructureModel::type)).value().center());
    QCoreApplication::processEvents();
    REQUIRE(editor->isVisible());
    editor->setText(QStringLiteral("99"));
    QApplication::sendEvent(editor, &escape);
    CHECK_FALSE(editor->isVisible());
    CHECK(view.cell_text(fixture.cell(fixture.node_id(2), ReclassStructureModel::type)).toStdString()
          == "Utf16Text [32]");

    // A typo is refused with its reason, the node keeps its length and the
    // editor stays open with the text selected.
    messages.clear();
    double_click_at(view,
                    view.extent_rect(fixture.cell(fixture.node_id(2), ReclassStructureModel::type)).value().center());
    QCoreApplication::processEvents();
    REQUIRE(editor->isVisible());
    editor->setText(QStringLiteral("0"));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();

    CHECK(editor->isVisible());
    CHECK(editor->selectedText().toStdString() == "0");
    CHECK(messages == QStringList {QStringLiteral("Length: expected a whole number of at least 1.")});
    CHECK(view.cell_text(fixture.cell(fixture.node_id(2), ReclassStructureModel::type)).toStdString()
          == "Utf16Text [32]");

    QApplication::sendEvent(editor, &escape);
    CHECK_FALSE(editor->isVisible());
}

TEST_CASE("a hex cell's readings are edited in place one by one", "[reclass][ui][view]")
{
    application();

    Fixture fixture;
    // The window repaints the live fields through the proxy's cache, which the
    // dialog wires up the same way.
    QObject::connect(
        &fixture.values, &ReclassValueModel::valuesChanged, &fixture.proxy, &ReclassProxyModel::refresh_values);

    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(1600, 200);
    view.show();
    QCoreApplication::processEvents();

    QStringList messages;
    QObject::connect(&fixture.values,
                     &ReclassValueModel::statusChanged,
                     [&messages](const QString& message, bool)
                     {
                         messages.push_back(message);
                     });

    fixture.resolve();
    const std::uint64_t address = fixture.player_address() + 9;
    fixture.put(address, {0x2A, 0, 0, 0, 0, 0, 0, 0});
    fixture.pass();
    QCoreApplication::processEvents();

    const std::size_t raw  = fixture.node_id(2);
    const auto        hint = [&fixture, raw]
    {
        return fixture.cell(raw, ReclassStructureModel::hints);
    };
    CHECK(view.cell_text(hint()) == QStringLiteral("(0.000) (42) (0x2A)"));

    const auto run = [&view, &hint](NodeReading reading)
    {
        return view.reading_rect(hint(), reading);
    };
    REQUIRE(run(NodeReading::floating).has_value());
    REQUIRE(run(NodeReading::integer).has_value());
    REQUIRE(run(NodeReading::raw).has_value());

    // Each run stands inside the field, in the order the field draws them and
    // one space apart, so the three readings never share a box.
    const QRect field = view.cell_rect(hint()).value();
    CHECK(field.contains(run(NodeReading::floating)->center()));
    CHECK(run(NodeReading::floating)->right() < run(NodeReading::integer)->left());
    CHECK(run(NodeReading::integer)->right() < run(NodeReading::raw)->left());

    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QKeyEvent escape {QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};

    // A double-click on the integer reading opens the editor over that run, with
    // the reading's own text selected, and Return writes the node's whole bytes.
    double_click_at(view, run(NodeReading::integer)->center());
    QCoreApplication::processEvents();
    QLineEdit* editor = view.editor();
    REQUIRE(editor != nullptr);
    CHECK(editor->isVisible());
    CHECK(editor->text() == QStringLiteral("42"));
    CHECK(editor->selectedText() == QStringLiteral("42"));
    CHECK(editor->geometry().left() == run(NodeReading::integer)->left());
    CHECK(editor->geometry().contains(run(NodeReading::integer)->center()));

    editor->setText(QStringLiteral("-1"));
    QApplication::sendEvent(editor, &enter);
    REQUIRE(drain_until_idle(fixture.worker));
    QCoreApplication::processEvents();

    CHECK_FALSE(editor->isVisible());
    CHECK(messages == QStringList {QStringLiteral("Reading written.")});
    CHECK(fixture.holds(address, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    // The field shows the new readings at once, without a live pass.
    CHECK(view.cell_text(hint()) == QStringLiteral("(NaN) (-1) (0xFFFFFFFFFFFFFFFF)"));

    // The raw bits take bare hex as hex and write the exact bytes back.
    double_click_at(view, run(NodeReading::raw)->center());
    QCoreApplication::processEvents();
    REQUIRE(editor->isVisible());
    CHECK(editor->text() == QStringLiteral("0xFFFFFFFFFFFFFFFF"));
    editor->setText(QStringLiteral("2A"));
    QApplication::sendEvent(editor, &enter);
    REQUIRE(drain_until_idle(fixture.worker));
    QCoreApplication::processEvents();
    CHECK(fixture.holds(address, {0x2A, 0, 0, 0, 0, 0, 0, 0}));

    // The float reading takes a decimal float.
    double_click_at(view, run(NodeReading::floating)->center());
    QCoreApplication::processEvents();
    REQUIRE(editor->isVisible());
    CHECK(editor->text() == QStringLiteral("0.000"));
    editor->setText(QStringLiteral("2.5"));
    QApplication::sendEvent(editor, &enter);
    REQUIRE(drain_until_idle(fixture.worker));
    QCoreApplication::processEvents();
    CHECK(fixture.holds(address, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40}));

    // Escape discards what was typed and writes nothing.
    double_click_at(view, run(NodeReading::integer)->center());
    QCoreApplication::processEvents();
    REQUIRE(editor->isVisible());
    editor->setText(QStringLiteral("99"));
    QApplication::sendEvent(editor, &escape);
    CHECK_FALSE(editor->isVisible());
    CHECK(fixture.holds(address, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40}));

    // A typo is refused with the reading's name, and the editor stays open with
    // the text selected.
    messages.clear();
    double_click_at(view, run(NodeReading::integer)->center());
    QCoreApplication::processEvents();
    REQUIRE(editor->isVisible());
    editor->setText(QStringLiteral("twelve"));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();
    CHECK(editor->isVisible());
    CHECK(editor->selectedText() == QStringLiteral("twelve"));
    CHECK(messages == QStringList {QStringLiteral("Integer: malformed number")});
    CHECK(fixture.holds(address, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40}));
    QApplication::sendEvent(editor, &escape);

    // A hints field that elides its text puts no run at a fixed place, so a
    // double-click on it offers no reading editor.
    fixture.put(address, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x7F});
    fixture.pass();
    QCoreApplication::processEvents();
    CHECK_FALSE(view.reading_rect(hint(), NodeReading::integer).has_value());
    double_click_at(view, view.cell_rect(hint()).value().center());
    QCoreApplication::processEvents();
    CHECK_FALSE(editor->isVisible());

    // Nor does a row with no target attached.
    fixture.put(address, {0x2A, 0, 0, 0, 0, 0, 0, 0});
    fixture.pass();
    QCoreApplication::processEvents();
    REQUIRE(run(NodeReading::raw).has_value());
    fixture.target.clear();
    fixture.pass();
    QCoreApplication::processEvents();
    CHECK_FALSE(view.reading_rect(hint(), NodeReading::raw).has_value());
}

TEST_CASE("a Type cell without a bracketed number keeps its own rules", "[reclass][ui][view]")
{
    application();

    Fixture fixture;

    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::bitfield, "flags"));
    player.nodes.push_back(array_of(fixture.project, 2, NodeKind::int32));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};

    const QModelIndex tag  = fixture.cell(fixture.node_id(0), ReclassStructureModel::type);
    const QModelIndex bits = fixture.cell(fixture.node_id(2), ReclassStructureModel::type);
    const QModelIndex list = fixture.cell(fixture.node_id(3), ReclassStructureModel::type);

    // A bit field brackets its width and a plain scalar brackets nothing, so
    // neither offers the run.
    CHECK_FALSE(view.extent_rect(tag).has_value());
    CHECK_FALSE(view.extent_rect(bits).has_value());

    // A double-click on a cell that brackets nothing keeps the cell's own
    // rules: no editor opens.
    double_click_at(view, view.cell_rect(tag).value().center());
    QCoreApplication::processEvents();
    QLineEdit* editor = view.editor();
    CHECK((editor == nullptr || !editor->isVisible()));

    // The array keeps the count it brackets and edits it in place.
    CHECK(view.cell_text(list).toStdString() == "Array<Int32> [2] ◀ (0) ▶");
    const std::optional<QRect> run = view.extent_rect(list);
    REQUIRE(run.has_value());
    double_click_at(view, run->center());
    QCoreApplication::processEvents();

    editor = view.editor();
    REQUIRE(editor != nullptr);
    CHECK(editor->isVisible());
    CHECK(editor->text().toStdString() == "2");

    editor->setText(QStringLiteral("4"));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();

    CHECK_FALSE(editor->isVisible());
    CHECK(view.cell_text(fixture.cell(fixture.node_id(3), ReclassStructureModel::type)).toStdString()
          == "Array<Int32> [4] ◀ (0) ▶");
}

TEST_CASE("an array row's arrows walk its elements", "[reclass][ui][view]")
{
    application();

    Fixture fixture;

    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(array_of(fixture.project, 4, NodeKind::uint8));
    player.nodes.push_back(array_of(fixture.project, 1, NodeKind::uint8));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    const std::size_t list = fixture.node_id(2);
    const auto        type = [&fixture, list]
    {
        return fixture.cell(list, ReclassStructureModel::type);
    };
    const auto control = [&view, &type]
    {
        return view.element_rects(type());
    };

    // The control sits in the Type cell, its three parts in the order the text
    // draws them, and the count it follows keeps its own run to the left.
    const std::optional<QRect> count = view.extent_rect(type());
    REQUIRE(count.has_value());
    const std::optional<ReClassView::ElementRects> rects = control();
    REQUIRE(rects.has_value());
    const QRect cell = view.cell_rect(type()).value();
    CHECK(cell.contains(rects->previous.center()));
    CHECK(cell.contains(rects->index.center()));
    CHECK(cell.contains(rects->next.center()));
    CHECK(count->right() < rects->previous.left());
    CHECK(rects->previous.right() < rects->index.left());
    CHECK(rects->index.right() < rects->next.left());

    // The forward arrow shows the next element, and the row it owns walks on
    // with it while the rest of the grid stays put.
    fixture.structure.set_expanded(list, true);
    QCoreApplication::processEvents();
    REQUIRE(view.row_count() == 5);
    CHECK(view.cell_text(type()).toStdString() == "Array<UInt8> [4] ◀ (0) ▶");

    click_at(view, control().value().next.center());
    QCoreApplication::processEvents();
    CHECK(fixture.structure.element_index(list) == 1);
    CHECK(view.row_count() == 5);
    CHECK(view.cell_text(type()).toStdString() == "Array<UInt8> [4] ◀ (1) ▶");

    // A quick second click arrives as a double-click: the arrow steps the
    // element again rather than folding the row it lives in.
    double_click_at(view, control().value().next.center());
    QCoreApplication::processEvents();
    CHECK(fixture.structure.is_expanded(list));
    CHECK(view.row_count() == 5);
    CHECK(fixture.structure.element_index(list) == 2);
    CHECK(view.cell_text(type()).toStdString() == "Array<UInt8> [4] ◀ (2) ▶");

    // The back arrow steps back, and the first element's back arrow wraps to
    // the last.
    click_at(view, control().value().previous.center());
    QCoreApplication::processEvents();
    CHECK(fixture.structure.element_index(list) == 1);
    click_at(view, control().value().previous.center());
    QCoreApplication::processEvents();
    CHECK(fixture.structure.element_index(list) == 0);
    click_at(view, control().value().previous.center());
    QCoreApplication::processEvents();
    CHECK(fixture.structure.element_index(list) == 3);
    CHECK(view.cell_text(type()).toStdString() == "Array<UInt8> [4] ◀ (3) ▶");

    // A hover over an arrow explains the element it shows and takes the
    // pointing hand.
    const QPoint arrow = control().value().next.center();
    mouse_event(view, QEvent::MouseMove, arrow, Qt::NoButton, Qt::NoButton);
    QCoreApplication::processEvents();
    CHECK(view.viewport()->cursor().shape() == Qt::PointingHandCursor);

    // A one-element array draws its control and moves nowhere on a click.
    const std::size_t single = fixture.node_id(3);
    mouse_event(view,
                QEvent::MouseMove,
                view.cell_rect(fixture.cell(single, ReclassStructureModel::type)).value().center(),
                Qt::NoButton,
                Qt::NoButton);
    QCoreApplication::processEvents();
    CHECK(view.viewport()->cursor().shape() != Qt::PointingHandCursor);

    const std::optional<ReClassView::ElementRects> one =
        view.element_rects(fixture.cell(single, ReclassStructureModel::type));
    REQUIRE(one.has_value());
    CHECK(view.cell_text(fixture.cell(single, ReclassStructureModel::type)).toStdString()
          == "Array<UInt8> [1] ◀ (0) ▶");
    click_at(view, one->next.center());
    QCoreApplication::processEvents();
    CHECK(fixture.structure.element_index(single) == 0);
    CHECK(view.cell_text(fixture.cell(single, ReclassStructureModel::type)).toStdString()
          == "Array<UInt8> [1] ◀ (0) ▶");

    // A quick second click there arrives as a double-click: the one-element
    // array moves nowhere and its row is left open rather than folded away.
    fixture.structure.set_expanded(single, true);
    QCoreApplication::processEvents();
    const std::optional<ReClassView::ElementRects> single_control =
        view.element_rects(fixture.cell(single, ReclassStructureModel::type));
    REQUIRE(single_control.has_value());
    double_click_at(view, single_control->next.center());
    QCoreApplication::processEvents();
    CHECK(fixture.structure.is_expanded(single));
    CHECK(fixture.structure.element_index(single) == 0);
    CHECK(view.cell_text(fixture.cell(single, ReclassStructureModel::type)).toStdString()
          == "Array<UInt8> [1] ◀ (0) ▶");
}

TEST_CASE("an array row's index is edited in place", "[reclass][ui][view]")
{
    application();

    Fixture fixture;

    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(array_of(fixture.project, 8, NodeKind::uint8));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    QStringList messages;
    QObject::connect(&fixture.structure,
                     &ReclassStructureModel::statusChanged,
                     [&messages](const QString& message, bool)
                     {
                         messages.push_back(message);
                     });

    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QKeyEvent escape {QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};

    const std::size_t list = fixture.node_id(2);
    const auto        type = [&fixture, list]
    {
        return fixture.cell(list, ReclassStructureModel::type);
    };
    const auto cell_text = [&view, &type]
    {
        return view.cell_text(type()).toStdString();
    };
    const auto index_rect = [&view, &type]
    {
        return view.element_rects(type()).value().index;
    };
    const auto count_rect = [&view, &type]
    {
        return view.extent_rect(type()).value();
    };

    // A double-click on the bracketed index opens the editor over it, holding
    // the bare index and no brackets.
    double_click_at(view, index_rect().center());
    QCoreApplication::processEvents();
    QLineEdit* editor = view.editor();
    REQUIRE(editor != nullptr);
    CHECK(editor->isVisible());
    CHECK(editor->text().toStdString() == "0");
    CHECK(editor->width() < view.cell_rect(type()).value().width());

    // The index typed in is applied, and one past the end wraps around.
    editor->setText(QStringLiteral("9"));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();
    CHECK_FALSE(editor->isVisible());
    CHECK(messages == QStringList {QStringLiteral("Element 2 of 8 shown.")});
    CHECK(fixture.structure.element_index(list) == 1);
    CHECK(cell_text() == "Array<UInt8> [8] ◀ (1) ▶");

    // Esc discards what was typed and the row keeps its element.
    double_click_at(view, index_rect().center());
    QCoreApplication::processEvents();
    REQUIRE(editor->isVisible());
    editor->setText(QStringLiteral("5"));
    QApplication::sendEvent(editor, &escape);
    CHECK_FALSE(editor->isVisible());
    CHECK(fixture.structure.element_index(list) == 1);

    // A text that is not a whole number is refused with its reason, the row
    // keeps its element and the editor stays open with the text selected.
    messages.clear();
    double_click_at(view, index_rect().center());
    QCoreApplication::processEvents();
    REQUIRE(editor->isVisible());
    editor->setText(QStringLiteral("next"));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();
    CHECK(editor->isVisible());
    CHECK(editor->selectedText().toStdString() == "next");
    CHECK(messages == QStringList {QStringLiteral("Index: expected a whole number.")});
    CHECK(fixture.structure.element_index(list) == 1);
    CHECK(cell_text() == "Array<UInt8> [8] ◀ (1) ▶");

    QApplication::sendEvent(editor, &escape);
    CHECK_FALSE(editor->isVisible());

    // The count's own run behind the arrows still edits the count.
    double_click_at(view, count_rect().center());
    QCoreApplication::processEvents();
    REQUIRE(editor->isVisible());
    CHECK(editor->text().toStdString() == "8");
    editor->setText(QStringLiteral("2"));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();
    CHECK_FALSE(editor->isVisible());
    CHECK(fixture.structure.element_count(*fixture.project.classes()[1].nodes[2]) == 2);
    CHECK(cell_text() == "Array<UInt8> [2] ◀ (1) ▶");

    // A two-digit index needs the room its own text takes, so the Type column
    // is measured again and the control is never clipped.
    double_click_at(view, count_rect().center());
    QCoreApplication::processEvents();
    REQUIRE(editor->isVisible());
    editor->setText(QStringLiteral("12"));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();
    CHECK(cell_text() == "Array<UInt8> [12] ◀ (1) ▶");
    const int narrow = view.cell_rect(type()).value().width();

    double_click_at(view, index_rect().center());
    QCoreApplication::processEvents();
    REQUIRE(editor->isVisible());
    CHECK(editor->text().toStdString() == "1");
    editor->setText(QStringLiteral("10"));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();
    CHECK(cell_text() == "Array<UInt8> [12] ◀ (10) ▶");
    CHECK(view.cell_rect(type()).value().width() > narrow);
}

TEST_CASE("an array's element row takes no name in the grid", "[reclass][ui][view]")
{
    application();

    Fixture fixture;

    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(array_of(fixture.project, 2, NodeKind::uint8));
    // An element can carry a name an earlier project gave it; the grid draws no
    // name for it all the same.
    static_cast<slopkit::reclass::ContainerNode&>(*player.nodes[2]).children_mutable()[0]->name = "second";
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    const std::size_t list = fixture.node_id(2);
    fixture.structure.set_expanded(list, true);
    QCoreApplication::processEvents();
    REQUIRE(view.row_count() == 4);

    const QModelIndex array_row = fixture.cell(list, 0);
    const QModelIndex name      = fixture.structure.index(0, ReclassStructureModel::name, array_row);
    REQUIRE(name.isValid());

    // The element's cell draws nothing and takes no edit, so a double-click on
    // it opens no editor at all.
    CHECK(view.cell_text(name).isEmpty());
    CHECK_FALSE(fixture.proxy.flags(name).testFlag(Qt::ItemIsEditable));
    double_click(view, name);
    QCoreApplication::processEvents();
    CHECK(view.editor() == nullptr);

    // F2 opens nothing there either, while the array's own row keeps the name
    // its cell shows.
    view.setCurrentIndex(name);
    QKeyEvent f2 {QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier};
    QApplication::sendEvent(&view, &f2);
    QCoreApplication::processEvents();
    CHECK(view.editor() == nullptr);

    const QModelIndex array_name = fixture.cell(list, ReclassStructureModel::name);
    CHECK(fixture.proxy.flags(array_name).testFlag(Qt::ItemIsEditable));
    view.setCurrentIndex(array_name);
    QApplication::sendEvent(&view, &f2);
    QCoreApplication::processEvents();
    REQUIRE(view.editor() != nullptr);
    CHECK(view.editor()->isVisible());
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

    const int name_width = view.cell_rect(name_cell).value().width();

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
    // A longer name widens the run it draws in.
    CHECK(view.cell_rect(fixture.cell(fixture.node_id(0), ReclassStructureModel::name)).value().width() > name_width);

    // Escape leaves the node as it was.
    double_click(view, fixture.cell(fixture.node_id(0), ReclassStructureModel::name));
    QCoreApplication::processEvents();
    editor->setText(QStringLiteral("discarded"));
    QKeyEvent escape {QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};
    QApplication::sendEvent(editor, &escape);
    CHECK_FALSE(editor->isVisible());
    CHECK(player.nodes[0]->name == "health");
    CHECK(messages.size() == 1);

    // A name is free text, so a name that is only spaces clears it and leaves
    // the row unnamed, moving no offset of the nodes behind it.
    const QModelIndex next_offset = fixture.cell(fixture.node_id(1), ReclassStructureModel::offset);
    const QString     offset_text = view.cell_text(next_offset);
    double_click(view, fixture.cell(fixture.node_id(0), ReclassStructureModel::name));
    QCoreApplication::processEvents();
    editor->setText(QStringLiteral("   "));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();

    CHECK_FALSE(editor->isVisible());
    CHECK(player.nodes[0]->name.empty());
    REQUIRE(messages.size() == 2);
    CHECK(messages.last().toStdString() == "Node name cleared.");
    CHECK(view.cell_text(fixture.cell(fixture.node_id(0), ReclassStructureModel::name)).isEmpty());
    CHECK(view.cell_text(next_offset) == offset_text);

    // The row draws no name run any more, so there is nothing to double-click;
    // F2 on the cursor row is the way back and opens a typeable editor.
    view.setCurrentIndex(fixture.cell(fixture.node_id(0), ReclassStructureModel::name));
    QKeyEvent f2 {QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier};
    QApplication::sendEvent(&view, &f2);
    QCoreApplication::processEvents();

    CHECK(editor->isVisible());
    CHECK(editor->text().isEmpty());
    CHECK(editor->width() >= 160);

    editor->setText(QStringLiteral("vitality"));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();

    CHECK(player.nodes[0]->name == "vitality");
    REQUIRE(messages.size() == 3);
    CHECK(messages.last().toStdString() == "Node renamed.");
}

TEST_CASE("the grid's digit keys ask for bytes at the cursor row", "[reclass][ui][view]")
{
    application();

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    std::vector<std::size_t> requests;
    QObject::connect(&view,
                     &ReClassView::insertBytesRequested,
                     [&requests](std::size_t count)
                     {
                         requests.push_back(count);
                     });

    view.setCurrentIndex(fixture.cell(fixture.node_id(1), ReclassStructureModel::name));
    QCoreApplication::processEvents();

    for (int digit = 1; digit <= 8; ++digit)
    {
        QKeyEvent press {QEvent::KeyPress, static_cast<int>(Qt::Key_0) + digit, Qt::NoModifier};
        QApplication::sendEvent(&view, &press);
        // The grid takes the key rather than handing it to the base class.
        CHECK(press.isAccepted());
    }
    CHECK(requests == std::vector<std::size_t> {1, 2, 3, 4, 5, 6, 7, 8});

    // The keypad digits are the same keys.
    QKeyEvent keypad {QEvent::KeyPress, Qt::Key_3, Qt::KeypadModifier};
    QApplication::sendEvent(&view, &keypad);
    CHECK(keypad.isAccepted());
    REQUIRE(requests.size() == 9);
    CHECK(requests.back() == 3);

    // A digit with another modifier is not a byte command and stays with the
    // base class, so the type keys keep their own combinations.
    requests.clear();
    QKeyEvent modified {QEvent::KeyPress, Qt::Key_3, Qt::AltModifier};
    QApplication::sendEvent(&view, &modified);
    CHECK(requests.empty());

    // With no cursor row the request still goes out, so the window can report
    // why nothing was inserted.
    view.setCurrentIndex(QModelIndex());
    QCoreApplication::processEvents();
    QKeyEvent no_row {QEvent::KeyPress, Qt::Key_2, Qt::NoModifier};
    QApplication::sendEvent(&view, &no_row);
    CHECK(no_row.isAccepted());
    CHECK(requests == std::vector<std::size_t> {2});
}

TEST_CASE("the grid's Ctrl+digit keys ask for a hex type", "[reclass][ui][view]")
{
    application();

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    std::vector<NodeKind> kinds;
    QObject::connect(&view,
                     &ReClassView::changeKindRequested,
                     [&kinds](NodeKind kind)
                     {
                         kinds.push_back(kind);
                     });

    view.setCurrentIndex(fixture.cell(fixture.node_id(0), ReclassStructureModel::name));
    QCoreApplication::processEvents();

    const std::pair<int, NodeKind> keys[] = {
        {Qt::Key_1,  NodeKind::hex8},
        {Qt::Key_2, NodeKind::hex16},
        {Qt::Key_4, NodeKind::hex32},
        {Qt::Key_8, NodeKind::hex64}
    };
    for (const auto& [key, kind] : keys)
    {
        QKeyEvent press {QEvent::KeyPress, key, Qt::ControlModifier};
        QApplication::sendEvent(&view, &press);
        CHECK(press.isAccepted());
        CHECK(kinds == std::vector<NodeKind> {kind});
        kinds.clear();
    }

    // Only the four hex sizes have a key of their own; the other digits stay
    // with the base class and change nothing.
    for (const int key : {Qt::Key_3, Qt::Key_5, Qt::Key_6, Qt::Key_7, Qt::Key_9})
    {
        QKeyEvent press {QEvent::KeyPress, key, Qt::ControlModifier};
        QApplication::sendEvent(&view, &press);
        CHECK_FALSE(press.isAccepted());
    }
    CHECK(kinds.empty());
}

TEST_CASE("a note is edited in place through the grid's editor", "[reclass][ui][view]")
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

    Class&            player    = fixture.project.classes()[1];
    const QModelIndex note_cell = fixture.cell(fixture.node_id(0), ReclassStructureModel::comment);

    // A note costs no target access, so the cell is typed into with nothing
    // resolved; the row draws the bare marker for the note it has none of.
    CHECK(fixture.proxy.flags(note_cell).testFlag(Qt::ItemIsEditable));
    CHECK(view.cell_text(note_cell).toStdString() == "//");

    // F3 opens the cursor row's note, which is how a node that draws no note
    // gets one: the bare marker is the only text there is to click.
    view.setCurrentIndex(fixture.cell(fixture.node_id(0), ReclassStructureModel::comment));
    QKeyEvent f3 {QEvent::KeyPress, Qt::Key_F3, Qt::NoModifier};
    QApplication::sendEvent(&view, &f3);
    QCoreApplication::processEvents();

    QLineEdit* editor = view.editor();
    REQUIRE(editor != nullptr);
    CHECK(editor->isVisible());
    CHECK(editor->text().isEmpty());
    // The marker's run is only as wide as `//`, so the editor opens wide enough
    // to type a note into it.
    CHECK(editor->width() > view.cell_rect(note_cell).value().width());

    editor->setText(QStringLiteral("  the player's tag  "));
    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();

    CHECK_FALSE(editor->isVisible());
    CHECK(player.nodes[0]->comment == "the player's tag");
    REQUIRE(messages.size() == 1);
    CHECK(messages.first().toStdString() == "Comment saved.");
    CHECK(view.cell_text(fixture.cell(fixture.node_id(0), ReclassStructureModel::comment)).toStdString()
          == "// the player's tag");

    // A double-click on the drawn note opens the same editor, prefilled with the
    // stored text rather than with the marker.
    double_click(view, fixture.cell(fixture.node_id(0), ReclassStructureModel::comment));
    QCoreApplication::processEvents();
    CHECK(editor->isVisible());
    CHECK(editor->text().toStdString() == "the player's tag");

    // Escape leaves the node as it was.
    editor->setText(QStringLiteral("discarded"));
    QKeyEvent escape {QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};
    QApplication::sendEvent(editor, &escape);
    CHECK_FALSE(editor->isVisible());
    CHECK(player.nodes[0]->comment == "the player's tag");
    CHECK(messages.size() == 1);

    // A note is free text, so a note that is only spaces clears it.
    view.setCurrentIndex(fixture.cell(fixture.node_id(0), ReclassStructureModel::comment));
    QApplication::sendEvent(&view, &f3);
    QCoreApplication::processEvents();
    REQUIRE(editor->isVisible());
    editor->setText(QStringLiteral("   "));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();

    CHECK_FALSE(editor->isVisible());
    CHECK(player.nodes[0]->comment.empty());
    REQUIRE(messages.size() == 2);
    CHECK(messages.last().toStdString() == "Comment cleared.");
    CHECK(view.cell_text(fixture.cell(fixture.node_id(0), ReclassStructureModel::comment)).toStdString() == "//");

    // A hidden node's `...` placeholder takes no note, so it opens no editor.
    player.nodes[0]->hidden = true;
    fixture.structure.rebuild();
    view.setCurrentIndex(fixture.cell(fixture.node_id(0), ReclassStructureModel::name));
    QCoreApplication::processEvents();

    CHECK_FALSE(fixture.proxy.flags(fixture.cell(fixture.node_id(0), ReclassStructureModel::comment))
                    .testFlag(Qt::ItemIsEditable));
    QApplication::sendEvent(&view, &f3);
    QCoreApplication::processEvents();
    CHECK_FALSE(editor->isVisible());
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
    view.resize(1700, 200);
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

    // A pane too narrow for the row the class draws no longer takes the value
    // field down: the field keeps the width the class measured and the pane
    // scrolls instead.
    const QRect value_rect = view.cell_rect(value_cell).value();
    view.resize(400, 200);
    QCoreApplication::processEvents();

    CHECK(view.cell_rect(fixture.cell(fixture.node_id(2), ReclassStructureModel::value)).value() == value_rect);
    CHECK(view.horizontalScrollBar()->maximum() > 0);
}

TEST_CASE("the pane scrolls the whole row without narrowing a field", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;

    // A hex64 row whose note runs past the pane makes the class wider than the
    // pane.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    player.nodes[2]->comment = "the raw bytes of the record, padded to a full eight-byte word";
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    QScrollBar* bar = view.horizontalScrollBar();
    REQUIRE(bar->maximum() > 0);

    const std::size_t tag  = fixture.node_id(0);
    const std::size_t raw  = fixture.node_id(2);
    const auto        rect = [&view, &fixture](std::size_t node, int column)
    {
        return view.cell_rect(fixture.cell(node, column)).value();
    };
    // The three leading fields tile each other at every scroll offset.
    const auto tiles = [&]
    {
        CHECK(rect(tag, ReclassStructureModel::address).left()
              == rect(tag, ReclassStructureModel::offset).left() + rect(tag, ReclassStructureModel::offset).width());
        CHECK(rect(tag, ReclassStructureModel::ascii).left()
              == rect(tag, ReclassStructureModel::address).left() + rect(tag, ReclassStructureModel::address).width());
    };

    const QRect tag_offset = rect(tag, ReclassStructureModel::offset);
    const QRect raw_value  = rect(raw, ReclassStructureModel::value);
    const int   left       = tag_offset.left();
    tiles();

    // A click on the row's own value lands on the row it covers.
    click(view, fixture.cell(raw, ReclassStructureModel::value));
    QCoreApplication::processEvents();
    CHECK(node_at(fixture.structure, view.currentIndex()) == raw);

    // Narrowing the pane past the row leaves every field alone and only grows
    // the bar's range.
    const int range = bar->maximum();
    view.resize(500, 200);
    QCoreApplication::processEvents();
    CHECK(bar->maximum() > range);
    CHECK(rect(tag, ReclassStructureModel::offset) == tag_offset);
    CHECK(rect(raw, ReclassStructureModel::value) == raw_value);

    // Scrolling shifts the leading fields together.
    bar->setValue(bar->maximum() / 2);
    QCoreApplication::processEvents();
    CHECK(rect(tag, ReclassStructureModel::offset).left() < left);
    tiles();

    // At the far end the widest row has scrolled as far as the bar allows, so
    // its whole content is reachable and past-the-end adds no slack.
    bar->setValue(bar->maximum());
    QCoreApplication::processEvents();
    tiles();
    CHECK(bar->value() == bar->maximum());
    CHECK(rect(raw, ReclassStructureModel::comment).right() < view.viewport()->width());

    // A pane wide enough for the class hides the bar and left-aligns the row.
    view.resize(2400, 200);
    QCoreApplication::processEvents();
    CHECK(bar->maximum() == 0);
    CHECK(rect(tag, ReclassStructureModel::offset) == tag_offset);
}

TEST_CASE("scrolling the pane closes the inline editor", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;
    Class&  player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    player.nodes[2]->comment = "the raw bytes of the record, padded to a full eight-byte word";
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
    const QRect        first(cell.left(), cell.top(), first_w, cell.height());
    const QRect second(cell.left() + first_w, cell.top(), mono.horizontalAdvance(QStringLiteral(" 00")), cell.height());

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

TEST_CASE("a marker keeps the grid colour while a whole-value kind warns", "[reclass][ui][view]")
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

    // Point's int32 x moves, so its value warns while the `= ` marker that names
    // it keeps the grid's own colour.
    fixture.resolve();
    fixture.put(kAppBase + 0x20, {0x2A, 0, 0, 0, 0, 0, 0, 0});
    fixture.pass();
    fixture.put(kAppBase + 0x20, {0x2B, 0, 0, 0, 0, 0, 0, 0});
    fixture.pass();
    QCoreApplication::processEvents();

    const QModelIndex value_cell = fixture.cell(x_node, ReclassStructureModel::value);
    CHECK(view.cell_text(value_cell).toStdString() == "= 43 (0x2B)");

    const QRect cell = view.cell_rect(value_cell).value();
    QImage      image(view.viewport()->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    view.paint_into(image);

    const QFontMetrics mono(slopkit::ui::mono_font());
    const int          marker_w = mono.horizontalAdvance(QStringLiteral("= "));
    const QRect        marker(cell.left(), cell.top(), marker_w, cell.height());
    const QRect        value(cell.left() + marker_w, cell.top(), cell.width() - marker_w, cell.height());

    const slopkit::ui::Theme& theme = slopkit::ui::active_theme();
    CHECK(paints_ink(image, marker, theme.text));
    CHECK_FALSE(paints_ink(image, marker, theme.warning));
    CHECK(paints_ink(image, value, theme.warning));
    CHECK_FALSE(paints_ink(image, value, theme.text));

    // A selected row paints the whole cell in the on-accent colour instead,
    // marker and value alike.
    click(view, value_cell);
    QCoreApplication::processEvents();
    image.fill(Qt::transparent);
    view.paint_into(image);
    CHECK(paints_ink(image, marker, theme.on_accent));
    CHECK(paints_ink(image, value, theme.on_accent));
    CHECK_FALSE(paints_ink(image, value, theme.warning));
}

TEST_CASE("a field measured for its own text draws that text whole", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    // The reported class: a uint64 `score` whose note is `the score`, its own
    // texts the widest the class draws, so each of its runs is measured for
    // exactly the text it holds. `raw` keeps the class wider than the pane the
    // row is checked in, `ratio` can read a value wider than the cap its kind
    // allows, and the hidden `skipped` row draws the `...` placeholder.
    Fixture fixture;
    Class&  player = fixture.project.classes()[1];
    player.nodes.clear();
    player.nodes.push_back(node_of(fixture.project, NodeKind::uint64, "score"));
    player.nodes[0]->comment = "the score";
    player.nodes.push_back(node_of(fixture.project, NodeKind::float32, "ratio"));
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    player.nodes[2]->comment          = "the raw bytes of the record";
    slopkit::reclass::NodePtr skipped = node_of(fixture.project, NodeKind::uint8, "skipped");
    skipped->hidden                   = true;
    player.nodes.push_back(std::move(skipped));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    PaintedView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(1000, 200);
    view.show();
    QCoreApplication::processEvents();

    const QFontMetrics prose(slopkit::ui::ui_font());
    const std::size_t  score  = fixture.node_id(0);
    const std::size_t  hidden = fixture.node_id(3);
    const auto         rect   = [&view, &fixture](std::size_t node, int column)
    {
        return view.cell_rect(fixture.cell(node, column)).value();
    };

    // A flowed field's run is exactly as wide as the text it was measured for,
    // which is the width Qt counts as too wide, so it used to drop the run's
    // tail.
    CHECK(rect(score, ReclassStructureModel::type).width() == prose.horizontalAdvance(QStringLiteral("UInt64")));
    CHECK(rect(score, ReclassStructureModel::name).width() == prose.horizontalAdvance(QStringLiteral("score")));
    CHECK(rect(score, ReclassStructureModel::comment).width()
          == prose.horizontalAdvance(QStringLiteral("// the score")));

    // The ink a run reaches: a text drawn whole fills its run, an elided one
    // stops well short of the run's last pixel. The last glyph's own ink can
    // stop a pixel short of its advance, its right side bearing.
    const auto whole = [&rect, &prose](const QImage& painted, std::size_t node, int column, const QString& text)
    {
        const QRect field = rect(node, column);
        REQUIRE(field.left() >= 0);
        REQUIRE(field.right() < painted.width());
        const auto span = ink_span(painted, field);
        REQUIRE(span.has_value());
        CHECK(span->second - field.left() + 1 >= prose.horizontalAdvance(text) - 1);
    };
    const auto no_tooltip = [&rect, &view](std::size_t node, int column)
    {
        CHECK(view.hover_full_text(rect(node, column).center()).isEmpty());
    };

    QImage image(view.viewport()->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    view.paint_into(image);

    // The reported row reads in full and asks for no tooltip, because nothing
    // was clipped.
    whole(image, score, ReclassStructureModel::type, QStringLiteral("UInt64"));
    whole(image, score, ReclassStructureModel::name, QStringLiteral("score"));
    whole(image, score, ReclassStructureModel::comment, QStringLiteral("// the score"));
    no_tooltip(score, ReclassStructureModel::type);
    no_tooltip(score, ReclassStructureModel::name);
    no_tooltip(score, ReclassStructureModel::comment);

    // The collapsed node's `...` placeholder goes through the same rule.
    whole(image, hidden, ReclassStructureModel::name, QStringLiteral("..."));

    // A pane narrower than the class, but still wide enough to hold the row's
    // tail: the bar is up, and a flowed run keeps its measured width, so the
    // same row still reads in full.
    view.resize(rect(score, ReclassStructureModel::comment).right() + 30, 200);
    QCoreApplication::processEvents();
    REQUIRE(view.horizontalScrollBar()->maximum() > 0);
    REQUIRE(rect(score, ReclassStructureModel::comment).right() < view.viewport()->width());

    QImage narrow(view.viewport()->size(), QImage::Format_ARGB32);
    narrow.fill(Qt::transparent);
    view.paint_into(narrow);
    whole(narrow, score, ReclassStructureModel::type, QStringLiteral("UInt64"));
    whole(narrow, score, ReclassStructureModel::name, QStringLiteral("score"));
    whole(narrow, score, ReclassStructureModel::comment, QStringLiteral("// the score"));

    // A live value wider than the cap its kind was measured for still elides,
    // and the tooltip still carries it whole.
    fixture.resolve();
    fixture.put(fixture.player_address() + 8, {0xFF, 0xFF, 0x7F, 0x7F});
    fixture.pass();
    QCoreApplication::processEvents();

    const QFontMetrics mono(slopkit::ui::mono_font());
    const QModelIndex  ratio_cell = fixture.cell(fixture.node_id(1), ReclassStructureModel::value);
    const QRect        ratio_run  = view.cell_rect(ratio_cell).value();
    const QString      ratio_full = view.cell_text(ratio_cell);
    REQUIRE(mono.horizontalAdvance(ratio_full) > ratio_run.width());
    CHECK(view.hover_full_text(ratio_run.center()) == ratio_full);

    QImage live(view.viewport()->size(), QImage::Format_ARGB32);
    live.fill(Qt::transparent);
    view.paint_into(live);
    const auto clipped = ink_span(live, ratio_run);
    REQUIRE(clipped.has_value());
    CHECK(clipped->second - ratio_run.left() + 1 < mono.horizontalAdvance(ratio_full));
}

TEST_CASE("an unread class shows no bar while its drawn text fits", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;
    // A hex64 row reserves the room a full dump, its readings and a pointer can
    // need, but with nothing read it draws only its name and its own value
    // placeholder: the pane holds all of that, so there is no bar to give.
    Class&  player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    const std::size_t raw = fixture.node_id(2);

    // Nothing the class draws is cut off, so the bar is down and the row sits at
    // its home offset.
    CHECK(view.horizontalScrollBar()->maximum() == 0);
    CHECK(view.horizontalScrollBar()->value() == 0);
    CHECK(view.cell_rect(fixture.cell(raw, ReclassStructureModel::value)).value().right() < view.viewport()->width());
}

TEST_CASE("the bar's span ends behind the widest drawn field", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;
    Class&  player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    player.nodes[2]->comment = "the raw bytes of the record, padded to a full eight-byte word";
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    QScrollBar* bar = view.horizontalScrollBar();
    REQUIRE(bar->maximum() > 0);

    // The widest field of all the class draws, wherever it sits.
    const auto widest_cell = [&]
    {
        QModelIndex widest;
        int         right = -1;
        for (const auto& node : player.nodes)
        {
            for (int column = 0; column < ReclassStructureModel::column_count; ++column)
            {
                const QModelIndex cell = fixture.cell(node->id, column);
                if (!cell.isValid())
                {
                    continue;
                }
                const QRect cell_rect = view.cell_rect(cell).value();
                if (cell_rect.width() > 0 && cell_rect.right() > right)
                {
                    right  = cell_rect.right();
                    widest = cell;
                }
            }
        }
        return widest;
    };

    const QModelIndex widest = widest_cell();
    REQUIRE(widest.isValid());
    const int home_left = view.cell_rect(widest).value().left();

    bar->setValue(bar->maximum());
    QCoreApplication::processEvents();

    // The span ends just behind that field: at full scroll its right edge leaves
    // the pane's own margin behind it, and nothing of the class is beyond reach.
    constexpr int kPaneMargin = 4;
    CHECK(view.cell_rect(widest).value().right() == view.viewport()->width() - 1 - kPaneMargin);
    CHECK(view.cell_rect(widest).value().left() < home_left);
    CHECK(bar->value() == bar->maximum());
}

TEST_CASE("a reading that arrives becomes reachable through the bar", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;
    Class&  player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    // A live pass repaints through the proxy's cache, which the dialog wires up
    // the same way.
    QObject::connect(
        &fixture.values, &ReclassValueModel::valuesChanged, &fixture.proxy, &ReclassProxyModel::refresh_values);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    // The widest cell of all the class draws, wherever it sits.
    const auto widest_cell = [&]
    {
        QModelIndex widest;
        int         right = -1;
        for (const auto& node : player.nodes)
        {
            for (int column = 0; column < ReclassStructureModel::column_count; ++column)
            {
                const QModelIndex cell = fixture.cell(node->id, column);
                if (!cell.isValid())
                {
                    continue;
                }
                const QRect cell_rect = view.cell_rect(cell).value();
                if (cell_rect.width() > 0 && cell_rect.right() > right)
                {
                    right  = cell_rect.right();
                    widest = cell;
                }
            }
        }
        return widest;
    };

    // Nothing read yet: the class draws little and the bar is down.
    CHECK(view.horizontalScrollBar()->maximum() == 0);

    // A reading arrives on the widest row and widens the text the class draws.
    fixture.resolve();
    fixture.put(fixture.player_address() + 9, {0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89});
    fixture.pass();
    QCoreApplication::processEvents();

    QScrollBar* bar = view.horizontalScrollBar();
    REQUIRE(bar->maximum() > 0);
    constexpr int kPaneMargin = 4;

    const QModelIndex wide      = widest_cell();
    const int         wide_home = view.cell_rect(wide).value().left();
    const int         span      = bar->maximum();

    // The range grew to cover exactly the new text.
    CHECK(span == view.cell_rect(wide).value().right() + 1 + kPaneMargin - view.viewport()->width());

    // A pass that moves only another row's value re-measures the same span and
    // leaves every cell where it was.
    fixture.put(fixture.player_address(), {0x7F});
    fixture.pass();
    QCoreApplication::processEvents();
    CHECK(bar->maximum() == span);
    CHECK(view.cell_rect(wide).value().left() == wide_home);

    // Scrolled to the end, the widest cell is inside the pane and the bar cannot
    // go further.
    bar->setValue(bar->maximum());
    QCoreApplication::processEvents();
    CHECK(view.cell_rect(wide).value().right() == view.viewport()->width() - 1 - kPaneMargin);
    CHECK(view.cell_rect(wide).value().left() < wide_home);
    CHECK(bar->value() == bar->maximum());
}

TEST_CASE("a live value wider than its cap does not stretch the bar", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;
    Class&  player = fixture.project.classes()[1];
    player.nodes.clear();
    player.nodes.push_back(node_of(fixture.project, NodeKind::float32, "ratio"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    // A live pass repaints through the proxy's cache, which the dialog wires up
    // the same way.
    QObject::connect(
        &fixture.values, &ReclassValueModel::valuesChanged, &fixture.proxy, &ReclassProxyModel::refresh_values);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(1000, 200);
    view.show();
    QCoreApplication::processEvents();

    // The widest cell of all the class draws, wherever it sits.
    const auto widest_cell = [&]
    {
        QModelIndex widest;
        int         right = -1;
        for (int column = 0; column < ReclassStructureModel::column_count; ++column)
        {
            const QModelIndex cell = fixture.cell(fixture.node_id(0), column);
            if (!cell.isValid())
            {
                continue;
            }
            const QRect cell_rect = view.cell_rect(cell).value();
            if (cell_rect.width() > 0 && cell_rect.right() > right)
            {
                right  = cell_rect.right();
                widest = cell;
            }
        }
        return widest;
    };

    // The value starts out readable and short, so the bar has a span to keep
    // once the live pass widens it.
    fixture.resolve();
    fixture.put(fixture.player_address(), {0x00, 0x00, 0x00, 0x00});
    fixture.pass();
    QCoreApplication::processEvents();

    const QRect baseline = view.cell_rect(widest_cell()).value();
    view.resize(baseline.right() - 20, 200);
    QCoreApplication::processEvents();

    QScrollBar* bar = view.horizontalScrollBar();
    REQUIRE(bar->maximum() > 0);
    constexpr int kPaneMargin = 4;

    // A live value wider than the cap its kind was measured for arrives, elides
    // and keeps its whole text in the tooltip.
    fixture.put(fixture.player_address(), {0xFF, 0xFF, 0x7F, 0x7F});
    fixture.pass();
    QCoreApplication::processEvents();

    const QModelIndex  ratio_cell = fixture.cell(fixture.node_id(0), ReclassStructureModel::value);
    const QRect        ratio_run  = view.cell_rect(ratio_cell).value();
    const QString      ratio_full = view.cell_text(ratio_cell);
    const QFontMetrics mono(slopkit::ui::mono_font());
    REQUIRE(mono.horizontalAdvance(ratio_full) > ratio_run.width());
    CHECK(view.hover_full_text(ratio_run.center()) == ratio_full);

    // The bar's span is measured from the run's capped width, not from the wider
    // text it holds: it ends exactly behind the widest cell the class draws, so
    // the live text cannot stretch the pane past the cap.
    const QRect widest = view.cell_rect(widest_cell()).value();
    CHECK(bar->maximum() == widest.right() + 1 + kPaneMargin - view.viewport()->width());
}

TEST_CASE("a span-only range change keeps the inline editor open", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;
    Class&  player = fixture.project.classes()[1];
    player.nodes.clear();
    player.nodes.push_back(node_of(fixture.project, NodeKind::float32, "ratio"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    // A live pass repaints through the proxy's cache, which the dialog wires up
    // the same way.
    QObject::connect(
        &fixture.values, &ReclassValueModel::valuesChanged, &fixture.proxy, &ReclassProxyModel::refresh_values);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(1000, 200);
    view.show();
    QCoreApplication::processEvents();

    // A short reading first, so the bar is up with a range the wider reading can
    // still grow.
    fixture.resolve();
    fixture.put(fixture.player_address(), {0x00, 0x00, 0x00, 0x00});
    fixture.pass();
    QCoreApplication::processEvents();

    const auto widest_right = [&]
    {
        int right = 0;
        for (int column = 0; column < ReclassStructureModel::column_count; ++column)
        {
            const QModelIndex cell = fixture.cell(fixture.node_id(0), column);
            if (cell.isValid())
            {
                const int cell_right = view.cell_rect(cell).value().right();
                if (cell_right > right)
                {
                    right = cell_right;
                }
            }
        }
        return right;
    };

    view.resize(widest_right() - 20, 200);
    QCoreApplication::processEvents();

    QScrollBar* bar = view.horizontalScrollBar();
    REQUIRE(bar->maximum() > 0);

    // The editor opens on the name cell, which the wider reading will not move.
    const QModelIndex name_cell = fixture.cell(fixture.node_id(0), ReclassStructureModel::name);
    const QRect       name_home = view.cell_rect(name_cell).value();
    double_click(view, name_cell);
    QCoreApplication::processEvents();
    REQUIRE(view.editor() != nullptr);
    REQUIRE(view.editor()->isVisible());

    // A wider live reading grows the widest drawn row, so the range moves while
    // the fields stay where they are: the editor stays open rather than closing
    // over a cell that did not move.
    const int span = bar->maximum();
    fixture.put(fixture.player_address(), {0xFF, 0xFF, 0x7F, 0x7F});
    fixture.pass();
    QCoreApplication::processEvents();

    CHECK(bar->maximum() > span);
    CHECK(view.cell_rect(name_cell).value() == name_home);
    CHECK(view.editor() != nullptr);
    CHECK(view.editor()->isVisible());
}

TEST_CASE("the bar follows the pane across the exact fit", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;
    Class&  player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(2400, 200);
    view.show();
    QCoreApplication::processEvents();

    const std::size_t raw     = fixture.node_id(2);
    const auto        left_of = [&](int column)
    {
        return view.cell_rect(fixture.cell(raw, column)).value().left();
    };
    const int offset_home  = left_of(ReclassStructureModel::offset);
    const int address_home = left_of(ReclassStructureModel::address);
    const int ascii_home   = left_of(ReclassStructureModel::ascii);

    QScrollBar* bar = view.horizontalScrollBar();

    // A pane wider than every drawn row: no bar, nothing scrolled.
    CHECK(bar->maximum() == 0);
    CHECK(bar->value() == 0);

    // Narrowed past the widest drawn row, the bar comes up and the aligned
    // fields keep their x.
    view.resize(500, 200);
    QCoreApplication::processEvents();
    REQUIRE(bar->maximum() > 0);
    CHECK(bar->value() == 0);
    CHECK(left_of(ReclassStructureModel::offset) == offset_home);
    CHECK(left_of(ReclassStructureModel::address) == address_home);
    CHECK(left_of(ReclassStructureModel::ascii) == ascii_home);

    // Narrower still, the range grows; the offset stays inside it.
    const int range = bar->maximum();
    view.resize(400, 200);
    QCoreApplication::processEvents();
    CHECK(bar->maximum() > range);
    CHECK(bar->value() <= bar->maximum());

    // Scrolled to the end and then widened so the span shrinks, the offset is
    // clamped under the new maximum instead of left past the end.
    bar->setValue(bar->maximum());
    QCoreApplication::processEvents();
    REQUIRE(bar->value() > 0);
    view.resize(495, 200);
    QCoreApplication::processEvents();
    CHECK(bar->maximum() > 0);
    CHECK(bar->value() <= bar->maximum());

    // Widened past the widest drawn row, the bar hides, the offset returns home
    // and the aligned fields are back where they started.
    view.resize(2400, 200);
    QCoreApplication::processEvents();
    CHECK(bar->maximum() == 0);
    CHECK(bar->value() == 0);
    CHECK(left_of(ReclassStructureModel::offset) == offset_home);
    CHECK(left_of(ReclassStructureModel::address) == address_home);
    CHECK(left_of(ReclassStructureModel::ascii) == ascii_home);
}

TEST_CASE("a class of hidden rows reserves only its placeholder", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture fixture;
    Class&  player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);

    // Every row collapses, so each draws only the `...` placeholder.
    for (const auto& node : player.nodes)
    {
        node->hidden = true;
    }
    fixture.structure.rebuild();
    QCoreApplication::processEvents();

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(500, 200);
    view.show();
    QCoreApplication::processEvents();

    const std::size_t raw = fixture.node_id(2);

    // A collapsed row paints the `...` placeholder in its name field.
    CHECK(view.cell_rect(fixture.cell(raw, ReclassStructureModel::name)).value().width()
          == QFontMetrics(slopkit::ui::ui_font()).horizontalAdvance(QStringLiteral("...")));
    // No room is spared for the fields a collapsed row does not draw.
    CHECK(view.cell_rect(fixture.cell(raw, ReclassStructureModel::value)).value().width() == 0);
    CHECK(view.cell_rect(fixture.cell(raw, ReclassStructureModel::hints)).value().width() == 0);
    CHECK(view.cell_rect(fixture.cell(raw, ReclassStructureModel::pointer)).value().width() == 0);
    CHECK(view.horizontalScrollBar()->maximum() == 0);
}

TEST_CASE("no class selected shows no bar", "[reclass][ui][view]")
{
    application();

    Fixture fixture;
    // An index past the project's classes leaves the model with nothing to draw,
    // which is what the window shows before a class is chosen.
    fixture.structure.set_class(99);
    QCoreApplication::processEvents();

    ReClassView view {fixture.proxy, fixture.structure, fixture.values};
    view.resize(500, 200);
    view.show();
    QCoreApplication::processEvents();

    CHECK(view.row_count() == 0);
    CHECK(view.horizontalScrollBar()->maximum() == 0);
}
