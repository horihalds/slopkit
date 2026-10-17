#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "scan/types.hpp"
#include "scan/value.hpp"
#include "trainer/payload.hpp"
#include "trainer/runtime/console.hpp"
#include "trainer/runtime/payload_reader.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/scripts.hpp"
#include "trainer/runtime/target.hpp"
#include "trainer/runtime/tui.hpp"

namespace slopkit::trainer
{
    namespace
    {
        struct Options
        {
            std::uint32_t                                    pid {};
            bool                                             has_pid {false};
            std::string                                      name;
            std::uint32_t                                    interval_ms {250};
            std::uint32_t                                    wait_ms {0};
            bool                                             list {false};
            std::vector<std::size_t>                         freeze;
            std::vector<std::pair<std::size_t, std::string>> set_values;
            std::vector<std::size_t>                         activate;
            bool                                             help {false};

            [[nodiscard]] bool has_actions() const
            {
                return list || wait_ms > 0 || !freeze.empty() || !set_values.empty();
            }
        };

        std::optional<std::uint32_t> parse_number(std::string_view text)
        {
            std::uint32_t value  = 0;
            const auto    parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size())
            {
                return std::nullopt;
            }
            return value;
        }

        std::expected<std::string_view, std::string>
        next_value(int& index, int argc, char** argv, std::string_view flag)
        {
            if (index + 1 >= argc)
            {
                return std::unexpected(std::format("{} needs a value", flag));
            }
            ++index;
            return std::string_view {argv[index]};
        }

        std::expected<std::uint32_t, std::string> number_value(int& index, int argc, char** argv, std::string_view flag)
        {
            const auto text = next_value(index, argc, argv, flag);
            if (!text)
            {
                return std::unexpected(text.error());
            }
            const auto number = parse_number(*text);
            if (!number || *number == 0)
            {
                return std::unexpected(std::format("{} needs a positive number, got \"{}\"", flag, *text));
            }
            return *number;
        }

        std::expected<Options, std::string> parse_arguments(int argc, char** argv)
        {
            Options options;
            for (int index = 1; index < argc; ++index)
            {
                const std::string_view argument {argv[index]};
                if (argument == "--help" || argument == "-h")
                {
                    options.help = true;
                    continue;
                }
                if (argument == "--list")
                {
                    options.list = true;
                    continue;
                }
                if (argument == "--pid")
                {
                    const auto number = number_value(index, argc, argv, argument);
                    if (!number)
                    {
                        return std::unexpected(number.error());
                    }
                    options.pid     = *number;
                    options.has_pid = true;
                    continue;
                }
                if (argument == "--name")
                {
                    const auto value = next_value(index, argc, argv, argument);
                    if (!value)
                    {
                        return std::unexpected(value.error());
                    }
                    options.name = std::string {*value};
                    continue;
                }
                if (argument == "--interval")
                {
                    const auto number = number_value(index, argc, argv, argument);
                    if (!number)
                    {
                        return std::unexpected(number.error());
                    }
                    options.interval_ms = *number;
                    continue;
                }
                if (argument == "--wait")
                {
                    const auto text = next_value(index, argc, argv, argument);
                    if (!text)
                    {
                        return std::unexpected(text.error());
                    }
                    const auto number = parse_number(*text);
                    if (!number)
                    {
                        return std::unexpected(std::format("--wait needs a number, got \"{}\"", *text));
                    }
                    options.wait_ms = *number;
                    continue;
                }
                if (argument == "--freeze")
                {
                    const auto number = number_value(index, argc, argv, argument);
                    if (!number)
                    {
                        return std::unexpected(number.error());
                    }
                    options.freeze.push_back(*number);
                    continue;
                }
                if (argument == "--set")
                {
                    const auto value = next_value(index, argc, argv, argument);
                    if (!value)
                    {
                        return std::unexpected(value.error());
                    }
                    const auto equals = value->find('=');
                    if (equals == std::string_view::npos)
                    {
                        return std::unexpected(std::format("--set needs N=VALUE, got \"{}\"", *value));
                    }
                    const auto number = parse_number(value->substr(0, equals));
                    if (!number || *number == 0)
                    {
                        return std::unexpected(std::format("--set needs N=VALUE, got \"{}\"", *value));
                    }
                    options.set_values.emplace_back(*number, std::string {value->substr(equals + 1)});
                    continue;
                }
                if (argument == "--activate")
                {
                    const auto number = number_value(index, argc, argv, argument);
                    if (!number)
                    {
                        return std::unexpected(number.error());
                    }
                    options.activate.push_back(*number);
                    continue;
                }
                return std::unexpected(std::format("unknown option \"{}\"", argument));
            }
            return options;
        }

        std::vector<LiveEntry> seed(const Payload& payload)
        {
            std::vector<LiveEntry> live;
            live.reserve(payload.entries.size());
            for (const auto& entry : payload.entries)
            {
                LiveEntry state;
                state.address = entry.last_address;
                state.frozen  = entry.frozen;
                live.push_back(std::move(state));
            }
            return live;
        }

