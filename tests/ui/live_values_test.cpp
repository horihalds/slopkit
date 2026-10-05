#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the live coordinator runs one gated pass per interval", "[ui]")
{
    application();

    FakeAccess                      access;
    slopkit::process::AccessWorker  worker {access};
    slopkit::ui::SettingsController settings {scratch_settings_file("live_gate.ini")};

    StubLiveSurface surface;
    surface.requests.push_back({.id = 0, .address = 0x1000, .size = 4});

    // A detached target submits nothing.
    slopkit::process::AttachedTarget detached;
    slopkit::ui::LiveValues          detached_live {worker, detached, settings};
    detached_live.add_surface(&surface);
    detached_live.poll();
    CHECK(surface.request_calls == 0);

    // With a target the first poll submits one batched pass; no session is
    // attached to the worker yet, so the reading comes back unreadable.
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::ui::LiveValues          live {worker, target, settings};
    live.add_surface(&surface);
    live.poll();
    CHECK(surface.request_calls == 1);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return surface.apply_calls == 1;
                    }));
    REQUIRE(surface.readables.size() == 1);
    CHECK_FALSE(surface.readables[0]);

    // While a pass is in flight a second request does not queue another job.
    surface.request_calls = 0;
    live.request_now();
    live.request_now();
    CHECK(surface.request_calls == 1);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return surface.apply_calls == 2;
                    }));

    // An empty request set submits nothing.
    surface.requests.clear();
    surface.request_calls = 0;
    live.request_now();
    CHECK(surface.request_calls == 1);
    CHECK(surface.apply_calls == 2);

    // Disabled: nothing is requested even with a target attached.
    settings.set_live_update_enabled(false);
    surface.request_calls = 0;
    live.request_now();
    CHECK(surface.request_calls == 0);
}
