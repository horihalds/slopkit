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
    QApplication& application()
    {
        static int          argc      = 1;
        static char         program[] = "slopkit_tests";
        static char*        argv[]    = {program, nullptr};
        static QApplication instance(argc, argv);
        return instance;
    }

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

    std::optional<RegionInfo> region_containing(const std::vector<RegionInfo>& regions, std::uint64_t address)
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
    std::uint64_t address_of(const T& field)
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
    bool scan_finds(slopkit::process::Session& session, ScanConfig config, std::uint64_t address)
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

TEST_CASE("the sandbox exposes one field per slopkit value type", "[sandbox]")
{
    using slopkit::scan::value_size;
    using slopkit::scan::ValueType;

    static_assert(std::is_same_v<decltype(Values::byte_value), std::uint8_t>);
    static_assert(std::is_same_v<decltype(Values::small_value), std::int16_t>);
    static_assert(std::is_same_v<decltype(Values::health), std::int32_t>);
    static_assert(std::is_same_v<decltype(Values::score), std::int64_t>);
    static_assert(std::is_same_v<decltype(Values::speed), float>);
    static_assert(std::is_same_v<decltype(Values::precision), double>);
    static_assert(std::is_same_v<decltype(Values::banner), std::array<char, 32>>);
    static_assert(std::is_same_v<decltype(Values::pattern), std::array<std::uint8_t, 8>>);

    CHECK(sizeof(decltype(Values::byte_value)) == value_size(ValueType::byte));
    CHECK(sizeof(decltype(Values::small_value)) == value_size(ValueType::int16));
    CHECK(sizeof(decltype(Values::health)) == value_size(ValueType::int32));
    CHECK(sizeof(decltype(Values::score)) == value_size(ValueType::int64));
    CHECK(sizeof(decltype(Values::speed)) == value_size(ValueType::float32));
    CHECK(sizeof(decltype(Values::precision)) == value_size(ValueType::float64));
    CHECK(std::tuple_size_v<decltype(Values::banner)> > std::string_view {"SLOPKIT-PRACTICE-TARGET"}.size());
    CHECK(std::tuple_size_v<decltype(Values::pattern)> == 8);
}

TEST_CASE("every sandbox field lies inside the Values object", "[sandbox]")
{
    constexpr std::size_t size = sizeof(Values);

    CHECK(offsetof(Values, byte_value) + sizeof(Values::byte_value) <= size);
    CHECK(offsetof(Values, small_value) + sizeof(Values::small_value) <= size);
    CHECK(offsetof(Values, health) + sizeof(Values::health) <= size);
    CHECK(offsetof(Values, score) + sizeof(Values::score) <= size);
    CHECK(offsetof(Values, speed) + sizeof(Values::speed) <= size);
    CHECK(offsetof(Values, precision) + sizeof(Values::precision) <= size);
    CHECK(offsetof(Values, banner) + sizeof(Values::banner) <= size);
    CHECK(offsetof(Values, pattern) + sizeof(Values::pattern) <= size);
    CHECK(offsetof(Values, drift) + sizeof(Values::drift) <= size);
}

TEST_CASE("advance animates only health, score and drift", "[sandbox]")
{
    slopkit::sandbox::reset();
    const Values before = slopkit::sandbox::values();

    slopkit::sandbox::advance();
    const Values& after = slopkit::sandbox::values();

    CHECK(after.health == before.health - 1);
    CHECK(after.score == before.score + slopkit::sandbox::kScoreStep);
    CHECK(after.drift == before.drift + slopkit::sandbox::kDriftStep);

    CHECK(after.byte_value == before.byte_value);
    CHECK(after.small_value == before.small_value);
    CHECK(after.speed == before.speed);
    CHECK(after.precision == before.precision);
    CHECK(after.banner == before.banner);
    CHECK(after.pattern == before.pattern);
}

TEST_CASE("reset restores every initial value", "[sandbox]")
{
    for (int i = 0; i < 16; ++i)
    {
        slopkit::sandbox::advance();
    }
    slopkit::sandbox::reset();

    const Values  expected {};
    const Values& actual = slopkit::sandbox::values();

    CHECK(actual.byte_value == expected.byte_value);
    CHECK(actual.small_value == expected.small_value);
    CHECK(actual.health == expected.health);
    CHECK(actual.score == expected.score);
    CHECK(actual.speed == expected.speed);
    CHECK(actual.precision == expected.precision);
    CHECK(actual.banner == expected.banner);
    CHECK(actual.pattern == expected.pattern);
    CHECK(actual.drift == expected.drift);
}

