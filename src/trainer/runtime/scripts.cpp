#include "trainer/runtime/scripts.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "reclass/serialize.hpp"
#include "script/structs_view.hpp"
#include "script/types.hpp"

namespace slopkit::trainer
{
    namespace
    {
        std::string reason_of(const std::string& message, const std::string& error)
        {
            return message.empty() ? error : message;
        }
    }

    ScriptHost::ScriptHost(const Target& target, const Payload& payload, std::uint64_t fallback_interval_ms)
        : target_ {target}, scripts_ {payload.scripts}, fallback_interval_ms_ {fallback_interval_ms}, caves_ {target},
          engine_ {memory_api(), symbols_.api()}
    {
        states_.resize(scripts_.size());

        std::vector<reclass::Member> members;
        members.reserve(payload.classes.size());
        for (const auto& type : payload.classes)
        {
            members.push_back(reclass::Member {type.name, type.text});
        }

        if (auto project = reclass::from_members(members))
        {
            project_ = std::move(*project);
        }
        else
        {
            class_error_ = project.error();
        }
        engine_.set_classes(script::class_index_of(project_));
    }

    std::size_t ScriptHost::size() const noexcept
    {
        return states_.size();
    }

    std::span<const ScriptState> ScriptHost::states() const noexcept
    {
        return states_;
    }

    const std::vector<PayloadScript>& ScriptHost::scripts() const noexcept
    {
        return scripts_;
    }

    std::vector<expr::SymbolRef> ScriptHost::symbols() const
    {
        return symbols_.snapshot();
    }

    const std::string& ScriptHost::class_error() const noexcept
    {
        return class_error_;
    }

    script::MemoryApi ScriptHost::memory_api()
    {
        script::MemoryApi api;
        api.pointer_size = [this]()
        {
            return static_cast<std::size_t>(target_.pointer_size());
        };
        api.read = [this](std::uint64_t address, std::size_t size)
        {
            return target_.read(address, size);
        };
        api.write = [this](std::uint64_t address, std::span<const std::byte> bytes)
        {
            return target_.write(address, bytes);
        };
        api.regions = [this]() -> std::expected<std::vector<script::MemoryRegion>, std::string>
        {
            std::vector<script::MemoryRegion> regions;
            for (const Region& region : target_.regions())
            {
                regions.push_back(script::MemoryRegion {region.start,
                                                        region.end > region.start ? region.end - region.start : 0,
                                                        region.readable,
                                                        region.name});
            }
            return regions;
        };
        api.allocate = [this](std::size_t size, std::uint64_t near)
        {
            return caves_.allocate(size, near);
        };
        api.deallocate = [this](std::uint64_t address)
        {
            return caves_.deallocate(address);
        };
        api.validate = [this](std::uint64_t address, std::size_t size) -> std::expected<bool, std::string>
        {
            return target_.readable(address, size);
        };
        api.modules = [this]() -> std::expected<std::vector<expr::ModuleRef>, std::string>
        {
            std::vector<expr::ModuleRef> modules;
            for (const Module& module : target_.modules())
            {
                modules.push_back(expr::ModuleRef {module.name, module.base});
            }
            return modules;
        };
        return api;
    }

    void ScriptHost::adopt_interval(std::size_t                           index,
                                    const std::optional<std::uint64_t>&   interval,
                                    std::chrono::steady_clock::time_point now)
    {
        ScriptState& state = states_[index];
        // A script asking for a zero interval would spin the trainer's loop, so
        // the trainer's own interval is the floor; a script that never asked for
        // one keeps ticking at the interval it last chose.
        const auto   chosen =
            interval && *interval > 0 ? *interval : (state.interval_ms > 0 ? state.interval_ms : fallback_interval_ms_);
        state.interval_ms = chosen;
        state.next_update = now + std::chrono::milliseconds {static_cast<std::int64_t>(chosen)};
    }

    void
    ScriptHost::note(std::size_t index, const std::vector<std::string>& output, const std::string& message, bool failed)
    {
        ScriptState& state = states_[index];
        state.output       = output;
        state.message      = message;
        state.failed       = failed;
    }

    std::expected<void, std::string> ScriptHost::activate(std::size_t index)
    {
        ScriptState& state = states_[index];
        if (state.active)
        {
            return std::unexpected(std::format("script {} is already active", index + 1));
        }

        const auto                    now = std::chrono::steady_clock::now();
        const script::LifecycleResult outcome =
            engine_.run_lifecycle(scripts_[index].source, std::string {script::kActivateHook});
        state.active = outcome.ok;
        if (!outcome.ok)
        {
            state.interval_ms = 0;
            state.next_update = {};
            note(index, outcome.output, reason_of(outcome.message, outcome.error), true);
            return std::unexpected(state.message);
        }

        note(index, outcome.output, outcome.message, false);
        adopt_interval(index, outcome.interval_ms, now);
        return {};
    }

    std::expected<void, std::string> ScriptHost::deactivate(std::size_t index)
    {
        ScriptState& state = states_[index];
        if (!state.active)
        {
            return std::unexpected(std::format("script {} is not active", index + 1));
        }

        const script::LifecycleResult outcome =
            engine_.run_lifecycle(scripts_[index].source, std::string {script::kDeactivateHook});
        state.active      = false;
        state.interval_ms = 0;
        state.next_update = {};
        note(index, outcome.output, reason_of(outcome.message, outcome.error), !outcome.ok);
        if (outcome.ok)
        {
            return {};
        }
        return std::unexpected(state.message);
    }

    void ScriptHost::tick(std::chrono::steady_clock::time_point now)
    {
        for (std::size_t index = 0; index < states_.size(); ++index)
        {
            const ScriptState& state = states_[index];
            if (!state.active || now < state.next_update)
            {
                continue;
            }

            const script::UpdateResult outcome = engine_.run_update(scripts_[index].source);
            if (!outcome.ok)
            {
                // A failing update stops this script alone: the others keep ticking
                // and the trainer's exit code is not affected.
                states_[index].active      = false;
                states_[index].interval_ms = 0;
                note(index, outcome.output, reason_of(outcome.message, outcome.error), true);
                continue;
            }

            note(index, outcome.output, outcome.message, false);
            adopt_interval(index, outcome.interval_ms, now);
        }
    }

    std::string ScriptHost::output_text(std::size_t index) const
    {
        const ScriptState& state = states_[index];
        std::string        text;
        for (const std::string& line : state.output)
        {
            text += line;
            text += '\n';
        }
        if (!state.message.empty())
        {
            text += state.message;
            text += '\n';
        }
        return text;
    }

}
