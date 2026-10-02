#include <catch2/catch.hpp>

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

#include "app/cli.hpp"
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

TEST_CASE("A missing plugin directory is not fatal", "[plugin]")
{
    slopkit::plugin::PluginHost host;
    host.discover({"/nonexistent/slopkit/plugins"});

    REQUIRE(host.plugins().empty());
    REQUIRE(host.diagnostics().empty());
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

TEST_CASE("A missing installed plugin directory is silent", "[plugin]")
{
    const auto directories = slopkit::plugin::PluginHost::default_search_directories("/nonexistent/slopkit/bin");

    slopkit::plugin::PluginHost host;
    host.discover(directories);

    REQUIRE(host.plugins().empty());
    REQUIRE(host.diagnostics().empty());
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
