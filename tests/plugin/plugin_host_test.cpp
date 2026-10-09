#include <catch2/catch.hpp>

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/cli.hpp"
#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "core/version.hpp"
#include "plugin/plugin_host.hpp"

namespace
{
    void set_env(const char* name, const char* value)
    {
        ::setenv(name, value, 1);
    }

    void unset_env(const char* name)
    {
        ::unsetenv(name);
    }

    // Restores the process-wide log level on scope exit so a test that pins it
    // cannot leak the setting into the next case.
    class LevelGuard
    {
    public:
        LevelGuard() : previous_(slopkit::log::Logger::instance().minimum_level()) {}

        LevelGuard(const LevelGuard&)            = delete;
        LevelGuard& operator=(const LevelGuard&) = delete;

        ~LevelGuard()
        {
            slopkit::log::Logger::instance().set_minimum_level(previous_);
        }

    private:
        slopkit::log::Level previous_;
    };

    // Registers a sink for the lifetime of the guard.
    class SinkGuard
    {
    public:
        explicit SinkGuard(slopkit::log::Sink sink) : id_(slopkit::log::Logger::instance().add_sink(std::move(sink))) {}

        SinkGuard(const SinkGuard&)            = delete;
        SinkGuard& operator=(const SinkGuard&) = delete;

        ~SinkGuard()
        {
            slopkit::log::Logger::instance().remove_sink(id_);
        }

    private:
        slopkit::log::SinkId id_;
    };
} // namespace

TEST_CASE("PluginHost diagnoses broken plugin libraries", "[plugin]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_TEST_PLUGIN_DIR});

    REQUIRE(host.plugins().empty());

    bool saw_abi   = false;
    bool saw_entry = false;
    for (const auto& diagnostic : host.diagnostics())
    {
        REQUIRE_FALSE(diagnostic.message.empty());
        saw_abi   = saw_abi || diagnostic.message.find("ABI") != std::string::npos;
        saw_entry = saw_entry || diagnostic.message.find("entry") != std::string::npos;
    }
    REQUIRE(saw_abi);
    REQUIRE(saw_entry);
}

TEST_CASE("A plugin without the regions entry point is rejected", "[plugin]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_TEST_PLUGIN_DIR});

    bool saw_small_struct = false;
    for (const auto& diagnostic : host.diagnostics())
    {
        saw_small_struct = saw_small_struct || diagnostic.message.find("struct too small") != std::string::npos;
    }
    REQUIRE(saw_small_struct);
    REQUIRE(host.find("old-abi") == nullptr);
}

TEST_CASE("A plugin built against an older ABI minor is rejected", "[plugin]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_TEST_PLUGIN_DIR});

    bool saw_old_minor = false;
    for (const auto& diagnostic : host.diagnostics())
    {
        saw_old_minor = saw_old_minor
                     || (diagnostic.message.find("incompatible ABI version") != std::string::npos
                         && diagnostic.message.find("1.1") != std::string::npos);
    }
    REQUIRE(saw_old_minor);
}

TEST_CASE("A missing plugin directory is not fatal", "[plugin]")
{
    SECTION("discovering a missing directory yields nothing and no diagnostics")
    {
        slopkit::plugin::PluginHost host;
        host.discover({"/nonexistent/slopkit/plugins"});

        REQUIRE(host.plugins().empty());
        REQUIRE(host.diagnostics().empty());
    }

    SECTION("discovering an installed directory that does not exist is silent")
    {
        const auto directories = slopkit::plugin::PluginHost::default_search_directories("/nonexistent/slopkit/bin");

        slopkit::plugin::PluginHost host;
        host.discover(directories);

        REQUIRE(host.plugins().empty());
        REQUIRE(host.diagnostics().empty());
    }
}

TEST_CASE("SLOPKIT_PLUGIN_PATH is split on colons", "[plugin]")
{
    const auto paths = slopkit::plugin::PluginHost::split_search_path("/a/b::/c:/d");

    REQUIRE(paths.size() == 3);
    REQUIRE(paths[0] == std::filesystem::path("/a/b"));
    REQUIRE(paths[1] == std::filesystem::path("/c"));
    REQUIRE(paths[2] == std::filesystem::path("/d"));

    REQUIRE(slopkit::plugin::PluginHost::split_search_path("").empty());
}

TEST_CASE("Default plugin search directories cover the build tree and the install", "[plugin]")
{
    const auto directories = slopkit::plugin::PluginHost::default_search_directories("/prefix/bin");

    REQUIRE(directories.size() >= 2);
    REQUIRE(directories[0] == std::filesystem::path("/prefix/bin/plugins"));

    const auto installed = std::filesystem::path("/prefix/bin") / SLOPKIT_PLUGIN_RELATIVE_DIR;
    REQUIRE(directories[1] == installed);
    REQUIRE(installed.filename() == "plugins");
    REQUIRE(installed.parent_path().filename() == "slopkit");
}

