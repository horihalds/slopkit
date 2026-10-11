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

    slopkit::process::AttachedTarget detached;
    slopkit::ui::LiveValues          detached_live {worker, detached, settings};
    detached_live.add_surface(&surface);
    detached_live.poll();
    CHECK(surface.request_calls == 0);

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

    surface.request_calls = 0;
    live.request_now();
    live.request_now();
    CHECK(surface.request_calls == 1);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return surface.apply_calls == 2;
                    }));

    surface.requests.clear();
    surface.request_calls = 0;
    live.request_now();
    CHECK(surface.request_calls == 1);
    CHECK(surface.apply_calls == 2);

    settings.set_live_update_enabled(false);
    surface.request_calls = 0;
    live.request_now();
    CHECK(surface.request_calls == 0);
}

TEST_CASE("the live coordinator asks each ticker on the same cadence", "[ui]")
{
    application();

    FakeAccess                      access;
    slopkit::process::AccessWorker  worker {access};
    slopkit::ui::SettingsController settings {scratch_settings_file("live_ticker.ini")};

    slopkit::process::AttachedTarget detached;
    StubLiveTicker                   detached_ticker;
    slopkit::ui::LiveValues          detached_live {worker, detached, settings};
    detached_live.add_ticker(&detached_ticker);
    detached_live.poll();
    CHECK(detached_ticker.calls == 0);

    slopkit::process::AttachedTarget target = fake_target();

    SECTION("a ticker is asked once per interval, not on every poll")
    {
        StubLiveTicker ticker;
        ticker.submits_now = true;
        slopkit::ui::LiveValues live {worker, target, settings};
        live.add_ticker(&ticker);

        live.poll();
        CHECK(ticker.calls == 1);
        CHECK(ticker.submits == 1);

        // The ticker's submission advanced the cadence, so an immediate poll is
        // not due and must not ask again.
        live.poll();
        CHECK(ticker.calls == 1);
    }

    SECTION("a ticker is not asked while live update is off")
    {
        settings.set_live_update_enabled(false);

        StubLiveTicker ticker;
        ticker.submits_now = true;
        slopkit::ui::LiveValues live {worker, target, settings};
        live.add_ticker(&ticker);

        live.poll();
        CHECK(ticker.calls == 0);
    }

    SECTION("a ticker is asked even when nothing is displayed to read")
    {
        StubLiveSurface surface;
        StubLiveTicker  ticker;
        ticker.submits_now = true;
        slopkit::ui::LiveValues live {worker, target, settings};
        live.add_surface(&surface);
        live.add_ticker(&ticker);

        live.poll();
        CHECK(surface.request_calls == 1);
        CHECK(surface.apply_calls == 0);
        CHECK(ticker.calls == 1);
    }

    SECTION("a read pass in flight does not stop a tick")
    {
        StubLiveSurface surface;
        surface.requests.push_back({.id = 0, .address = 0x1000, .size = 4});
        StubLiveTicker ticker;
        ticker.submits_now = true;
        slopkit::ui::LiveValues live {worker, target, settings};
        live.add_surface(&surface);
        live.add_ticker(&ticker);

        live.poll();
        CHECK(surface.request_calls == 1);
        CHECK(ticker.calls == 1);

        live.request_now();
        CHECK(ticker.calls == 2);

        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return surface.apply_calls == 1;
                        }));
    }
}
