#include "ui/script_updates.hpp"

#include <chrono>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::ui
{

    ScriptUpdates::ScriptUpdates(process::AccessWorker& worker) : worker_(worker) {}

    void ScriptUpdates::set_failure_handler(FailureHandler handler)
    {
        on_failure_ = std::move(handler);
    }

    bool ScriptUpdates::tick(std::chrono::milliseconds interval)
    {
        if (pending_.has_value())
        {
            return false; // One pass in flight; a slow hook cannot queue up.
        }
        if (worker_.active_scripts() == 0)
        {
            // Idle: a target with no ticked script adds no job and no traffic.
            if (!logged_idle_)
            {
                log::debug(log::category::ui, "script updates idle: no script is ticked");
                logged_idle_ = true;
            }
            return false;
        }
        logged_idle_ = false;

        const auto now = std::chrono::steady_clock::now();
        if (now - last_tick_ < interval)
        {
            return false; // A burst of forced polls cannot run hooks early.
        }

        const process::JobId job_id = worker_.next_job_id();
        last_tick_                  = now;
        pending_                    = job_id;

        const bool submitted =
            worker_.submit_script_updates(job_id,
                                          [this, job_id](process::JobResult&& result)
                                          {
                                              apply(job_id, std::get<process::ScriptUpdatesResult>(std::move(result)));
                                          });
        if (!submitted)
        {
            pending_.reset();
            log::warning(log::category::ui, "script update pass unavailable");
            return false;
        }
        return true;
    }

    void ScriptUpdates::apply(process::JobId job_id, process::ScriptUpdatesResult&& result)
    {
        if (pending_ != job_id)
        {
            return; // Stale or superseded: the pass has moved on.
        }
        pending_.reset();

        // The worker already ran each failed script's deactivate: the handler
        // only clears the row and reports the reason.
        for (const process::ScriptUpdateFailure& failure : result.failed)
        {
            if (on_failure_)
            {
                on_failure_(failure);
            }
        }
    }

} // namespace slopkit::ui
