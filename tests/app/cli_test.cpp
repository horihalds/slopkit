#include <catch2/catch.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <unistd.h>

#include "app/cli.hpp"
#include "plugin/plugin_host.hpp"
#include "process/plugin_access.hpp"
#include "process/types.hpp"
#include "scan/engine.hpp"
#include "scan/source.hpp"

namespace
{
    using slopkit::process::RegionInfo;
    using slopkit::scan::ScanConfig;
    using slopkit::scan::ScanEngine;
    using slopkit::scan::ScanSnapshot;
    using slopkit::scan::ScanState;

    constexpr std::uint64_t kSentinel = 0x51F2C0DE1BADBEEFULL;

    ScanSnapshot wait(ScanEngine& engine)
    {
        for (int i = 0; i < 10000; ++i)
        {
            const auto snapshot = engine.snapshot();
            if (snapshot.state != ScanState::running)
            {
                return snapshot;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return engine.snapshot();
    }
} // namespace

TEST_CASE("the scan engine finds a sentinel in the current process through the plugin", "[scan][integration]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    auto       sentinel = std::make_unique<std::uint64_t>(kSentinel);
    const auto address  = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(sentinel.get()));

    auto session = access.attach(static_cast<slopkit::process::ProcessId>(::getpid()), "linux-proc");
    REQUIRE(session.has_value());

    const auto regions = session->regions();
    REQUIRE(regions.has_value());

    std::optional<RegionInfo> home;
    for (const auto& region : *regions)
    {
        if (address >= region.start && address < region.end)
        {
            home = region;
            break;
        }
    }
    REQUIRE(home.has_value());

    ScanConfig config;
    config.type                 = slopkit::scan::ScanType::exact_value;
    config.value_type           = slopkit::scan::ValueType::int64;
    config.value                = static_cast<std::int64_t>(kSentinel);
    config.filter.writable      = false;
    config.filter.executable    = false;
    config.filter.copy_on_write = false;
    config.filter.start         = home->start;
    config.filter.stop          = home->end;

    ScanEngine engine;
    engine.first_scan(config, slopkit::scan::make_session_source(*session));
    const auto snapshot = wait(engine);
    REQUIRE(snapshot.state == ScanState::done);

    bool found = false;
    for (const auto& hit : snapshot.hits)
    {
        if (hit.address == address)
        {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("the scan command reports an unknown pid", "[scan][integration]")
{
    std::ostringstream out;
    std::ostringstream err;

    const int code = slopkit::app::scan_process(out, err, 0x7fffffffu, "1234");
    CHECK(code != 0);
    CHECK(err.str().find("pid") != std::string::npos);
}

TEST_CASE("the scan command finds a sentinel value in the current process", "[scan][integration]")
{
    auto sentinel = std::make_unique<std::uint64_t>(kSentinel);

    std::ostringstream out;
    std::ostringstream err;

    const int code = slopkit::app::scan_process(out, err, static_cast<std::uint32_t>(::getpid()), "0x51F2C0DE1BADBEEF");

    REQUIRE(code == 0);
    CHECK(out.str().find("hit(s)") != std::string::npos);
}
