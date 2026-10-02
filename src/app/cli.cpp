#include "app/cli.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>

#include <unistd.h>

#include "core/version.hpp"
#include "plugin/plugin_host.hpp"
#include "ui/app.hpp"

namespace slopkit::app
{

    namespace
    {
        std::filesystem::path self_executable_path()
        {
            std::array<char, 4096> buffer {};
            const auto             length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
            if (length <= 0)
            {
                return {};
            }
            return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
        }

        void report_diagnostics(std::ostream& err, const plugin::PluginHost& host)
        {
            for (const auto& diagnostic : host.diagnostics())
            {
                err << "skipped " << diagnostic.path.string() << ": " << diagnostic.message << '\n';
            }
        }
    } // namespace

    std::filesystem::path executable_directory()
    {
        auto path = self_executable_path();
        return path.has_parent_path() ? path.parent_path() : std::filesystem::path {"."};
    }

    std::vector<std::filesystem::path> plugin_search_directories()
    {
        return plugin::PluginHost::default_search_directories(executable_directory());
    }

    int print_version(std::ostream& out, std::ostream& /*err*/)
    {
        out << "slopkit " << version() << '\n';
        return 0;
    }

    int list_plugins(std::ostream& out, std::ostream& err)
    {
        plugin::PluginHost host;
        host.discover(plugin_search_directories());

        for (const auto& plugin : host.plugins())
        {
            out << plugin->id() << '\t' << plugin->version() << "\tprecedence " << plugin->precedence() << '\t'
                << process::describe(plugin->access_methods()) << '\n';
        }
        report_diagnostics(err, host);
        out << host.plugins().size() << " plugin(s) loaded, " << host.diagnostics().size() << " rejected\n";
        return 0;
    }

    int list_processes(std::ostream& out, std::ostream& err)
    {
        plugin::PluginHost host;
        host.discover(plugin_search_directories());

        for (const auto& process : host.list_processes())
        {
            out << process.pid << '\t' << process.plugin_id << '\t' << process.name;
            if (!process.exe_path.empty())
            {
                out << '\t' << process.exe_path;
            }
            out << '\n';
        }
        report_diagnostics(err, host);
        return 0;
    }

    int run(int argc, char** argv)
    {
        const std::vector<std::string_view> args(argv + 1, argv + argc);
        bool                                handled = false;

        for (const auto& arg : args)
        {
            if (arg == "--version" || arg == "-v")
            {
                handled = true;
                if (const int code = print_version(std::cout, std::cerr); code != 0)
                {
                    return code;
                }
            }
            else if (arg == "--list-plugins")
            {
                handled = true;
                if (const int code = list_plugins(std::cout, std::cerr); code != 0)
                {
                    return code;
                }
            }
            else if (arg == "--list-processes")
            {
                handled = true;
                if (const int code = list_processes(std::cout, std::cerr); code != 0)
                {
                    return code;
                }
            }
            else if (arg == "--help" || arg == "-h")
            {
                handled = true;
                std::cout << "usage: slopkit [--version] [--list-plugins] [--list-processes]\n"
                             "  no flags launches the GUI\n";
            }
            else
            {
                std::cerr << "unknown option: " << arg << '\n';
                return 2;
            }
        }

        if (handled)
        {
            return 0;
        }

        ui::App app;
        return app.run();
    }

} // namespace slopkit::app
