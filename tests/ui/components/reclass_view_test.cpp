#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
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
            values.set_modules(std::vector<slopkit::process::ModuleInfo> {module_image("app", kAppBase, 0x1000)});
            values.set_structure(structure);

            project.add_class("Point", "app+0x20");
            project.add_class("Player", "app+0x10");

            Class& point = project.classes()[0];
            point.nodes.push_back(node_of(project, NodeKind::int32, "x"));
            point.nodes.push_back(node_of(project, NodeKind::int32, "y"));

            Class& player = project.classes()[1];
            player.nodes.push_back(node_of(project, NodeKind::int8, "tag"));
            slopkit::reclass::Node next = node_of(project, NodeKind::class_pointer, "next");
            next.class_name             = "Point";
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
            return project.classes()[1].nodes[index].id;
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
}

TEST_CASE("the class view holds fixed columns and paints no header", "[reclass][ui][view]")
{
    application();

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure};
    view.resize(700, 200);
    view.show();
    QCoreApplication::processEvents();

    using slopkit::ui::models::ReclassStructureModel;

    CHECK(view.model() == &fixture.proxy);
    CHECK(view.structure() == &fixture.structure);
    CHECK(view.row_count() == 2);

    // The columns run left to right and none of them is resizable.
    for (int column = 1; column < ReclassStructureModel::column_count; ++column)
    {
        CHECK(view.column_x(column) > view.column_x(column - 1));
    }
    CHECK(view.column_width(ReclassStructureModel::address) > 0);
    CHECK(view.column_width(ReclassStructureModel::offset) > 0);
    CHECK(view.column_width(ReclassStructureModel::name) > 0);

    // The value column owns the pane's slack with the comment column pinned
    // against the right edge, so a reading wider than its text still fits.
    CHECK(view.column_x(ReclassStructureModel::comment)
          == view.column_x(ReclassStructureModel::value) + view.column_width(ReclassStructureModel::value));
    CHECK(view.column_x(ReclassStructureModel::comment) + view.column_width(ReclassStructureModel::comment)
          <= view.viewport()->width());

    const QModelIndex name_cell = fixture.cell(fixture.node_id(0), ReclassStructureModel::name);
    const int         address_x = view.column_x(ReclassStructureModel::address);
    const int         address_w = view.column_width(ReclassStructureModel::address);
    const int         comment_x = view.column_x(ReclassStructureModel::comment);
    const int         value_w   = view.column_width(ReclassStructureModel::value);
    const int         first_top = view.cell_rect(name_cell).value().top();

    CHECK(view.cell_text(name_cell).toStdString() == "tag");

    // The widths never follow the pane: only the value column answers a resize.
    view.resize(1000, 200);
    QCoreApplication::processEvents();

    CHECK(view.column_x(ReclassStructureModel::address) == address_x);
    CHECK(view.column_width(ReclassStructureModel::address) == address_w);
    CHECK(view.column_width(ReclassStructureModel::value) > value_w);
    CHECK(view.column_x(ReclassStructureModel::comment) > comment_x);

    // No header band: the first row starts at the view's own top margin.
    CHECK(view.cell_rect(name_cell).value().top() == first_top);
    CHECK(first_top <= 8);
}

TEST_CASE("the narrow columns are sized to the text the class draws", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure};
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
    const int name_width                       = view.column_width(ReclassStructureModel::name);
    fixture.project.classes()[1].nodes[0].name = "a_much_longer_field_name";
    fixture.structure.rebuild();
    QCoreApplication::processEvents();
    CHECK(view.column_width(ReclassStructureModel::name) > name_width);

    // ...while a live pass repaints without re-measuring anything.
    const int settled = view.column_width(ReclassStructureModel::name);
    fixture.resolve();
    fixture.put(fixture.player_address(), {1, 0x20, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.pass();
    QCoreApplication::processEvents();
    CHECK(view.column_width(ReclassStructureModel::name) == settled);
}

TEST_CASE("clicking a row selects it and shift extends the selection", "[reclass][ui][view]")
{
    application();

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure};
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

TEST_CASE("toggling a reference row inlines its class and keeps the cursor", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure};
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
    ReClassView view {fixture.proxy, fixture.structure};
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
    CHECK(view.cell_text(value_cell).toStdString() == "42  0x2A");

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
    CHECK(view.cell_text(value_cell).toStdString() == "-7  0xF9");
}

TEST_CASE("a double-click on a name cell renames the node in place", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure};
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
    CHECK(player.nodes[0].name == "health");
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
    CHECK(player.nodes[0].name == "health");
    CHECK(messages.size() == 1);

    // An empty name is refused with its reason, and the editor stays open for
    // the correction.
    double_click(view, fixture.cell(fixture.node_id(0), ReclassStructureModel::name));
    QCoreApplication::processEvents();
    editor->setText(QStringLiteral("   "));
    QApplication::sendEvent(editor, &enter);
    CHECK(editor->isVisible());
    CHECK(player.nodes[0].name == "health");
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

TEST_CASE("a reading wider than its column is readable from the tooltip", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture     fixture;
    ReClassView view {fixture.proxy, fixture.structure};
    view.resize(900, 200);
    view.show();
    QCoreApplication::processEvents();

    fixture.put(fixture.player_address(), {0x00, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.resolve();
    fixture.pass();

    const QModelIndex pointer_value = fixture.cell(fixture.node_id(1), ReclassStructureModel::value);
    CHECK(view.cell_text(pointer_value).toStdString() == "-> 0x00000000002A0000");

    // A cell whose text fits carries no tooltip...
    CHECK(view.hover_full_text(view.cell_rect(pointer_value).value().center()).isEmpty());

    view.resize(300, 200);
    QCoreApplication::processEvents();

    // ...and one that overflows is read from its full text.
    const QModelIndex narrow_value = fixture.cell(fixture.node_id(1), ReclassStructureModel::value);
    CHECK(view.hover_full_text(view.cell_rect(narrow_value).value().center()).toStdString()
          == view.cell_text(narrow_value).toStdString());
    CHECK_FALSE(view.hover_full_text(view.cell_rect(narrow_value).value().center()).isEmpty());
}

TEST_CASE("the grid stripes its rows and colours the cells the model colours", "[reclass][ui][view]")
{
    application();

    using slopkit::ui::models::ReclassStructureModel;

    Fixture     fixture;
    PaintedView view {fixture.proxy, fixture.structure};
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
          == slopkit::ui::active_theme().text_muted);
    CHECK(colour_of(fixture.node_id(0), ReclassStructureModel::type).value<QColor>()
          == slopkit::ui::active_theme().syntax_keyword);
    CHECK_FALSE(colour_of(fixture.node_id(0), ReclassStructureModel::name).isValid());
}
