#pragma once

#include <chrono>
#include <functional>
#include <optional>

#include "process/access_worker.hpp"
#include "ui/live_values.hpp"

namespace slopkit::ui
{

    // Drives the `update` hook of every ticked script on the live cadence: one
    // batched worker job per interval, skipped while no script is ticked, with
    // at most one pass in flight. The worker deactivates a script whose tick
    // failed, so a failure is only handed to the handler, which clears the row's
    // tick and reports it; this class never submits a deactivate of its own.
    class ScriptUpdates : public LiveTicker
    {
    public:
        using FailureHandler = std::function<void(const process::ScriptUpdateFailure&)>;

        explicit ScriptUpdates(process::AccessWorker& worker);

        void set_failure_handler(FailureHandler handler);

        // A pass is due at most once per `interval`; true when one was submitted.
        [[nodiscard]] bool tick(std::chrono::milliseconds interval) override;

    private:
        void apply(process::JobId job_id, process::ScriptUpdatesResult&& result);

        process::AccessWorker&                worker_;
        std::optional<process::JobId>         pending_; // one pass in flight
        std::chrono::steady_clock::time_point last_tick_ {};
        bool                                  logged_idle_ {false};
        FailureHandler                        on_failure_;
    };

} // namespace slopkit::ui
