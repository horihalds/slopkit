#include <catch2/catch.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "support/ui_helpers.hpp"

#include "process/types.hpp"
#include "ui/models/process_list_model.hpp"

namespace
{
    slopkit::process::ProcessInfo make_process(std::uint32_t            pid,
                                               std::string              name,
                                               std::string              exe_path,
                                               std::string              plugin_id,
                                               std::vector<std::string> claimants)
    {
        slopkit::process::ProcessInfo info;
        info.pid       = pid;
        info.name      = std::move(name);
        info.exe_path  = std::move(exe_path);
        info.plugin_id = std::move(plugin_id);
        info.claimants = std::move(claimants);
        return info;
    }
} // namespace

TEST_CASE("the process model filters by name, executable, pid and plugin", "[ui]")
{
    application();

    slopkit::ui::models::ProcessListModel model;
    model.set_processes({make_process(100, "Mumble", "/usr/bin/mumble", "linux-proc", {"linux-proc"}),
                         make_process(200, "game.exe", "/games/game.exe", "wine-proton", {"wine-proton", "linux-proc"}),
                         make_process(300, "bash", "/usr/bin/bash", "linux-proc", {"linux-proc"})});
    model.set_application_index({"mumble"});

    CHECK(model.rowCount() == 3);
    CHECK(model.columnCount() == 2);
    CHECK(model.row_for_pid(200) >= 0);
    CHECK(model.row_for_pid(999) == -1);
    CHECK(model.process_at(-1) == nullptr);
    CHECK(model.process_at(model.rowCount()) == nullptr);

    // Name search, case-insensitive.
    model.set_search(QStringLiteral("muMbLe"));
    CHECK(model.rowCount() == 1);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->pid == 100);

    // Executable path search.
    model.set_search(QStringLiteral("/games/"));
    CHECK(model.rowCount() == 1);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->pid == 200);

    // PID search.
    model.set_search(QStringLiteral("300"));
    CHECK(model.rowCount() == 1);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->pid == 300);

    // A search that matches nothing empties the model.
    model.set_search(QStringLiteral("nothing-matches"));
    CHECK(model.rowCount() == 0);

    // The plugin filter keeps only the processes that plugin claims.
    model.set_search({});
    model.set_plugin_filter(QStringLiteral("wine-proton"));
    CHECK(model.rowCount() == 1);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->pid == 200);

    model.set_plugin_filter({});
    CHECK(model.rowCount() == 3);
}

TEST_CASE("the process model sorts by name and by pid", "[ui]")
{
    application();

    slopkit::ui::models::ProcessListModel model;
    model.set_processes({make_process(300, "zsh", "/bin/zsh", "linux-proc", {"linux-proc"}),
                         make_process(100, "vim", "/usr/bin/vim", "linux-proc", {"linux-proc"}),
                         make_process(200, "bash", "/usr/bin/bash", "linux-proc", {"linux-proc"})});

    model.sort(slopkit::ui::models::ProcessListModel::name, Qt::AscendingOrder);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->name == "bash");
    REQUIRE(model.process_at(2) != nullptr);
    CHECK(model.process_at(2)->name == "zsh");

    model.sort(slopkit::ui::models::ProcessListModel::pid, Qt::DescendingOrder);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->pid == 300);

    // The data() accessor follows the sort order.
    const QModelIndex first = model.index(0, slopkit::ui::models::ProcessListModel::name);
    CHECK(model.data(first, Qt::DisplayRole).toString() == QStringLiteral("zsh"));
}

TEST_CASE("the process model applications view keeps only desktop entries", "[ui]")
{
    application();

    slopkit::ui::models::ProcessListModel model;
    model.set_processes({make_process(100, "Mumble", "/usr/bin/mumble", "linux-proc", {"linux-proc"}),
                         make_process(200, "bash", "/usr/bin/bash", "linux-proc", {"linux-proc"})});
    model.set_application_index({"mumble"});

    CHECK(model.rowCount() == 2);
    model.set_applications_only(true);
    CHECK(model.rowCount() == 1);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->pid == 100);

    model.set_applications_only(false);
    CHECK(model.rowCount() == 2);
}

TEST_CASE("the process model is empty without processes", "[ui]")
{
    application();

    slopkit::ui::models::ProcessListModel model;
    CHECK(model.rowCount() == 0);
    CHECK(model.row_for_pid(42) == -1);
    CHECK(model.process_at(0) == nullptr);
    CHECK(model.data(model.index(0, slopkit::ui::models::ProcessListModel::pid), Qt::DisplayRole).toString().isEmpty());

    model.set_search(QStringLiteral("anything"));
    CHECK(model.rowCount() == 0);
}