TEST_CASE("advance wraps the int32 health instead of overflowing", "[sandbox]")
{
    slopkit::sandbox::reset();
    slopkit::sandbox::values().health = slopkit::sandbox::kHealthFloor;
    slopkit::sandbox::advance();
    CHECK(slopkit::sandbox::values().health == slopkit::sandbox::kHealthStart);

    slopkit::sandbox::values().health = std::numeric_limits<std::int32_t>::min();
    slopkit::sandbox::advance();
    CHECK(slopkit::sandbox::values().health == slopkit::sandbox::kHealthStart);
}

TEST_CASE("the heap marker holds the ASCII marker at a stable address", "[sandbox]")
{
    const std::span<std::byte> marker = slopkit::sandbox::heap_marker();
    REQUIRE(marker.size() >= std::string_view {"SLOPKIT-SANDBOX-MARKER"}.size());

    const std::string_view text(reinterpret_cast<const char*>(marker.data()),
                                std::string_view {"SLOPKIT-SANDBOX-MARKER"}.size());
    CHECK(text == "SLOPKIT-SANDBOX-MARKER");
    CHECK(slopkit::sandbox::heap_marker().data() == marker.data());
}

TEST_CASE("print_layout emits one parseable line per field", "[sandbox]")
{
    std::ostringstream out;
    slopkit::sandbox::print_layout(out);
    const std::string text = out.str();

    for (const std::string_view name : {"byte_value",
                                        "small_value",
                                        "health",
                                        "score",
                                        "speed",
                                        "precision",
                                        "banner",
                                        "pattern",
                                        "drift",
                                        "heap_marker"})
    {
        const auto at = text.find(name);
        REQUIRE(at != std::string::npos);
        const std::string line = text.substr(at, text.find('\n', at) - at);
        CHECK(line.find(" address=0x") != std::string::npos);
        CHECK(line.find(" size=") != std::string::npos);
        CHECK(line.find(" value=") != std::string::npos);
    }

    CHECK(std::ranges::count(text, '\n') == 10);
}

TEST_CASE("the scan engine finds the sandbox values in the current process", "[sandbox][scan]")
{
    slopkit::sandbox::reset();

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    auto session = access.attach(static_cast<slopkit::process::ProcessId>(::getpid()), "linux-proc");
    REQUIRE(session.has_value());

    SECTION("the int32 health field")
    {
        const std::uint64_t address = address_of(slopkit::sandbox::values().health);

        ScanConfig config;
        config.type       = ScanType::exact_value;
        config.value_type = ValueType::int32;
        config.value      = static_cast<std::int64_t>(slopkit::sandbox::values().health);

        CHECK(scan_finds(*session, config, address));
    }

    SECTION("the string banner")
    {
        const std::uint64_t address = address_of(slopkit::sandbox::values().banner);

        ScanConfig config;
        config.type       = ScanType::exact_value;
        config.value_type = ValueType::string;
        config.value      = std::string("SLOPKIT-PRACTICE-TARGET");

        CHECK(scan_finds(*session, config, address));
    }

    SECTION("the heap marker's ASCII marker")
    {
        const std::span<std::byte> marker  = slopkit::sandbox::heap_marker();
        const std::uint64_t        address = reinterpret_cast<std::uintptr_t>(marker.data());

        ScanConfig config;
        config.type       = ScanType::exact_value;
        config.value_type = ValueType::string;
        config.value      = std::string("SLOPKIT-SANDBOX-MARKER");

        CHECK(scan_finds(*session, config, address));
    }
}

