#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
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
    using slopkit::ui::models::ReclassProxyModel;
    using slopkit::ui::models::ReclassStructureModel;
    using slopkit::ui::models::ReclassValueModel;

    constexpr std::uint64_t kAppBase = 0x400000;

    // Player { int8 tag; int32 health; } pinned at app+0x10, wired to the proxy
    // the way the window wires it.
    class Fixture
    {
    public:
        Fixture()
        {
            target = slopkit::test::fake_target();
            attach_session(worker);
            values.set_modules(std::vector<slopkit::process::ModuleInfo> {module_image("app", kAppBase, 0x1000)});
            values.set_structure(structure);

            project.add_class("Player", "app+0x10");

            Class& player = project.classes()[0];
            player.nodes.push_back(node_of(project, NodeKind::int8, "tag"));
            player.nodes.push_back(node_of(project, NodeKind::int32, "health"));

            structure.set_project(project);
            structure.set_class(0);

            proxy = std::make_unique<ReclassProxyModel>(structure, values);
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

        void stage(std::uint64_t address, std::initializer_list<unsigned> bytes)
        {
            stage_bytes(access, address, bytes);
        }

        // One live pass, run the way the window runs it: ask for the readings,
        // answer them from the target, then repaint what moved.
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
            proxy->refresh_values();
        }

        [[nodiscard]] QModelIndex index(int row, ReclassStructureModel::Column column) const
        {
            return proxy->index(row, column);
        }

        FakeAccess                         access;
        AccessWorker                       worker {access};
        AttachedTarget                     target;
        Project                            project;
        ReclassStructureModel              structure {project};
        ReclassValueModel                  values {worker, target};
        std::unique_ptr<ReclassProxyModel> proxy;
    };
}

TEST_CASE("a live pass repaints only the cells whose text moved", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();

    std::vector<QModelIndex> repainted;
    QObject::connect(fixture.proxy.get(),
                     &QAbstractItemModel::dataChanged,
                     [&repainted](const QModelIndex& first, const QModelIndex& last, const QList<int>&)
                     {
                         repainted.push_back(first);
                         if (last != first)
                         {
                             repainted.push_back(last);
                         }
                     });

    fixture.stage(fixture.player_address(), {1, 2, 0, 0, 0});
    fixture.pass();
    repainted.clear();

    // Nothing in the target moved: one pass later the grid is left alone.
    fixture.pass();
    CHECK(repainted.empty());

    // The health field alone changed, so its own value cell is the one cell
    // that is repainted.
    fixture.stage(fixture.player_address() + 1, {3, 0, 0, 0});
    fixture.pass();
    REQUIRE(repainted.size() == 1);
    CHECK(repainted[0].column() == ReclassStructureModel::value);
    CHECK(repainted[0].row() == 1);
    CHECK(fixture.proxy->data(fixture.index(1, ReclassStructureModel::value), Qt::DisplayRole).toString()
          == QStringLiteral("3  0x00000003"));
}

TEST_CASE("a rebuilt grid fills the repaint cache without repainting", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();
    fixture.stage(fixture.player_address(), {1, 2, 0, 0, 0});
    fixture.pass();

    std::vector<QModelIndex> repainted;
    QObject::connect(fixture.proxy.get(),
                     &QAbstractItemModel::dataChanged,
                     [&repainted](const QModelIndex& first, const QModelIndex&, const QList<int>&)
                     {
                         repainted.push_back(first);
                     });

    // The rebuild already repainted the whole grid through its reset, so the
    // pass that follows must not repaint it a second time.
    fixture.structure.rebuild();
    fixture.pass();
    CHECK(repainted.empty());
}

TEST_CASE("a value that moved is highlighted until the next pass", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();
    fixture.stage(fixture.player_address(), {1, 2, 0, 0, 0});
    fixture.pass();

    const QModelIndex health = fixture.index(1, ReclassStructureModel::value);
    CHECK_FALSE(fixture.proxy->data(health, Qt::ForegroundRole).isValid());

    fixture.stage(fixture.player_address() + 1, {0x2A, 0, 0, 0});
    fixture.pass();
    CHECK(fixture.proxy->data(health, Qt::DisplayRole).toString() == QStringLiteral("42  0x0000002A"));
    CHECK(fixture.proxy->data(health, Qt::ForegroundRole).value<QColor>() == slopkit::ui::active_theme().warning);

    // The cell draws the value beside its hex form, while the editor starts
    // from the value alone, so what the user types is what a write parses.
    CHECK(fixture.proxy->data(health, Qt::EditRole).toString() == QStringLiteral("42"));

    // The row keeps its value and drops the highlight on the next pass.
    fixture.pass();
    CHECK(fixture.proxy->data(health, Qt::DisplayRole).toString() == QStringLiteral("42  0x0000002A"));
    CHECK_FALSE(fixture.proxy->data(health, Qt::ForegroundRole).isValid());
}

TEST_CASE("an unreadable block mutes every value cell of the class", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();
    fixture.stage(fixture.player_address(), {1, 2, 0, 0, 0});
    fixture.pass();

    static_cast<void>(fixture.values.requests());
    fixture.values.apply_readings(std::vector<LiveReading> {
        LiveReading {.id = 0, .readable = false, .bytes = {}}
    });
    fixture.proxy->refresh_values();

    for (int row = 0; row < 2; ++row)
    {
        const QModelIndex value = fixture.index(row, ReclassStructureModel::value);
        CHECK(fixture.proxy->data(value, Qt::DisplayRole).toString() == QStringLiteral("?"));
        CHECK(fixture.proxy->data(value, Qt::ForegroundRole).value<QColor>() == slopkit::ui::active_theme().text_muted);
    }
}

TEST_CASE("the live columns take the theme's colours per role", "[reclass][ui]")
{
    application();

    Fixture fixture;
    fixture.resolve();
    fixture.stage(fixture.player_address(), {1, 2, 0, 0, 0});
    fixture.pass();

    const slopkit::ui::Theme& theme = slopkit::ui::active_theme();
    CHECK(fixture.proxy->data(fixture.index(0, ReclassStructureModel::address), Qt::ForegroundRole).value<QColor>()
          == theme.syntax_immediate);
    CHECK(fixture.proxy->data(fixture.index(0, ReclassStructureModel::value), Qt::ForegroundRole).isValid() == false);
    // The static columns keep the structure model's own colouring.
    CHECK(fixture.proxy->data(fixture.index(0, ReclassStructureModel::type), Qt::ForegroundRole).value<QColor>()
          == theme.syntax_keyword);
    CHECK(fixture.proxy->data(fixture.index(0, ReclassStructureModel::offset), Qt::ForegroundRole).value<QColor>()
          == theme.text_muted);
    CHECK_FALSE(fixture.proxy->data(fixture.index(0, ReclassStructureModel::name), Qt::ForegroundRole).isValid());

    // An unresolved class address is muted rather than dressed as a real one.
    Fixture unresolved;
    unresolved.stage(unresolved.player_address(), {1, 2, 0, 0, 0});
    CHECK(unresolved.proxy->data(unresolved.index(0, ReclassStructureModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("not resolved yet"));
    CHECK(
        unresolved.proxy->data(unresolved.index(0, ReclassStructureModel::address), Qt::ForegroundRole).value<QColor>()
        == theme.text_muted);
}
