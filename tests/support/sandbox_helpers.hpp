#pragma once

#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QTimer>

#include "app/sandbox.hpp"
#include "plugin/plugin_host.hpp"
#include "process/plugin_access.hpp"
#include "process/types.hpp"
#include "sandbox/sandbox_values.hpp"
#include "sandbox/sandbox_window.hpp"
#include "scan/engine.hpp"
#include "scan/source.hpp"
#include "scan/types.hpp"

namespace
{
    using slopkit::process::RegionInfo;
    using slopkit::sandbox::Values;
    using slopkit::scan::ScanConfig;
    using slopkit::scan::ScanEngine;
    using slopkit::scan::ScanSnapshot;
    using slopkit::scan::ScanState;
    using slopkit::scan::ScanType;
    using slopkit::scan::ValueType;

    // A QApplication may only exist once per process; Catch2 normally runs each
    // case in its own process, but the binary accepts several.
    [[maybe_unused]] QApplication& application()
    {
        static int          argc      = 1;
        static char         program[] = "slopkit_tests";
        static char*        argv[]    = {program, nullptr};
        static QApplication instance(argc, argv);
        return instance;
    }

    [[maybe_unused]] ScanSnapshot wait(ScanEngine& engine)
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

    [[maybe_unused]] std::optional<RegionInfo> region_containing(const std::vector<RegionInfo>& regions,
                                                                 std::uint64_t                  address)
    {
        for (const auto& region : regions)
        {
            if (address >= region.start && address < region.end)
            {
                return region;
            }
        }
        return std::nullopt;
    }

    template<typename T>
    [[maybe_unused]] std::uint64_t address_of(const T& field)
    {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&field));
    }

    // Kills and reaps a practice target a test launched, so a failing assertion
    // never leaves the sandbox running (or a zombie behind).
    struct LaunchedTarget
    {
        pid_t pid {-1};

        ~LaunchedTarget()
        {
            if (pid > 0)
            {
                static_cast<void>(::kill(pid, SIGKILL));
                static_cast<void>(::waitpid(pid, nullptr, 0));
            }
        }
    };

    // Runs one exact-value first scan over the region holding `address` and
    // returns whether the real engine stored that address among its hits.
    [[maybe_unused]] bool scan_finds(slopkit::process::Session& session, ScanConfig config, std::uint64_t address)
    {
        const auto regions = session.regions();
        REQUIRE(regions.has_value());

        const auto home = region_containing(*regions, address);
        REQUIRE(home.has_value());

        config.filter.writable      = false;
        config.filter.executable    = false;
        config.filter.copy_on_write = false;
        config.filter.start         = home->start;
        config.filter.stop          = home->end;
        config.filter.alignment     = 1;

        ScanEngine engine;
        engine.first_scan(config, slopkit::scan::make_session_source(session));
        const auto snapshot = wait(engine);
        REQUIRE(snapshot.state == ScanState::done);
        REQUIRE(snapshot.result_hits != nullptr);

        return std::ranges::any_of(*snapshot.result_hits,
                                   [address](const auto& hit)
                                   {
                                       return hit.address == address;
                                   });
    }
} // namespace