TEST_CASE("the sandbox window shows every value with its name and type", "[ui][sandbox]")
{
    application();
    slopkit::sandbox::reset();

    slopkit::sandbox::SandboxWindow window;
    window.show();
    QApplication::processEvents();

    CHECK(window.windowTitle() == QStringLiteral("slopkit sandbox"));
    CHECK(window.minimumWidth() > 0);
    CHECK(window.minimumHeight() > 0);

    struct Expected
    {
        const char* name;
        const char* type;
    };

    const Expected expected[] = {
        { "byte_value",          "Byte"},
        {"small_value",       "2 Bytes"},
        {     "health",       "4 Bytes"},
        {      "score",       "8 Bytes"},
        {      "speed",         "Float"},
        {  "precision",        "Double"},
        {     "banner",        "String"},
        {    "pattern", "Array of byte"},
        {      "drift",         "Float"},
        {"heap_marker", "Array of byte"},
    };

    for (const auto& item : expected)
    {
        const QString name = QString::fromUtf8(item.name);

        auto* label = window.findChild<QLabel*>(QStringLiteral("name_%1").arg(name));
        auto* type  = window.findChild<QLabel*>(QStringLiteral("type_%1").arg(name));
        auto* value = window.findChild<QLineEdit*>(QStringLiteral("value_%1").arg(name));

        REQUIRE(label != nullptr);
        REQUIRE(type != nullptr);
        REQUIRE(value != nullptr);
        CHECK(label->text() == name);
        CHECK(type->text() == QString::fromUtf8(item.type));
        CHECK_FALSE(value->text().isEmpty());
    }

    CHECK_FALSE(window.findChild<QLineEdit*>(QStringLiteral("value_health"))->isReadOnly());
    CHECK(window.findChild<QLineEdit*>(QStringLiteral("value_pattern"))->isReadOnly());
    CHECK(window.findChild<QLineEdit*>(QStringLiteral("value_heap_marker"))->isReadOnly());

    auto* address = window.findChild<QLabel*>(QStringLiteral("heap_marker_address"));
    REQUIRE(address != nullptr);
    CHECK(address->text().startsWith(QStringLiteral("0x")));
}

TEST_CASE("a write through the value store becomes visible after one refresh", "[ui][sandbox]")
{
    application();
    slopkit::sandbox::reset();

    slopkit::sandbox::SandboxWindow window;
    window.set_paused(true);

    slopkit::sandbox::values().health = 42;
    window.refresh();

    auto* health = window.findChild<QLineEdit*>(QStringLiteral("value_health"));
    REQUIRE(health != nullptr);
    CHECK(health->text() == QStringLiteral("42"));
}

TEST_CASE("a refresh leaves a focused editor alone", "[ui][sandbox]")
{
    application();
    slopkit::sandbox::reset();

    slopkit::sandbox::SandboxWindow window;
    window.set_paused(true);
    window.show();
    QApplication::processEvents();

    auto* health = window.findChild<QLineEdit*>(QStringLiteral("value_health"));
    REQUIRE(health != nullptr);
    health->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    REQUIRE(health->hasFocus());

    health->setText(QStringLiteral("123"));
    slopkit::sandbox::values().health = 7;
    window.refresh();

    CHECK(health->text() == QStringLiteral("123"));
}

TEST_CASE("an edited row writes back into the value store", "[ui][sandbox]")
{
    application();
    slopkit::sandbox::reset();

    slopkit::sandbox::SandboxWindow window;
    window.set_paused(true);

    auto* health = window.findChild<QLineEdit*>(QStringLiteral("value_health"));
    REQUIRE(health != nullptr);

    health->setText(QStringLiteral("321"));
    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(health, &enter);

    CHECK(slopkit::sandbox::values().health == 321);
}

TEST_CASE("pause stops the animation timer and resume restarts it", "[ui][sandbox]")
{
    application();

    slopkit::sandbox::SandboxWindow window;

    auto* animation = window.findChild<QTimer*>(QStringLiteral("animation_timer"));
    auto* refresh   = window.findChild<QTimer*>(QStringLiteral("refresh_timer"));
    REQUIRE(animation != nullptr);
    REQUIRE(refresh != nullptr);
    CHECK(animation->parent() == &window);
    CHECK(refresh->parent() == &window);
    CHECK(animation->isActive());
    CHECK(refresh->isActive());

    window.set_paused(true);
    CHECK(window.is_paused());
    CHECK_FALSE(animation->isActive());

    window.set_paused(false);
    CHECK_FALSE(window.is_paused());
    CHECK(animation->isActive());
}

TEST_CASE("the reset button restores every initial value", "[ui][sandbox]")
{
    application();
    slopkit::sandbox::reset();

    slopkit::sandbox::SandboxWindow window;
    window.set_paused(true);

    slopkit::sandbox::values().health = 5;
    slopkit::sandbox::values().score  = 9999;
    window.refresh();

    auto* reset = window.findChild<QPushButton*>(QStringLiteral("reset_button"));
    REQUIRE(reset != nullptr);
    reset->click();

    const Values expected {};
    CHECK(slopkit::sandbox::values().health == expected.health);
    CHECK(slopkit::sandbox::values().score == expected.score);

    auto* health = window.findChild<QLineEdit*>(QStringLiteral("value_health"));
    REQUIRE(health != nullptr);
    CHECK(health->text() == QString::number(static_cast<int>(expected.health)));
}

