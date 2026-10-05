#include <catch2/catch.hpp>

#include "support/sandbox_helpers.hpp"

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