        void refresh_all(const Payload& payload, Resolver& resolver, std::vector<LiveEntry>& live)
        {
            for (std::size_t position = 0; position < payload.entries.size(); ++position)
            {
                resolver.refresh(payload.entries[position], live[position]);
            }
        }

        void rewrite_frozen(const Payload& payload, Resolver& resolver, std::vector<LiveEntry>& live)
        {
            for (std::size_t position = 0; position < payload.entries.size(); ++position)
            {
                if (!live[position].frozen || live[position].bytes.empty())
                {
                    continue;
                }
                static_cast<void>(resolver.write(payload.entries[position], live[position], live[position].bytes));
            }
        }

        // Ticks the scripts and hands the symbols they published to the resolver, so
        // an entry whose expression names one resolves in the same refresh.
        void tick_scripts(ScriptHost& scripts, Resolver& resolver)
        {
            scripts.tick(std::chrono::steady_clock::now());
            resolver.set_symbols(scripts.symbols());
        }

        // Activates or deactivates one script and prints whatever its hook said.
        // A failing hook never leaves the console or changes the exit code.
        void run_script_command(ScriptHost& scripts, Resolver& resolver, bool activating, std::size_t index)
        {
            const auto        outcome = activating ? scripts.activate(index - 1) : scripts.deactivate(index - 1);
            std::ostream&     stream  = outcome.has_value() ? std::cout : std::cerr;
            const std::string text    = scripts.output_text(index - 1);
            if (!text.empty())
            {
                stream << text;
            }
            else if (!outcome.has_value())
            {
                stream << std::format("{}\n", outcome.error());
            }
            resolver.set_symbols(scripts.symbols());
        }

        std::optional<std::string>
        apply_set(Resolver& resolver, const PayloadEntry& entry, LiveEntry& live, std::string_view text)
        {
            const auto value = scan::parse_value(entry.type, text, entry.hex);
            if (!value)
            {
                return std::format(
                    "cannot read \"{}\" as {}: {}", text, scan::describe(entry.type), value.error().message);
            }

            const auto bytes = scan::encode_value(entry.type, *value);
            if (const auto written = resolver.write(entry, live, bytes); !written)
            {
                return written.error();
            }

            live.bytes = bytes;
            live.note.clear();
            return std::nullopt;
        }

