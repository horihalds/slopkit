#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include "process/access_worker.hpp"
#include "ui/live_values.hpp"

namespace slopkit::ui
{
    class ScriptUpdates : public LiveTicker
    {
    public:
        using FailureHandler = std::function<void(const process::ScriptUpdateFailure&)>;

        // The classes the ticked scripts may name, snapshotted on the UI thread
        // when a pass is submitted, so the worker never reads the editing model.
        using ClassesProvider = std::function<std::shared_ptr<const script::ClassIndex>()>;

        ScriptUpdates(process::AccessWorker& worker, ClassesProvider classes);

        void set_failure_handler(FailureHandler handler);

        [[nodiscard]] bool tick(std::chrono::milliseconds interval) override;

    private:
        void apply(process::JobId job_id, process::ScriptUpdatesResult&& result);

        process::AccessWorker&                worker_;
        ClassesProvider                       classes_;
        std::optional<process::JobId>         pending_;
        std::chrono::steady_clock::time_point last_tick_ {};
        bool                                  logged_idle_ {false};
        FailureHandler                        on_failure_;
    };

}
