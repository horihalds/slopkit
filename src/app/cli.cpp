#include "app/cli.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <unistd.h>

#include <QString>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "core/version.hpp"
#include "plugin/plugin_host.hpp"
#include "process/plugin_access.hpp"
#include "scan/engine.hpp"
#include "scan/source.hpp"
#include "scan/value.hpp"
#include "ui/app.hpp"

namespace slopkit::app
{

    namespace
    {
        // One mebibyte, after which the file sink keeps a single rotated backup.
        constexpr std::size_t kLogFileLimit = 1024 * 1024;

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
                log::warning(log::category::plugin,
                             std::format("skipped {}: {}", diagnostic.path.string(), diagnostic.message));
            }
        }

        // Infers the value type from the literal: a real when it has a fractional
        // part or an exponent, a wide integer for long hex literals, otherwise the
        // narrowest integer that fits.
        scan::ValueType infer_value_type(std::string_view text)
        {
            std::string_view body = text;
            if (!body.empty() && (body.front() == '-' || body.front() == '+'))
            {
                body.remove_prefix(1);
            }
            if (body.size() > 1 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
            {
                const auto        digits = body.substr(2);
                const auto        first  = digits.find_first_not_of('0');
                const std::size_t count  = first == std::string_view::npos ? 1 : digits.size() - first;
                return count > 8 ? scan::ValueType::int64 : scan::ValueType::int32;
            }
            if (body.find_first_of(".eE") != std::string_view::npos)
            {
                return scan::ValueType::float64;
            }
            if (scan::parse_value(scan::ValueType::int32, text, false).has_value())
            {
                return scan::ValueType::int32;
            }
            return scan::ValueType::int64;
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
        log::info(log::category::app, "command print-version: start");
        out << "slopkit " << version() << '\n';
        log::info(log::category::app, std::format("command print-version: done ({})", version()));
        return 0;
    }

    int list_plugins(std::ostream& out, std::ostream& err)
    {
        log::info(log::category::app, "command list-plugins: start");

        plugin::PluginHost host;
        host.discover(plugin_search_directories());

        for (const auto& plugin : host.plugins())
        {
            out << plugin->id() << '\t' << plugin->version() << "\tprecedence " << plugin->precedence() << '\t'
                << process::describe(plugin->access_methods()) << '\n';
        }
        report_diagnostics(err, host);
        out << host.plugins().size() << " plugin(s) loaded, " << host.diagnostics().size() << " rejected\n";
        log::info(log::category::app,
                  std::format("command list-plugins: done, {} loaded, {} rejected",
                              host.plugins().size(),
                              host.diagnostics().size()));
        return 0;
    }

    int list_processes(std::ostream& out, std::ostream& err)
    {
        log::info(log::category::app, "command list-processes: start");

        plugin::PluginHost host;
        host.discover(plugin_search_directories());

        const auto processes = host.list_processes();
        for (const auto& process : processes)
        {
            out << process.pid << '\t' << process.plugin_id << '\t' << process.name;
            if (!process.exe_path.empty())
            {
                out << '\t' << process.exe_path;
            }
            out << '\n';
        }
        report_diagnostics(err, host);
        log::info(log::category::app,
                  std::format("command list-processes: done, {} process(es), {} rejected",
                              processes.size(),
                              host.diagnostics().size()));
        return 0;
    }

    int scan_process(std::ostream& out, std::ostream& err, std::uint32_t pid, std::string_view value_text)
    {
        log::info(log::category::app, std::format("command scan: start pid {} value {}", pid, value_text));

        plugin::PluginHost host;
        host.discover(plugin_search_directories());
        report_diagnostics(err, host);

        process::PluginAccess access(host);

        const auto processes = access.list_processes();
        if (!processes)
        {
            err << "could not list processes: " << process::describe(processes.error()) << '\n';
            log::warning(
                log::category::app,
                std::format("command scan: could not list processes: {}", process::describe(processes.error())));
            return 1;
        }
        std::string plugin_id;
        for (const auto& process : *processes)
        {
            if (process.pid == pid)
            {
                plugin_id = process.plugin_id;
                break;
            }
        }
        if (plugin_id.empty())
        {
            err << "no loaded plugin claims pid " << pid << '\n';
            log::warning(log::category::app, std::format("command scan: no loaded plugin claims pid {}", pid));
            return 1;
        }

        auto session = access.attach(pid, plugin_id);
        if (!session)
        {
            err << "attach failed: " << process::describe(session.error()) << '\n';
            log::warning(log::category::app,
                         std::format("command scan: attach failed: {}", process::describe(session.error())));
            return 1;
        }

        const scan::ValueType value_type = infer_value_type(value_text);
        auto                  value      = scan::parse_value(value_type, value_text, false);
        if (!value)
        {
            err << "invalid value: " << value.error().message << '\n';
            log::warning(log::category::app, std::format("command scan: invalid value: {}", value.error().message));
            return 2;
        }

        scan::ScanConfig config;
        config.type                 = scan::ScanType::exact_value;
        config.value_type           = value_type;
        config.value                = std::move(*value);
        config.filter.writable      = true;
        config.filter.copy_on_write = true;

        scan::ScanEngine engine;
        engine.first_scan(config, scan::make_session_source(*session));
        while (engine.is_running())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        const scan::ScanSnapshot snapshot = engine.snapshot();
        if (snapshot.state != scan::ScanState::done)
        {
            err << "scan failed: " << snapshot.message << '\n';
            log::warning(log::category::app, std::format("command scan: failed: {}", snapshot.message));
            return 1;
        }
        if (snapshot.hits.empty())
        {
            out << "no matches\n";
            log::info(log::category::app, std::format("command scan: done, no matches for pid {}", pid));
            return 0;
        }

        for (const auto& hit : snapshot.hits)
        {
            out << std::format("{:X}\t{}\t{}\n",
                               hit.address,
                               scan::describe(value_type),
                               scan::format_value(value_type, hit.value, false));
        }
        out << snapshot.hit_count << " hit(s)\n";
        log::info(log::category::app, std::format("command scan: done, {} hit(s) for pid {}", snapshot.hit_count, pid));
        return 0;
    }

    int run(int argc, char** argv)
    {
        const std::vector<std::string_view> args(argv + 1, argv + argc);

        // The minimum level is a global flag: parse it before anything logs, so a
        // bad value is a usage error and never reaches the command dispatch.
        log::Level log_level = log::Level::info;
        for (std::size_t index = 0; index < args.size(); ++index)
        {
            const auto       arg = args[index];
            std::string_view value;
            if (arg == "--log-level")
            {
                if (index + 1 >= args.size())
                {
                    std::cerr << "--log-level needs a level\n";
                    return 2;
                }
                value = args[++index];
            }
            else if (arg.starts_with("--log-level="))
            {
                value = arg.substr(std::string_view("--log-level=").size());
            }
            else
            {
                continue;
            }

            if (!log::parse_level(value, log_level))
            {
                std::cerr << "unknown log level: " << value << '\n';
                return 2;
            }
        }

        auto& logger = log::Logger::instance();
        logger.set_minimum_level(log_level);
        logger.add_sink(log::stderr_sink());

        const auto log_path = log::default_log_path();
        logger.add_sink(log::rolling_file_sink(log_path, kLogFileLimit));

        log::info(log::category::app,
                  std::format("slopkit {} starting, level {}, sinks: stderr + {}",
                              version(),
                              log::level_name(log_level),
                              log_path.string()));

        bool    handled = false;
        QString table_path;

        for (std::size_t index = 0; index < args.size(); ++index)
        {
            const auto arg = args[index];
            if (arg == "--log-level")
            {
                ++index; // Consumed by the pre-pass above.
            }
            else if (arg.starts_with("--log-level="))
            {
                // Consumed by the pre-pass above.
            }
            else if (arg == "--version" || arg == "-v")
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
            else if (arg == "--scan")
            {
                handled = true;
                if (index + 2 >= args.size())
                {
                    std::cerr << "--scan needs a pid and a value\n";
                    return 2;
                }
                const auto pid_text   = args[++index];
                const auto value_text = args[++index];

                std::uint32_t pid       = 0;
                const auto [end, error] = std::from_chars(pid_text.data(), pid_text.data() + pid_text.size(), pid);
                if (error != std::errc {} || end != pid_text.data() + pid_text.size())
                {
                    std::cerr << "invalid pid: " << pid_text << '\n';
                    return 2;
                }
                if (const int code = scan_process(std::cout, std::cerr, pid, value_text); code != 0)
                {
                    return code;
                }
            }
            else if (arg == "--help" || arg == "-h")
            {
                handled = true;
                std::cout << "usage: slopkit [--log-level <level>] [--version] [--list-plugins]\n"
                             "               [--list-processes] [--scan <pid> <value>] [--help] [<table.skt>]\n"
                             "  --log-level sets the minimum level: debug|info|warning|error (default info)\n"
                             "  --scan runs one exact-value scan and prints address<TAB>type<TAB>value\n"
                             "  <table.skt> opens that address table; no flags launches the GUI\n";
            }
            else if (arg.starts_with('-'))
            {
                std::cerr << "unknown option: " << arg << '\n';
                return 2;
            }
            else if (table_path.isEmpty())
            {
                table_path = QString::fromUtf8(arg.data(), static_cast<qsizetype>(arg.size()));
            }
            else
            {
                std::cerr << "too many table arguments: " << arg << '\n';
                return 2;
            }
        }

        if (handled)
        {
            return 0;
        }

        if (!table_path.isEmpty())
        {
            log::info(log::category::app, std::format("opening table {}", table_path.toStdString()));
        }

        ui::App app;
        return app.run(argc, argv, table_path);
    }

} // namespace slopkit::app
