#include <catch2/catch.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "reclass/node.hpp"
#include "reclass/project.hpp"
#include "support/reclass_helpers.hpp"
#include "support/reclass_live_helpers.hpp"
#include "support/ui_helpers.hpp"
#include "ui/live_values.hpp"
#include "ui/models/reclass_structure_model.hpp"
#include "ui/models/reclass_value_model.hpp"

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
    using slopkit::test::instance_array_of;
    using slopkit::test::module_image;
    using slopkit::test::node_of;
    using slopkit::test::reading_for;
    using slopkit::test::region_of;
    using slopkit::test::stage_bytes;
    using slopkit::test::wrapper_array_of;
    using slopkit::ui::LiveReading;
    using slopkit::ui::LiveRequest;
    using slopkit::ui::models::ReclassRow;
    using slopkit::ui::models::ReclassStructureModel;
    using slopkit::ui::models::ReclassValueModel;

    constexpr std::uint64_t kAppBase   = 0x400000;
    constexpr std::uint64_t kHeapBase  = 0x600000;
    constexpr std::uint64_t kStackBase = 0x7FF00000;

    // Player { int8 tag; class_pointer next; } and Point { int32 x; int32 y; },
    // with Player pinned at app+0x10.
    class Fixture
    {
    public:
        Fixture()
        {
            target = slopkit::test::fake_target();
            attach_session(worker);
            // The image, an anonymous writable block and the stack: the kinds of
            // memory the pointer field tags an address with.
            values.set_modules(std::vector<slopkit::process::ModuleInfo> {module_image("app", kAppBase, 0x1000)},
                               std::vector<slopkit::process::RegionInfo> {
                                   region_of(kAppBase, 0x1000, "/app", true),
                                   region_of(kHeapBase, 0x1000, {}, false, true),
                                   region_of(kStackBase, 0x1000, "[stack]"),
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

        void resolve()
        {
            values.maybe_resolve_addresses();
            REQUIRE(drain_until_idle(worker));
        }

        void write(std::uint64_t address, std::initializer_list<unsigned> values_to_write)
        {
            stage_bytes(access, address, values_to_write);
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

        [[nodiscard]] LiveReading reading(std::size_t id, std::uint64_t address, std::size_t size) const
        {
            return reading_for(access, id, address, size);
        }

        // One live pass: the reads the model asks for, answered from the fake
        // memory the way the live values pass answers them.
        std::vector<LiveRequest> pass()
        {
            const std::vector<LiveRequest> requests = values.requests();
            std::vector<LiveReading>       readings;
            readings.reserve(requests.size());
            for (const LiveRequest& request : requests)
            {
                readings.push_back(reading(request.id, request.address, request.size));
            }
            values.apply_readings(readings);
            return requests;
        }

        [[nodiscard]] const ReclassRow& row(std::size_t index) const
        {
            const ReclassRow* found = structure.row_for(structure.index(static_cast<int>(index), 0));
            REQUIRE(found != nullptr);
            return *found;
        }

        [[nodiscard]] const ReclassRow& child(std::size_t parent, std::size_t index) const
        {
            const ReclassRow* found = structure.row_for(
                structure.index(static_cast<int>(index), 0, structure.index(static_cast<int>(parent), 0)));
            REQUIRE(found != nullptr);
            return *found;
        }

        FakeAccess            access;
        AccessWorker          worker {access};
        AttachedTarget        target;
        Project               project;
        ReclassStructureModel structure {project};
        ReclassValueModel     values {worker, target};
    };
}

TEST_CASE("one read is submitted per visible block", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();

    const std::vector<LiveRequest> requests = fixture.values.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].id == 0);
    CHECK(requests[0].address == fixture.player_address());
    CHECK(requests[0].size == 9);

    CHECK(fixture.values.address_text(fixture.row(0)) == QStringLiteral("400010"));
}

TEST_CASE("an address is spelled as the shortest digits the grid shows", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();

    CHECK(fixture.values.short_address(0x400010) == QStringLiteral("400010"));
    CHECK(fixture.values.short_address(0) == QStringLiteral("0"));
    // The padded absolute form is still what the window's Copy writes.
    CHECK(fixture.values.format_address(0x400010) == QStringLiteral("0x0000000000400010"));
}

TEST_CASE("nothing is submitted while no target is attached", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();
    REQUIRE(fixture.values.requests().size() == 1);

    fixture.target.clear();

    CHECK(fixture.values.requests().empty());
    CHECK(fixture.values.address_text(fixture.row(0)) == QStringLiteral("Attach to a target first."));
    CHECK(fixture.values.value_text(fixture.row(0)) == QStringLiteral("?"));
    CHECK(fixture.values.ascii_text(fixture.row(0)) == QStringLiteral("?"));
}

TEST_CASE("an unresolved class address reads not resolved yet and nothing more", "[reclass][ui]")
{
    application();

    Fixture fixture;

    CHECK(fixture.values.requests().empty());
    CHECK(fixture.values.address_text(fixture.row(0)) == QStringLiteral("not resolved yet"));
    CHECK(fixture.values.value_text(fixture.row(0)) == QStringLiteral("?"));
}

TEST_CASE("each node's value is sliced out of its block at its offset", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();

    // The class pointer's bytes name the heap, so its field tags the region.
    fixture.write(fixture.player_address(), {0x2A, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.pass();

    // Each cell draws the node's value: the integer beside its hex form, the
    // class pointer as the address it holds.
    CHECK(fixture.values.value_text(fixture.row(0)) == QStringLiteral("= 42 (0x2A)"));
    // The marker is its own run, so a painter keeps it the grid's colour while
    // the value beside it warns.
    const std::vector<slopkit::reclass::ValuePart> runs = fixture.values.value_parts(fixture.row(0));
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].text == "= ");
    CHECK(runs[0].label);
    CHECK(runs[1].text == "42 (0x2A)");
    CHECK_FALSE(runs[1].label);
    // A pointer's address is the pointer field's; the value cell stays empty.
    CHECK(fixture.values.value_text(fixture.row(1)).isEmpty());
    CHECK(fixture.values.pointer_text(fixture.row(1)) == QStringLiteral("-> <HEAP>600000"));

    // An address in no region at all is no pointer: the field stays empty.
    fixture.write(fixture.player_address(), {0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A});
    fixture.pass();
    CHECK(fixture.values.pointer_text(fixture.row(1)).isEmpty());

    // The address column follows the node's own offset inside the class.
    CHECK(fixture.values.address_text(fixture.row(1)) == QStringLiteral("400011"));
    CHECK(fixture.values.node_address(fixture.row(1)) == fixture.player_address() + 1);
}

TEST_CASE("a hex node whose bytes name a module address fills the pointer field", "[reclass][ui]")
{
    application();

    Fixture fixture;

    // A hex64 node after Player's own fields, so the class covers the bytes the
    // node reads.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);
    fixture.resolve();

    // The module's own address, little-endian, in the node's bytes.
    fixture.write(fixture.player_address(),
                  {0, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.pass();

    const QString text = fixture.values.value_text(fixture.row(2));
    CHECK(text.startsWith(QStringLiteral("40 00 40 00")));
    CHECK_FALSE(text.contains(QStringLiteral("->")));

    // The readings are the hints field's and the plausible pointer is the
    // pointer field's.
    const QString hints = fixture.values.hint_text(fixture.row(2));
    CHECK(hints.startsWith(QStringLiteral("(")));
    CHECK(hints.endsWith(QStringLiteral(")")));
    CHECK(hints.contains(QStringLiteral("400040")));
    CHECK(fixture.values.pointer_text(fixture.row(2)) == QStringLiteral("-> <CODE>400040"));
    CHECK(fixture.values.hint_text(fixture.row(1)).isEmpty());
    CHECK(fixture.values.pointer_text(fixture.row(0)).isEmpty());

    // The value cell's runs are its own byte pairs and nothing else.
    const std::vector<slopkit::reclass::ValuePart> parts = fixture.values.value_parts(fixture.row(2));
    REQUIRE(parts.size() == 8);
    for (std::size_t index = 0; index < 8; ++index)
    {
        CHECK_FALSE(parts[index].reading);
        CHECK(parts[index].byte == index);
    }
    // A value outside every module or region is drawn by the value cell alone,
    // with the readings still in the hints field.
    fixture.write(fixture.player_address() + 9, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x00});
    fixture.pass();
    CHECK(fixture.values.hint_text(fixture.row(2)).contains(QStringLiteral("77665544332211")));
    CHECK(fixture.values.pointer_text(fixture.row(2)).isEmpty());
    CHECK_FALSE(fixture.values.value_text(fixture.row(2)).contains(QStringLiteral("->")));
}

TEST_CASE("a node reports which of its own bytes moved", "[reclass][ui]")
{
    application();

    Fixture fixture;

    // A hex32 node after Player's own fields, so the class covers the bytes it
    // reads.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::hex32, "raw"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);
    fixture.resolve();

    fixture.write(fixture.player_address(), {0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1A, 0x2B, 0x3C, 0x4D});
    fixture.pass();

    const ReclassRow& raw = fixture.row(2);
    CHECK(fixture.values.node_bytes(raw).size() == 4);

    // Nothing moved yet: the mask is there but no byte is lit.
    const auto first = fixture.values.node_changed_bytes(raw);
    REQUIRE(first.size() == 4);
    CHECK(first[0] == 0);
    CHECK(first[1] == 0);
    CHECK(first[2] == 0);
    CHECK(first[3] == 0);

    // The second byte alone moves; only its own flag lights.
    fixture.write(fixture.player_address() + 9, {0x1A, 0x00, 0x3C, 0x4D});
    fixture.pass();
    const auto moved = fixture.values.node_changed_bytes(raw);
    REQUIRE(moved.size() == 4);
    CHECK(moved[0] == 0);
    CHECK(moved[1] == 1);
    CHECK(moved[2] == 0);
    CHECK(moved[3] == 0);

    // The mask is dropped with the reading when the block goes unreadable.
    static_cast<void>(fixture.values.requests());
    fixture.values.apply_readings(std::vector<LiveReading> {
        LiveReading {.id = 0, .readable = false, .bytes = {}}
    });
    CHECK(fixture.values.node_changed_bytes(raw).empty());

    // And there is nothing to light while no target is attached.
    fixture.target.clear();
    static_cast<void>(fixture.values.requests());
    CHECK(fixture.values.node_changed_bytes(raw).empty());
}

TEST_CASE("an unreadable block paints every one of its nodes with a question mark", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();
    fixture.pass();

    static_cast<void>(fixture.values.requests());
    fixture.values.apply_readings(std::vector<LiveReading> {
        LiveReading {.id = 0, .readable = false, .bytes = {}}
    });

    CHECK(fixture.values.value_text(fixture.row(0)) == QStringLiteral("?"));
    CHECK(fixture.values.ascii_text(fixture.row(0)) == QStringLiteral("?"));
    // The address is known even when the page is not.
    CHECK(fixture.values.address_text(fixture.row(0)) == QStringLiteral("400010"));
}

TEST_CASE("a class edit keeps the values the reading still covers", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();

    fixture.write(fixture.player_address(), {0x2A, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.pass();

    CHECK(fixture.values.value_text(fixture.row(0)) == QStringLiteral("= 42 (0x2A)"));
    const QString pointer_ascii = fixture.values.ascii_text(fixture.row(1));

    // Appending a node grows the class past the retained reading: the nodes the
    // reading still covers keep their values, the new one reads as unread.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::int32, "score"));
    fixture.write(fixture.player_address(),
                  {0x2A, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00});
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);
    fixture.values.invalidate();

    CHECK(fixture.values.value_text(fixture.row(0)) == QStringLiteral("= 42 (0x2A)"));
    CHECK(fixture.values.ascii_text(fixture.row(1)) == pointer_ascii);
    CHECK(fixture.values.value_text(fixture.row(2)) == QStringLiteral("?"));

    // The next pass fills the rest in.
    fixture.pass();
    CHECK(fixture.values.value_text(fixture.row(2)) == QStringLiteral("= 7 (0x7)"));
}

TEST_CASE("a block whose address moved reads unread until the next pass", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();

    // The pointer at offset 1 points at the heap object; both blocks are read.
    fixture.write(fixture.player_address(), {0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.write(kHeapBase, {0x07, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00});
    fixture.pass();

    const std::size_t pointer_id = fixture.project.classes()[1].nodes[1]->id;
    fixture.structure.set_expanded(pointer_id, true);
    fixture.pass();
    REQUIRE(fixture.values.value_text(fixture.child(1, 0)) == QStringLiteral("= 7 (0x7)"));

    // Retyping the tag to int32 moves the pointer field four bytes along, so its
    // target sits where the retained reading of the class does not reach.
    Class& player   = fixture.project.classes()[1];
    player.nodes[0] = node_of(fixture.project, NodeKind::int32, "tag");
    fixture.write(fixture.player_address(), {0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);
    fixture.structure.set_expanded(pointer_id, true);
    fixture.values.invalidate();

    // The class's own row keeps the reading it was taken at; the inlined target,
    // whose address is no longer known, reads unread.
    CHECK(fixture.values.value_text(fixture.row(0)) == QStringLiteral("= 1610612736 (0x60000000)"));
    CHECK(fixture.values.value_text(fixture.child(1, 0)) == QStringLiteral("?"));
    CHECK(fixture.values.address_text(fixture.child(1, 0)) == QStringLiteral("?"));

    // The next passes read the class at its new shape and the target it points
    // at, and the values come back.
    fixture.pass();
    fixture.pass();
    CHECK(fixture.values.value_text(fixture.child(1, 0)) == QStringLiteral("= 7 (0x7)"));
}

TEST_CASE("the ascii view spells a row's bytes", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();

    // A printable byte as itself, every other byte as a dot.
    fixture.write(fixture.player_address(), {0x41, 0x42, 0x43, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.pass();
    CHECK(fixture.values.ascii_text(fixture.row(0)) == QStringLiteral("A"));
    CHECK(fixture.values.ascii_text(fixture.row(1)) == QStringLiteral("BCD....."));

    // A node that carries no bytes of its own draws an empty cell.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::class_instance, "obj"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);
    fixture.structure.rebuild();
    CHECK(fixture.values.ascii_text(fixture.row(2)).isEmpty());
}

TEST_CASE("an ascii cell keeps the characters it does not draw for the hover", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();

    // A string field wide enough to overflow the cell: sixteen bytes behind the
    // row's int8 and its pointer.
    Class& player = fixture.project.classes()[1];
    player.nodes.push_back(node_of(fixture.project, NodeKind::utf8_text, "title"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(1);
    fixture.structure.rebuild();

    fixture.write(fixture.player_address(),
                  {0xAA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x65, 0x6C, 0x6C,
                   0x6F, 0x20, 0x57, 0x6F, 0x72, 0x6C, 0x64, 0x21, 0x00, 0x00, 0x00, 0x00});
    fixture.pass();

    REQUIRE(fixture.values.node_bytes(fixture.row(2)).size() == 16);
    CHECK(fixture.values.ascii_text(fixture.row(2)) == QStringLiteral("Hello Wo"));
    CHECK(fixture.values.ascii_full_text(fixture.row(2)) == QStringLiteral("Hello World!...."));

    // A node of eight bytes or fewer is unaffected: the two readings agree.
    REQUIRE(fixture.values.node_bytes(fixture.row(1)).size() == 8);
    CHECK(fixture.values.ascii_text(fixture.row(1)) == fixture.values.ascii_full_text(fixture.row(1)));
}

TEST_CASE("an expanded class pointer is read one pass after its pointer changes", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();

    // The pointer sits at offset 1 of Player and points at the heap object.
    fixture.write(fixture.player_address(), {0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.write(kHeapBase, {0x07, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00});
    fixture.pass();

    fixture.structure.set_expanded(fixture.project.classes()[1].nodes[1]->id, true);

    // One pass later the pointer's target is known, read and shown.
    const std::vector<LiveRequest> requests = fixture.pass();
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].address == kHeapBase);
    CHECK(requests[1].size == 8);

    REQUIRE(fixture.structure.rowCount(fixture.structure.index(1, 0)) == 2);
    CHECK(fixture.values.value_text(fixture.child(1, 0)) == QStringLiteral("= 7 (0x7)"));
    CHECK(fixture.values.value_text(fixture.child(1, 1)) == QStringLiteral("= 9 (0x9)"));
    CHECK(fixture.values.node_address(fixture.child(1, 1)) == kHeapBase + 4);
    CHECK(fixture.values.address_text(fixture.child(1, 1)) == QStringLiteral("600004"));
}

TEST_CASE("a wrapper array's element sits at the index's own stride", "[reclass][ui]")
{
    application();

    Fixture fixture;

    // Holder { uint8 tag; Point points[3]; }: the class is inlined where the
    // array sits, so element `n` starts eight bytes along from element zero.
    fixture.project.add_class("Holder", "app+0x80");
    Class& holder = fixture.project.classes().back();
    holder.nodes.push_back(node_of(fixture.project, NodeKind::uint8, "tag"));
    holder.nodes.push_back(wrapper_array_of(fixture.project, "Point", 3));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(2);
    fixture.resolve();

    const std::uint64_t holder_address = kAppBase + 0x80;
    fixture.write(holder_address, {0x11, 1, 0, 0, 0, 2, 0, 0, 0, 12, 0, 0, 0, 13, 0, 0, 0, 22, 0, 0, 0, 23, 0, 0, 0});
    fixture.pass();

    const std::size_t array_id = fixture.row(1).node_id;
    CHECK(fixture.values.address_text(fixture.row(1)) == QStringLiteral("400081"));
    CHECK(fixture.values.value_text(fixture.row(1)).isEmpty());

    fixture.structure.set_expanded(array_id, true);
    fixture.pass();

    // Element zero is the first Point the array covers.
    CHECK(fixture.values.value_text(fixture.child(1, 0)) == QStringLiteral("= 1 (0x1)"));
    CHECK(fixture.values.value_text(fixture.child(1, 1)) == QStringLiteral("= 2 (0x2)"));
    CHECK(fixture.values.node_address(fixture.child(1, 0)) == holder_address + 1);

    // Element one is one stride along, the index being what moves it there.
    REQUIRE(fixture.structure.step_element(array_id, 1));
    fixture.pass();
    CHECK(fixture.values.value_text(fixture.child(1, 0)) == QStringLiteral("= 12 (0xC)"));
    CHECK(fixture.values.value_text(fixture.child(1, 1)) == QStringLiteral("= 13 (0xD)"));
    CHECK(fixture.values.address_text(fixture.child(1, 0)) == QStringLiteral("400089"));
    CHECK(fixture.values.address_text(fixture.child(1, 1)) == QStringLiteral("40008D"));

    // And the last element is the last stride.
    REQUIRE(fixture.structure.step_element(array_id, 1));
    fixture.pass();
    CHECK(fixture.values.value_text(fixture.child(1, 0)) == QStringLiteral("= 22 (0x16)"));
    CHECK(fixture.values.node_address(fixture.child(1, 0)) == holder_address + 17);
}

TEST_CASE("an instance array's element is read past the pointer's target", "[reclass][ui]")
{
    application();

    Fixture fixture;

    // Pool { uint8 tag; ClassInstanceArray<Point>[3] points; }: the pointer's
    // own eight bytes sit in the class, three Points sitting at its target.
    fixture.project.add_class("Pool", "app+0x80");
    Class& pool = fixture.project.classes().back();
    pool.nodes.push_back(node_of(fixture.project, NodeKind::uint8, "tag"));
    pool.nodes.push_back(instance_array_of(fixture.project, "Point", 3));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(2);
    fixture.resolve();

    const std::uint64_t pool_address = kAppBase + 0x80;
    fixture.write(pool_address, {0x11, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.write(kHeapBase, {1, 0, 0, 0, 2, 0, 0, 0, 11, 0, 0, 0, 12, 0, 0, 0, 21, 0, 0, 0, 22, 0, 0, 0});
    fixture.pass();

    const std::size_t array_id = fixture.row(1).node_id;
    // The pointer's own bytes stay where the pointer is.
    CHECK(fixture.values.pointer_text(fixture.row(1)) == QStringLiteral("-> <HEAP>600000"));
    CHECK(fixture.values.address_text(fixture.row(1)) == QStringLiteral("400081"));

    fixture.structure.set_expanded(array_id, true);
    const std::vector<LiveRequest> first = fixture.pass();
    REQUIRE(first.size() == 2);
    CHECK(first[1].address == kHeapBase);
    CHECK(first[1].size == 8);
    CHECK(fixture.values.value_text(fixture.child(1, 0)) == QStringLiteral("= 1 (0x1)"));

    // Element two is two strides past the pointer's target, and that is the
    // one read the grid now submits for the array.
    REQUIRE(fixture.structure.set_element_index(array_id, 2));
    const std::vector<LiveRequest> moved = fixture.pass();
    REQUIRE(moved.size() == 2);
    CHECK(moved[1].address == kHeapBase + 16);
    CHECK(moved[1].size == 8);
    CHECK(fixture.values.node_address(fixture.child(1, 0)) == kHeapBase + 16);
    CHECK(fixture.values.address_text(fixture.child(1, 0)) == QStringLiteral("600010"));
    CHECK(fixture.values.value_text(fixture.child(1, 0)) == QStringLiteral("= 21 (0x15)"));
    CHECK(fixture.values.value_text(fixture.child(1, 1)) == QStringLiteral("= 22 (0x16)"));

    // Stepping back to the first element reads the target again.
    REQUIRE(fixture.structure.step_element(array_id, -2));
    const std::vector<LiveRequest> back = fixture.pass();
    REQUIRE(back.size() == 2);
    CHECK(back[1].address == kHeapBase);
    CHECK(fixture.values.value_text(fixture.child(1, 0)) == QStringLiteral("= 1 (0x1)"));

    // A reading asked for before a jump belongs to the element that moved away,
    // so the late answer must not land where the new element sits, and the moved
    // block reads unread until the next pass reads the element it now covers.
    const std::vector<LiveRequest> stale = fixture.values.requests();
    REQUIRE(stale.size() == 2);
    REQUIRE(fixture.structure.step_element(array_id, 2));
    fixture.write(kHeapBase, {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE});
    fixture.values.apply_readings(
        std::vector<LiveReading> {fixture.reading(stale[0].id, stale[0].address, stale[0].size),
                                  fixture.reading(stale[1].id, stale[1].address, stale[1].size)});
    CHECK(fixture.values.value_text(fixture.child(1, 0)) == QStringLiteral("?"));
}

TEST_CASE("a live pass leaves the grid's rows alone", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();

    const ReclassRow* before = fixture.structure.row_for(fixture.structure.index(0, 0));
    REQUIRE(before != nullptr);

    fixture.pass();
    fixture.pass();

    CHECK(fixture.structure.row_for(fixture.structure.index(0, 0)) == before);
}

TEST_CASE("writing a value submits the node's address and its encoded bytes", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();
    fixture.pass();

    QStringList messages;
    bool        failed = false;
    QObject::connect(&fixture.values,
                     &ReclassValueModel::statusChanged,
                     [&](const QString& message, bool error)
                     {
                         messages.push_back(message);
                         failed = error;
                     });

    fixture.write(fixture.player_address(), {0xAA, 0xAA, 0xAA, 0xAA});
    REQUIRE(fixture.holds(fixture.player_address(), {0xAA, 0xAA, 0xAA, 0xAA}));

    CHECK(fixture.values.editable(fixture.row(0)));
    REQUIRE(fixture.values.write_value(fixture.row(0), QStringLiteral("-7")));
    REQUIRE(drain_until_idle(fixture.worker));

    CHECK(messages == QStringList {QStringLiteral("Value written.")});
    CHECK_FALSE(failed);

    // The int8 node's byte changed in place; its neighbours are untouched.
    CHECK(fixture.holds(fixture.player_address(), {0xF9, 0xAA, 0xAA, 0xAA}));
}

TEST_CASE("writing one byte submits one byte at the node's own offset", "[reclass][ui]")
{
    application();

    Fixture fixture;
    // A hex node is what draws a byte dump, so the byte writer needs one: Player
    // holds an int8 and a pointer, neither of which is a dump.
    REQUIRE(fixture.project.add_class("Flags", "app+0x40") != 0);
    fixture.project.classes().back().nodes.push_back(node_of(fixture.project, NodeKind::hex32, "bits"));
    fixture.structure.set_project(fixture.project);
    fixture.structure.set_class(2);
    fixture.resolve();
    fixture.pass();

    const std::uint64_t address = kAppBase + 0x40;
    fixture.write(address, {0x11, 0x22, 0x33, 0x44});

    QStringList messages;
    QObject::connect(&fixture.values,
                     &ReclassValueModel::statusChanged,
                     [&messages](const QString& message, bool)
                     {
                         messages.push_back(message);
                     });

    // A hex node's whole value is not typed into, but one of its bytes is, so
    // the dump and the Ascii column can be poked without retyping the value.
    const ReclassRow& row = fixture.row(0);
    CHECK_FALSE(fixture.values.editable(row));
    CHECK(fixture.values.byte_editable(row));

    REQUIRE(fixture.values.write_byte(row, 2, QStringLiteral("ff")));
    REQUIRE(drain_until_idle(fixture.worker));

    CHECK(messages == QStringList {QStringLiteral("Byte written.")});
    // Only the byte that was typed changed; the node's other bytes stand.
    CHECK(fixture.holds(address, {0x11, 0x22, 0xFF, 0x44}));

    // The number fields spell a byte: bare or `0x` hex, or `#` for decimal.
    REQUIRE(fixture.values.write_byte(row, 0, QStringLiteral("0x7A")));
    REQUIRE(drain_until_idle(fixture.worker));
    CHECK(fixture.holds(address, {0x7A, 0x22, 0xFF, 0x44}));

    // What was written shows at once, without waiting for the next pass.
    CHECK(fixture.values.node_bytes(row)[0] == static_cast<std::byte>(0x7A));
    CHECK(fixture.values.node_changed_bytes(row)[0] != 0);

    REQUIRE(fixture.values.write_byte(row, 1, QStringLiteral("#1")));
    REQUIRE(drain_until_idle(fixture.worker));
    CHECK(fixture.holds(address, {0x7A, 0x01, 0xFF, 0x44}));

    // One byte and no more: too many digits, a digit out of range, an empty
    // field and a number past a byte are all refused, and the target stands.
    for (const QString& refused :
         {QStringLiteral("ZZ"), QStringLiteral("1G"), QString(), QStringLiteral("100"), QStringLiteral("#300")})
    {
        CHECK_FALSE(fixture.values.write_byte(row, 0, refused));
        CHECK(messages.back() == QStringLiteral("Byte: expected one hex byte such as 1A."));
        CHECK(fixture.holds(address, {0x7A, 0x01, 0xFF, 0x44}));
    }

    // A byte the node does not cover is refused rather than written past it.
    CHECK_FALSE(fixture.values.write_byte(row, 4, QStringLiteral("1")));
    CHECK(messages.back() == QStringLiteral("Byte: the node is smaller than that byte."));

    // A kind neither writer takes never reaches the target.
    fixture.structure.set_class(1);
    const ReclassRow& pointer = fixture.row(1);
    CHECK_FALSE(fixture.values.byte_editable(pointer));
    CHECK_FALSE(fixture.values.write_byte(pointer, 0, QStringLiteral("1")));

    // Nor does a detached target or a block that was not read.
    fixture.target.clear();
    CHECK_FALSE(fixture.values.byte_editable(row));
    CHECK_FALSE(fixture.values.write_byte(row, 0, QStringLiteral("1")));
}

TEST_CASE("a value the node's kind cannot hold never reaches the target", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();
    fixture.pass();

    QString last;
    bool    failed = false;
    QObject::connect(&fixture.values,
                     &ReclassValueModel::statusChanged,
                     [&](const QString& message, bool error)
                     {
                         last   = message;
                         failed = error;
                     });

    fixture.write(fixture.player_address(), {0xAA, 0xAA, 0xAA, 0xAA});
    fixture.write(fixture.player_address(), {0xAA, 0xAA, 0xAA, 0xAA});
    CHECK_FALSE(fixture.values.write_value(fixture.row(0), QStringLiteral("out of range")));
    CHECK(failed);
    CHECK(last.startsWith(QStringLiteral("Value:")));

    // A kind the grid cannot type into refuses before the text is even parsed.
    failed = false;
    CHECK_FALSE(fixture.values.editable(fixture.row(1)));
    CHECK_FALSE(fixture.values.write_value(fixture.row(1), QStringLiteral("1")));
    CHECK(failed);
    CHECK(fixture.holds(fixture.player_address(), {0xAA, 0xAA, 0xAA, 0xAA}));
}

TEST_CASE("a write is refused while no target is attached or the block was not read", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();
    fixture.pass();

    QString last;
    bool    failed = false;
    QObject::connect(&fixture.values,
                     &ReclassValueModel::statusChanged,
                     [&](const QString& message, bool error)
                     {
                         last   = message;
                         failed = error;
                     });

    fixture.target.clear();
    fixture.write(fixture.player_address(), {0xAA, 0xAA, 0xAA, 0xAA});
    CHECK_FALSE(fixture.values.editable(fixture.row(0)));
    CHECK_FALSE(fixture.values.write_value(fixture.row(0), QStringLiteral("1")));
    CHECK(last == QStringLiteral("Not attached; cannot write."));

    // An unreadable block has no trustworthy address to write to.
    fixture.target = slopkit::test::fake_target();
    fixture.resolve();
    static_cast<void>(fixture.values.requests());
    fixture.values.apply_readings(std::vector<LiveReading> {
        LiveReading {.id = 0, .readable = false, .bytes = {}}
    });

    CHECK_FALSE(fixture.values.editable(fixture.row(0)));
    CHECK_FALSE(fixture.values.write_value(fixture.row(0), QStringLiteral("1")));
    CHECK(fixture.holds(fixture.player_address(), {0xAA, 0xAA, 0xAA, 0xAA}));
}

TEST_CASE("a second write is refused while one is in flight", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();
    fixture.pass();

    QString last;
    bool    failed = false;
    QObject::connect(&fixture.values,
                     &ReclassValueModel::statusChanged,
                     [&](const QString& message, bool error)
                     {
                         last   = message;
                         failed = error;
                     });

    fixture.write(fixture.player_address(), {0xAA, 0xAA, 0xAA, 0xAA});
    REQUIRE(fixture.values.write_value(fixture.row(0), QStringLiteral("1")));
    CHECK_FALSE(fixture.values.write_value(fixture.row(0), QStringLiteral("2")));
    CHECK(last == QStringLiteral("A write is already in progress."));
    CHECK(failed);

    REQUIRE(drain_until_idle(fixture.worker));
    CHECK(fixture.holds(fixture.player_address(), {0x01, 0xAA, 0xAA, 0xAA}));
}