        void pump(std::uint32_t           wait_ms,
                  std::uint32_t           interval_ms,
                  const Payload&          payload,
                  ScriptHost&             scripts,
                  Resolver&               resolver,
                  std::vector<LiveEntry>& live)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds {wait_ms};
            while (std::chrono::steady_clock::now() < deadline)
            {
                tick_scripts(scripts, resolver);
                rewrite_frozen(payload, resolver, live);
                refresh_all(payload, resolver, live);

                const auto left =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                        .count();
                const auto step =
                    std::min<std::uint32_t>(interval_ms, static_cast<std::uint32_t>(std::max<std::int64_t>(left, 0)));
                std::this_thread::sleep_for(std::chrono::milliseconds {step});
            }
        }

        // One interactive action, applied to the session; the returned text is the
        // message the list shows. Everything it touches stays on the loop's own
        // thread, so the target is never reached from a second one.
        std::string apply_action(const tui::Action&      action,
                                 const Payload&          payload,
                                 ScriptHost&             scripts,
                                 Resolver&               resolver,
                                 std::vector<LiveEntry>& live)
        {
            const auto entry_error = [&payload, &action]
            {
                return std::format(
                    "this trainer carries {} entries, so there is no entry {}", payload.entries.size(), action.index);
            };
            const auto script_error = [&scripts, &action]
            {
                return std::format("this trainer carries {} script{}, so there is no script {}",
                                   scripts.size(),
                                   scripts.size() == 1 ? "" : "s",
                                   action.index);
            };

            switch (action.kind)
            {
            case tui::Action::Kind::none:
            case tui::Action::Kind::quit:
                return {};
            case tui::Action::Kind::refresh:
                resolver.refresh_modules();
                tick_scripts(scripts, resolver);
                rewrite_frozen(payload, resolver, live);
                refresh_all(payload, resolver, live);
                return {};
            case tui::Action::Kind::freeze:
            {
                if (action.index == 0 || action.index > payload.entries.size())
                {
                    return entry_error();
                }
                auto& state  = live[action.index - 1];
                state.frozen = !state.frozen;
                if (state.frozen && !state.bytes.empty())
                {
                    if (const auto written = resolver.write(payload.entries[action.index - 1], state, state.bytes);
                        !written)
                    {
                        return written.error();
                    }
                }
                return {};
            }
            case tui::Action::Kind::set_value:
            {
                if (action.index == 0 || action.index > payload.entries.size())
                {
                    return entry_error();
                }
                return apply_set(resolver, payload.entries[action.index - 1], live[action.index - 1], action.value)
                    .value_or(std::string {});
            }
            case tui::Action::Kind::activate_script:
            case tui::Action::Kind::deactivate_script:
            {
                if (action.index == 0 || action.index > scripts.size())
                {
                    return script_error();
                }

                const auto outcome = action.kind == tui::Action::Kind::activate_script
                                       ? scripts.activate(action.index - 1)
                                       : scripts.deactivate(action.index - 1);
                resolver.set_symbols(scripts.symbols());

                // The hook's own output is the interesting part; a failure adds why.
                auto message = scripts.output_text(action.index - 1);
                if (!outcome.has_value())
                {
                    if (!message.empty())
                    {
                        message += " | ";
                    }
                    message += outcome.error();
                }
                return message;
            }
            }
            return {};
        }

        int interactive(const Payload&          payload,
                        ScriptHost&             scripts,
                        Resolver&               resolver,
                        std::vector<LiveEntry>& live,
                        std::uint32_t           pid,
                        std::uint32_t           interval_ms)
        {
            const auto perform = [&](const tui::Action& action)
            {
                return apply_action(action, payload, scripts, resolver, live);
            };
            const auto step = [&]
            {
                tick_scripts(scripts, resolver);
                rewrite_frozen(payload, resolver, live);
                refresh_all(payload, resolver, live);
            };

            return tui::run_tui(payload, live, scripts.states(), pid, interval_ms, perform, step);
        }

        int run(const Options& options)
        {
            const auto payload = load_own_payload();
            if (!payload)
            {
                std::cerr << "slopkit-trainer: " << payload.error() << "\n";
                return kExitNoPayload;
            }
            if (payload->entries.empty() && payload->scripts.empty())
            {
                std::cerr << "slopkit-trainer: this trainer carries no entries and no scripts\n";
                return kExitNoPayload;
            }

            AttachRequest request;
            if (options.has_pid)
            {
                request.pid = options.pid;
            }
            request.name =
                request.pid != 0 ? std::string {} : (options.name.empty() ? payload->process_name : options.name);
            if (request.pid == 0 && request.name.empty())
            {
                std::cerr << "slopkit-trainer: no process name was recorded; pass --pid or --name\n";
                return kExitUsage;
            }

            const auto target = attach(request);
            if (!target)
            {
                std::cerr << "slopkit-trainer: " << target.error().message << "\n";
                return target.error().not_found ? kExitNoProcess : kExitAccess;
            }

            Resolver               resolver {**target, payload->pointer_size};
            std::vector<LiveEntry> live = seed(*payload);
            ScriptHost             scripts {**target, *payload, options.interval_ms};

            if (!scripts.class_error().empty())
            {
                // `structs` is the only thing a class project backs, so a project
                // that cannot be read is reported once and nothing else changes.
                std::cerr << std::format("slopkit-trainer: script structs are unavailable: {}\n",
                                         scripts.class_error());
            }

            // `--activate N` ticks script S<N> exactly as `a<N>` does interactively,
            // before the requested actions run.
            for (const auto index : options.activate)
            {
                if (index == 0 || index > scripts.size())
                {
                    std::cerr << std::format(
                        "slopkit-trainer: this trainer carries {} script{}, so there is no script {}\n",
                        scripts.size(),
                        scripts.size() == 1 ? "" : "s",
                        index);
                    return kExitUsage;
                }
                run_script_command(scripts, resolver, true, index);
            }
            resolver.set_symbols(scripts.symbols());

            refresh_all(*payload, resolver, live);

            for (const auto index : options.freeze)
            {
                if (index == 0 || index > live.size())
                {
                    std::cerr << std::format(
                        "slopkit-trainer: this trainer carries {} entries, so there is no entry {}\n",
                        live.size(),
                        index);
                    return kExitUsage;
                }
                live[index - 1].frozen = true;
            }
            rewrite_frozen(*payload, resolver, live);

            for (const auto& [index, text] : options.set_values)
            {
                if (index == 0 || index > live.size())
                {
                    std::cerr << std::format(
                        "slopkit-trainer: this trainer carries {} entries, so there is no entry {}\n",
                        live.size(),
                        index);
                    return kExitUsage;
                }
                if (const auto message = apply_set(resolver, payload->entries[index - 1], live[index - 1], text))
                {
                    std::cerr << "slopkit-trainer: " << *message << "\n";
                    return kExitAccess;
                }
            }

            if (options.wait_ms > 0)
            {
                pump(options.wait_ms, options.interval_ms, *payload, scripts, resolver, live);
            }

            if (options.has_actions() || !stdin_is_terminal())
            {
                std::cout << console::render_header(*payload, (*target)->pid())
                          << console::render_rows(*payload, live, scripts.states());
                return kExitOk;
            }

            return interactive(*payload, scripts, resolver, live, (*target)->pid(), options.interval_ms);
        }
    }

    int run(int argc, char** argv)
    {
        const auto options = parse_arguments(argc, argv);
        if (!options)
        {
            std::cerr << "slopkit-trainer: " << options.error() << "\n\n" << console::render_usage();
            return kExitUsage;
        }
        if (options->help)
        {
            std::cout << console::render_usage();
            return kExitOk;
        }
        return run(*options);
    }

}

int main(int argc, char** argv)
{
    return slopkit::trainer::run(argc, argv);
}