TEST_CASE("destroying the window cleans up without a stray animation tick", "[ui][sandbox]")
{
    application();
    slopkit::sandbox::reset();
    const std::int32_t before = slopkit::sandbox::values().health;

    {
        slopkit::sandbox::SandboxWindow window;
        QApplication::processEvents();
    }

    CHECK(slopkit::sandbox::values().health == before);
}

TEST_CASE("the sandbox binary resolves next to the running executable", "[app][sandbox]")
{
    const auto path = slopkit::app::sandbox_binary_path();
    REQUIRE(path.has_value());
    CHECK(path->filename() == std::filesystem::path {"slopkit-sandbox"});
    CHECK(std::filesystem::is_regular_file(*path));
}

TEST_CASE("an empty directory yields no sandbox binary and a readable reason", "[app][sandbox]")
{
    const std::filesystem::path empty = std::filesystem::path {SLOPKIT_TMP_DIR} / "sandbox_test_empty";
    std::error_code             error;
    std::filesystem::remove_all(empty, error);
    std::filesystem::create_directories(empty, error);

    CHECK_FALSE(slopkit::app::sandbox_binary_path(empty).has_value());

    const auto result = slopkit::app::launch_sandbox(empty);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("slopkit-sandbox") != std::string::npos);
}

TEST_CASE("launching the sandbox returns a live pid", "[app][sandbox]")
{
    REQUIRE(slopkit::app::sandbox_binary_path().has_value());

    const auto result = slopkit::app::launch_sandbox();
    REQUIRE(result.has_value());
    CHECK(*result > 0);

    // The sandbox runs in its own session and is meant to outlive slopkit; stop
    // and reap the instance this test spawned so nothing is left behind.
    const auto pid = static_cast<pid_t>(*result);
    static_cast<void>(::kill(pid, SIGKILL));
    static_cast<void>(::waitpid(pid, nullptr, 0));
}

TEST_CASE("a launched practice target's memory is readable and scannable", "[app][sandbox]")
{
    REQUIRE(slopkit::app::sandbox_binary_path().has_value());

    const auto launched = slopkit::app::launch_sandbox();
    REQUIRE(launched.has_value());
    REQUIRE(*launched > 0);

    LaunchedTarget target {static_cast<pid_t>(*launched)};

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    const auto target_pid = static_cast<slopkit::process::ProcessId>(*launched);

    // The child needs a moment before it has mapped its image; retry briefly.
    // Before this fix the read always failed with permission_denied, so the loop
    // would exhaust and the REQUIRE below would fail.
    std::optional<slopkit::process::Session> session;
    std::optional<std::vector<std::byte>>    header;
    std::uint64_t                            image_base = 0;
    std::uint64_t                            image_size = 0;
    for (int attempt = 0; attempt < 300 && !header.has_value(); ++attempt)
    {
        auto candidate = access.attach(target_pid, "linux-proc");
        if (candidate.has_value())
        {
            const auto  modules = candidate->modules();
            const auto* main    = modules.has_value() ? slopkit::process::main_module(*modules) : nullptr;
            if (main != nullptr && main->base != 0)
            {
                auto bytes = candidate->read(main->base, 4);
                if (bytes.has_value() && bytes->size() == 4)
                {
                    image_base = main->base;
                    image_size = main->size;
                    header     = std::move(*bytes);
                    session    = std::move(*candidate);
                }
            }
        }
        if (!header.has_value())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    REQUIRE(session.has_value());
    REQUIRE(header.has_value());
    CHECK((*header)[0] == std::byte {0x7F});
    CHECK((*header)[1] == std::byte {'E'});
    CHECK((*header)[2] == std::byte {'L'});
    CHECK((*header)[3] == std::byte {'F'});

    // The user story in its out-of-process form: scanning the launched target's
    // main image finds a constant practice value. The animated health/score are
    // avoided because they change every tick.
    ScanConfig config;
    config.type                 = ScanType::exact_value;
    config.value_type           = ValueType::string;
    config.value                = std::string {"SLOPKIT-PRACTICE-TARGET"};
    config.filter.writable      = false;
    config.filter.executable    = false;
    config.filter.copy_on_write = false;
    config.filter.start         = image_base;
    config.filter.stop          = image_base + image_size;
    config.filter.alignment     = 1;

    ScanEngine engine;
    engine.first_scan(config, slopkit::scan::make_session_source(*session));
    const auto snapshot = wait(engine);
    REQUIRE(snapshot.state == ScanState::done);
    REQUIRE(snapshot.result_hits != nullptr);
    CHECK_FALSE(snapshot.result_hits->empty());
}
