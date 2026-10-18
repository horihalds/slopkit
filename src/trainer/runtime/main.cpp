#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"
#include "trainer/payload.hpp"
#include "trainer/runtime/console.hpp"
#include "trainer/runtime/gui/window.hpp"
#include "trainer/runtime/logging.hpp"
#include "trainer/runtime/payload_reader.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/rows.hpp"
#include "trainer/runtime/scripts.hpp"
#include "trainer/runtime/target.hpp"

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
            bool                                             verbose {false};
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
                if (argument == "--verbose")
                {
                    options.verbose = true;
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
            if (const auto refused = parse_value_text(entry, text))
            {
                return refused;
            }

            const auto bytes = scan::encode_value(entry.type, scan::parse_value(entry.type, text, entry.hex).value());
            if (const auto written = resolver.write(entry, live, bytes); !written)
            {
                return written.error();
            }

            live.bytes = bytes;
            live.note.clear();
            return std::nullopt;
        }

        // A stand-in bound to the resolver and script host while no target is
        // attached. Every access fails cleanly, so the rows show as unresolved
        // instead of the trainer holding on to a dead handle.
        class DetachedTarget final : public Target
        {
        public:
            explicit DetachedTarget(std::uint32_t pointer_size) : pointer_size_ {pointer_size} {}

            [[nodiscard]] std::uint32_t pid() const override
            {
                return 0;
            }

            [[nodiscard]] std::uint32_t pointer_size() const override
            {
                return pointer_size_;
            }

            [[nodiscard]] bool alive() const override
            {
                return false;
            }

            [[nodiscard]] const std::vector<Module>& modules() const override
            {
                return modules_;
            }

            [[nodiscard]] const std::vector<Region>& regions() const override
            {
                return regions_;
            }

            [[nodiscard]] std::expected<std::vector<std::byte>, std::string> read(std::uint64_t,
                                                                                  std::size_t) const override
            {
                return std::unexpected(std::string {"the target is not attached"});
            }

            [[nodiscard]] std::expected<void, std::string> write(std::uint64_t,
                                                                 std::span<const std::byte>) const override
            {
                return std::unexpected(std::string {"the target is not attached"});
            }

        private:
            std::uint32_t       pointer_size_ {8};
            std::vector<Module> modules_;
            std::vector<Region> regions_;
        };

        // Owns the target handle and the reconnect policy. A trainer follows the
        // process it was built for: it may start before that process does, and
        // when the process exits its scripts are switched off and a re-attach is
        // tried until it runs again, restoring the scripts and frozen entries.
        class Session
        {
        public:
            Session(std::unique_ptr<Target> target,
                    const Target&           detached,
                    AttachRequest           request,
                    const Payload&          payload,
                    ScriptHost&             scripts,
                    Resolver&               resolver,
                    std::vector<LiveEntry>& live)
                : target_ {std::move(target)}, detached_ {detached}, request_ {std::move(request)}, payload_ {payload},
                  scripts_ {scripts}, resolver_ {resolver}, live_ {live}
            {
                ever_attached_ = target_ != nullptr;
            }

            [[nodiscard]] bool attached() const noexcept
            {
                return target_ != nullptr;
            }

            [[nodiscard]] bool ever_attached() const noexcept
            {
                return ever_attached_;
            }

            [[nodiscard]] std::uint32_t pid() const noexcept
            {
                return target_ ? target_->pid() : 0;
            }

            // Startup actions that need a target are replayed by the first
            // successful attach instead of failing the run.
            void defer_activation(std::size_t index)
            {
                pending_activation_.push_back(index);
            }

            void defer_set(std::size_t index, std::string text)
            {
                pending_set_.emplace_back(index, std::move(text));
            }

            // True when the target is live. When it is absent, tries an attach at
            // most once a second and restores the state when it succeeds.
            bool ensure()
            {
                if (target_ && target_->alive())
                {
                    return true;
                }

                if (target_)
                {
                    log::warning(log::category::process,
                                 std::format("target pid {} exited; scripts deactivated", target_->pid()));
                    active_.clear();
                    const auto states = scripts_.states();
                    for (std::size_t index = 0; index < states.size(); ++index)
                    {
                        if (states[index].active)
                        {
                            active_.push_back(index);
                        }
                    }
                    scripts_.disable_all();
                    target_.reset();
                    resolver_.rebind(detached_);
                    scripts_.rebind(detached_);
                }

                const auto now = std::chrono::steady_clock::now();
                if (now < next_attempt_)
                {
                    return false;
                }
                next_attempt_ = now + std::chrono::seconds {1};

                AttachRequest request = request_;
                if (request.pid != 0 && request.name.empty() && !payload_.process_name.empty())
                {
                    // A recording names the process it was made against; that is
                    // what re-attaching has to look for once the pid is gone.
                    request.pid  = 0;
                    request.name = payload_.process_name;
                }

                auto attached = attach(request);
                if (!attached)
                {
                    log::debug(log::category::process, std::format("re-attach failed: {}", attached.error().message));
                    return false;
                }

                const bool reattached = ever_attached_;
                target_               = std::move(*attached);
                ever_attached_        = true;
                log::info(log::category::process,
                          reattached ? std::format("re-attached to pid {}; restoring scripts", target_->pid())
                                     : std::format("attached to pid {}; restoring scripts", target_->pid()));

                resolver_.rebind(*target_);
                scripts_.rebind(*target_);

                refresh_all(payload_, resolver_, live_);
                rewrite_frozen(payload_, resolver_, live_);

                for (const std::size_t index : active_)
                {
                    run_script_command(scripts_, resolver_, true, index + 1);
                }
                active_.clear();
                for (const std::size_t index : pending_activation_)
                {
                    run_script_command(scripts_, resolver_, true, index + 1);
                }
                pending_activation_.clear();
                resolver_.set_symbols(scripts_.symbols());
                refresh_all(payload_, resolver_, live_);

                for (const auto& [index, text] : pending_set_)
                {
                    if (const auto message = apply_set(resolver_, payload_.entries[index], live_[index], text))
                    {
                        log::warning(log::category::app,
                                     std::format("deferred --set of entry {} failed: {}", index + 1, *message));
                    }
                }
                pending_set_.clear();
                return true;
            }

        private:
            std::unique_ptr<Target>                          target_;
            const Target&                                    detached_;
            AttachRequest                                    request_;
            const Payload&                                   payload_;
            ScriptHost&                                      scripts_;
            Resolver&                                        resolver_;
            std::vector<LiveEntry>&                          live_;
            std::vector<std::size_t>                         active_;
            std::vector<std::size_t>                         pending_activation_;
            std::vector<std::pair<std::size_t, std::string>> pending_set_;
            std::chrono::steady_clock::time_point            next_attempt_ {};
            bool                                             ever_attached_ {false};
        };

        void pump(std::uint32_t           wait_ms,
                  std::uint32_t           interval_ms,
                  const Payload&          payload,
                  Session&                session,
                  ScriptHost&             scripts,
                  Resolver&               resolver,
                  std::vector<LiveEntry>& live)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds {wait_ms};
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (session.ensure())
                {
                    tick_scripts(scripts, resolver);
                    rewrite_frozen(payload, resolver, live);
                    refresh_all(payload, resolver, live);
                }

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
        std::string apply_action(const Action&           action,
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
            case ActionKind::none:
            case ActionKind::quit:
                return {};
            case ActionKind::refresh:
                resolver.refresh_modules();
                tick_scripts(scripts, resolver);
                rewrite_frozen(payload, resolver, live);
                refresh_all(payload, resolver, live);
                return {};
            case ActionKind::toggle_freeze:
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
            case ActionKind::set_value:
            {
                if (action.index == 0 || action.index > payload.entries.size())
                {
                    return entry_error();
                }
                return apply_set(resolver, payload.entries[action.index - 1], live[action.index - 1], action.text)
                    .value_or(std::string {});
            }
            case ActionKind::activate_script:
            case ActionKind::deactivate_script:
            {
                if (action.index == 0 || action.index > scripts.size())
                {
                    return script_error();
                }

                const auto outcome = action.kind == ActionKind::activate_script ? scripts.activate(action.index - 1)
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
                        Session&                session,
                        ScriptHost&             scripts,
                        Resolver&               resolver,
                        std::vector<LiveEntry>& live,
                        std::uint32_t           interval_ms)
        {
            const auto perform = [&](const Action& action)
            {
                if (!session.ensure())
                {
                    return std::string {session.ever_attached() ? "the target has exited; waiting for it to reappear"
                                                                : "waiting for the target to appear"};
                }
                return apply_action(action, payload, scripts, resolver, live);
            };
            const auto step = [&]
            {
                if (!session.ensure())
                {
                    return;
                }
                tick_scripts(scripts, resolver);
                rewrite_frozen(payload, resolver, live);
                refresh_all(payload, resolver, live);
            };

            gui::Options options;
            options.title = payload.process_name.empty()
                              ? QStringLiteral("slopkit-trainer")
                              : QStringLiteral("slopkit-trainer: %1").arg(QString::fromStdString(payload.process_name));
            options.interval_ms = interval_ms;

            return gui::run(
                payload,
                live,
                scripts.states(),
                [&session]
                {
                    return session.pid();
                },
                std::move(options),
                perform,
                step);
        }

        // `--verbose` is the only way a trainer writes a log, so start it before
        // anything else can report why it stops; an executable that cannot be
        // located fails the payload read right after and is reported there.
        void start_verbose_logging()
        {
            const auto executable = own_executable_path();
            if (executable.empty())
            {
                std::cerr << "slopkit-trainer: --verbose could not locate this trainer's own executable, so no log "
                             "is written\n";
                return;
            }
            std::cerr << std::format("slopkit-trainer: verbose logging to {}\n",
                                     enable_verbose_logging(executable).string());
        }

        int run(const Options& options)
        {
            if (options.verbose)
            {
                start_verbose_logging();
            }

            const auto payload = load_own_payload();
            if (!payload)
            {
                log::error(log::category::app, std::format("cannot read the payload: {}", payload.error()));
                std::cerr << "slopkit-trainer: " << payload.error() << "\n";
                return kExitNoPayload;
            }
            log::info(log::category::app,
                      std::format("payload: {} value entr(ies), {} script(s), {} class(es)",
                                  payload->entries.size(),
                                  payload->scripts.size(),
                                  payload->classes.size()));
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

            log::info(log::category::process,
                      request.pid != 0 ? std::format("attaching to pid {}", request.pid)
                                       : std::format("attaching to \"{}\"", request.name));
            auto        target = attach(request);
            std::string attach_failure;
            if (!target)
            {
                attach_failure = target.error().message;
            }
            const bool interactive_session = !options.has_actions() && gui::window_available();
            const bool wait_for_target =
                !target && target.error().not_found && (options.wait_ms > 0 || interactive_session);
            if (!target && !wait_for_target)
            {
                log::error(log::category::process, attach_failure);
                std::cerr << "slopkit-trainer: " << attach_failure << "\n";
                return target.error().not_found ? kExitNoProcess : kExitAccess;
            }
            if (target)
            {
                log::info(log::category::process,
                          std::format("attached to pid {}: {}-byte pointers, {} module(s), {} region(s)",
                                      (*target)->pid(),
                                      (*target)->pointer_size(),
                                      (*target)->modules().size(),
                                      (*target)->regions().size()));
            }
            else
            {
                log::info(log::category::process,
                          std::format("target not found ({}); waiting for it to appear", attach_failure));
                std::cerr << "slopkit-trainer: " << attach_failure << "; waiting for it to appear\n";
            }

            DetachedTarget          detached {payload->pointer_size};
            const Target&           bound = target ? **target : detached;
            Resolver                resolver {bound, payload->pointer_size};
            std::vector<LiveEntry>  live = seed(*payload);
            ScriptHost              scripts {bound, *payload, options.interval_ms};
            std::unique_ptr<Target> handle;
            if (target)
            {
                handle = std::move(*target);
            }
            Session session {std::move(handle), detached, request, *payload, scripts, resolver, live};

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
                if (session.attached())
                {
                    run_script_command(scripts, resolver, true, index);
                }
                else
                {
                    session.defer_activation(index - 1);
                }
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
            if (session.attached())
            {
                rewrite_frozen(*payload, resolver, live);
            }

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
                if (!session.attached())
                {
                    session.defer_set(index - 1, text);
                    continue;
                }
                if (const auto message = apply_set(resolver, payload->entries[index - 1], live[index - 1], text))
                {
                    std::cerr << "slopkit-trainer: " << *message << "\n";
                    return kExitAccess;
                }
            }

            if (options.wait_ms > 0)
            {
                pump(options.wait_ms, options.interval_ms, *payload, session, scripts, resolver, live);
            }

            if (!interactive_session)
            {
                if (!session.attached())
                {
                    std::cerr << "slopkit-trainer: "
                              << (attach_failure.empty() ? "the target is not running" : attach_failure) << "\n";
                    return kExitNoProcess;
                }
                std::cout << console::render_header(*payload, session.pid())
                          << console::render_rows(*payload, live, scripts.states());
                return kExitOk;
            }

            return interactive(*payload, session, scripts, resolver, live, options.interval_ms);
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