TEST_CASE("SLOPKIT_PLUGIN_PATH entries follow the default directories", "[plugin]")
{
    set_env("SLOPKIT_PLUGIN_PATH", "/extra/one:/extra/two");

    const auto directories = slopkit::plugin::PluginHost::default_search_directories("/prefix/bin");

    unset_env("SLOPKIT_PLUGIN_PATH");

    REQUIRE(directories.size() >= 4);
    REQUIRE(directories[0] == std::filesystem::path("/prefix/bin/plugins"));
    REQUIRE(directories[1] == std::filesystem::path("/prefix/bin") / SLOPKIT_PLUGIN_RELATIVE_DIR);
    REQUIRE(directories[2] == std::filesystem::path("/extra/one"));
    REQUIRE(directories[3] == std::filesystem::path("/extra/two"));
}

TEST_CASE("The version command prints the version", "[app]")
{
    std::ostringstream out;
    std::ostringstream err;

    REQUIRE(slopkit::app::print_version(out, err) == 0);
    REQUIRE(out.str().find(slopkit::version()) != std::string::npos);
}

TEST_CASE("Headless commands tolerate broken plugins", "[app]")
{
    set_env("SLOPKIT_PLUGIN_PATH", SLOPKIT_TEST_PLUGIN_DIR);

    std::ostringstream plugin_out;
    std::ostringstream plugin_err;
    REQUIRE(slopkit::app::list_plugins(plugin_out, plugin_err) == 0);
    REQUIRE(plugin_out.str().find("plugin(s) loaded") != std::string::npos);
    REQUIRE(plugin_err.str().find("skipped") != std::string::npos);

    std::ostringstream process_out;
    std::ostringstream process_err;
    REQUIRE(slopkit::app::list_processes(process_out, process_err) == 0);

    unset_env("SLOPKIT_PLUGIN_PATH");
}

TEST_CASE("Plugin discovery records a warning per rejected library", "[plugin]")
{
    LevelGuard level;
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::warning);

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_TEST_PLUGIN_DIR});

    REQUIRE_FALSE(host.diagnostics().empty());
    for (const auto& diagnostic : host.diagnostics())
    {
        bool recorded = false;
        for (const auto& record : records)
        {
            recorded = recorded
                    || (record.level == slopkit::log::Level::warning
                        && std::string_view {record.category} == slopkit::log::category::plugin
                        && record.message.find(diagnostic.path.string()) != std::string::npos);
        }
        REQUIRE(recorded);
    }
}

TEST_CASE("A plugin's host log messages carry its id", "[plugin]")
{
    LevelGuard level;
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::debug);

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_TEST_LOGGING_PLUGIN_DIR});

    auto* plugin = host.find("logging-fixture");
    REQUIRE(plugin != nullptr);

    // The entry-call record happens before the id is known, so it falls back to
    // the library file name and must still arrive under `plugin`.
    bool saw_fallback = false;
    for (const auto& record : records)
    {
        saw_fallback = saw_fallback
                    || (record.level == slopkit::log::Level::warning
                        && std::string_view {record.category} == slopkit::log::category::plugin
                        && record.message.find("loaded without a descriptor id yet") != std::string::npos);
    }
    REQUIRE(saw_fallback);

    auto session = plugin->open_session(1);
    REQUIRE(session.has_value());

    bool saw_attributed = false;
    for (const auto& record : records)
    {
        saw_attributed = saw_attributed
                      || (record.level == slopkit::log::Level::info
                          && std::string_view {record.category} == slopkit::log::category::plugin
                          && record.message.starts_with("logging-fixture: "));
    }
    REQUIRE(saw_attributed);
}

TEST_CASE("A plugin that leaves the allocation slots null reports unsupported", "[plugin]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_TEST_LOGGING_PLUGIN_DIR});

    auto* plugin = host.find("logging-fixture");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(1);
    REQUIRE(session.has_value());

    REQUIRE_FALSE(session->supports_allocation());

    auto allocated = session->allocate_memory(4096, 0);
    REQUIRE_FALSE(allocated.has_value());
    CHECK(allocated.error() == slopkit::process::AccessError::unsupported);

    auto freed = session->free_memory(0x1000);
    REQUIRE_FALSE(freed.has_value());
    CHECK(freed.error() == slopkit::process::AccessError::unsupported);
}
