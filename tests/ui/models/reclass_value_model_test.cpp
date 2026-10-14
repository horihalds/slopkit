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
    using slopkit::test::module_image;
    using slopkit::test::node_of;
    using slopkit::test::reading_for;
    using slopkit::test::stage_bytes;
    using slopkit::ui::LiveReading;
    using slopkit::ui::LiveRequest;
    using slopkit::ui::models::ReclassRow;
    using slopkit::ui::models::ReclassStructureModel;
    using slopkit::ui::models::ReclassValueModel;

    constexpr std::uint64_t kAppBase  = 0x400000;
    constexpr std::uint64_t kHeapBase = 0x600000;

    // Player { int8 tag; class_pointer next; } and Point { int32 x; int32 y; },
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

    CHECK(fixture.values.address_text(fixture.row(0)) == QStringLiteral("0x0000000000400010"));
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

    fixture.write(fixture.player_address(), {0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A});
    fixture.pass();

    // Each cell draws the node's value: the integer beside its hex form, the
    // class pointer as the address it holds.
    CHECK(fixture.values.value_text(fixture.row(0)) == QStringLiteral("42  0x2A"));
    CHECK(fixture.values.value_text(fixture.row(1)) == QStringLiteral("-> 0x2A00000000000000"));

    // The address column follows the node's own offset inside the class.
    CHECK(fixture.values.address_text(fixture.row(1)) == QStringLiteral("0x0000000000400011"));
    CHECK(fixture.values.node_address(fixture.row(1)) == fixture.player_address() + 1);
}

TEST_CASE("a hex node whose bytes land in a module hints at the pointer", "[reclass][ui]")
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
    CHECK(text.startsWith(QStringLiteral("4000400000000000")));
    CHECK(text.endsWith(QStringLiteral("-> 0x0000000000400040")));

    // A value outside every module is only its own readings.
    fixture.write(fixture.player_address() + 9, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x00});
    fixture.pass();
    CHECK_FALSE(fixture.values.value_text(fixture.row(2)).contains(QStringLiteral("->")));
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
    CHECK(fixture.values.value_text(fixture.row(1)) == QStringLiteral("?"));
    // The address is known even when the page is not.
    CHECK(fixture.values.address_text(fixture.row(0)) == QStringLiteral("0x0000000000400010"));
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

    fixture.structure.set_expanded(fixture.project.classes()[1].nodes[1].id, true);

    // One pass later the pointer's target is known, read and shown.
    const std::vector<LiveRequest> requests = fixture.pass();
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].address == kHeapBase);
    CHECK(requests[1].size == 8);

    REQUIRE(fixture.structure.rowCount(fixture.structure.index(1, 0)) == 2);
    CHECK(fixture.values.value_text(fixture.child(1, 0)) == QStringLiteral("7  0x00000007"));
    CHECK(fixture.values.value_text(fixture.child(1, 1)) == QStringLiteral("9  0x00000009"));
    CHECK(fixture.values.node_address(fixture.child(1, 1)) == kHeapBase + 4);
    CHECK(fixture.values.address_text(fixture.child(1, 1)) == QStringLiteral("0x0000000000600004"));
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
