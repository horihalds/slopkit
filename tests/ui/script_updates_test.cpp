#include <catch2/catch.hpp>

#include <chrono>
#include <string>
#include <vector>

#include "script/types.hpp"
#include "support/ui_helpers.hpp"
#include "ui/script_updates.hpp"

namespace
{
    using slopkit::process::ScriptUpdateFailure;

    // Activates a script on the worker and waits for its verdict.
    bool activate_script(slopkit::process::AccessWorker& worker, std::string chunk)
    {
        bool ok = false;
        worker.submit_script(worker.next_job_id(),
                             "helper",
                             std::move(chunk),
                             std::string {slopkit::script::kActivateHook},
                             8,
                             [&](slopkit::process::JobResult&& result)
                             {
                                 const auto& ran = std::get<slopkit::process::ScriptResult>(std::move(result));
                                 ok              = ran.lifecycle.has_value() && ran.lifecycle->ok;
                             });
        pump_ui(worker,
                [&]
                {
                    return ok;
                });
        return ok;
    }
} // namespace

TEST_CASE("the script update ticker submits one pass per interval", "[ui]")
{
    application();

    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    slopkit::ui::ScriptUpdates     updates {worker};

    std::vector<ScriptUpdateFailure> failures;
    updates.set_failure_handler(
        [&failures](const ScriptUpdateFailure& failure)
        {
            failures.push_back(failure);
        });

    // Nothing is ticked yet: no job is submitted at all.
    CHECK_FALSE(updates.tick(std::chrono::milliseconds {50}));

    attach_app_session(worker);

    SECTION("a failing update reaches the failure handler")
    {
        REQUIRE(activate_script(worker, R"(
function activate() return true end
function update() return false, "nope" end
)"));

        CHECK(updates.tick(std::chrono::milliseconds {10}));
        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return !failures.empty();
                        }));
        REQUIRE(failures.size() == 1);
        CHECK(failures[0].description == "helper");
        CHECK(failures[0].reason == "nope");
        // The worker already ran deactivate and stopped ticking it.
        CHECK(worker.active_scripts() == 0);
    }

    SECTION("one pass in flight is not queued again")
    {
        REQUIRE(activate_script(worker, R"(
function activate() return true end
function update() end
)"));

        CHECK(updates.tick(std::chrono::milliseconds {10}));
        CHECK_FALSE(updates.tick(std::chrono::milliseconds {10}));

        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return worker.active_scripts() == 1 && failures.empty();
                        }));
    }

    SECTION("a second tick inside the interval is refused")
    {
        REQUIRE(activate_script(worker, R"(
function activate() return true end
function update() end
)"));

        CHECK(updates.tick(std::chrono::milliseconds {60'000}));
        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return failures.empty();
                        }));
        // The spacing rule refuses a second pass inside the same interval.
        CHECK_FALSE(updates.tick(std::chrono::milliseconds {60'000}));
    }
}
