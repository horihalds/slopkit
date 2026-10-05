#include "ui/live_values.hpp"

#include <format>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/settings.hpp"

namespace slopkit::ui
{

    LiveValues::LiveValues(process::AccessWorker&   worker,
                           process::AttachedTarget& target,
                           SettingsController&      settings,
                           QObject*                 parent)
        : QObject(parent), worker_(worker), target_(target), settings_(settings)
    {
        connect(&settings_,
                &SettingsController::liveUpdateChanged,
                this,
                [this](bool)
                {
                    refresh_from_settings();
                });
        connect(&settings_,
                &SettingsController::liveUpdateIntervalChanged,
                this,
                [this](int)
                {
                    refresh_from_settings();
                });
        refresh_from_settings();
    }

    void LiveValues::add_surface(LiveSurface* surface)
    {
        if (surface != nullptr)
        {
            surfaces_.push_back(surface);
        }
    }

    void LiveValues::refresh_from_settings()
    {
        const Settings& values = settings_.values();
        enabled_               = values.live_update_enabled;
        interval_ms_           = values.live_update_interval_ms;
        if (interval_ms_ <= 0)
        {
            interval_ms_ = 250;
        }
        if (!enabled_)
        {
            force_next_ = false;
        }
    }

    void LiveValues::request_now()
    {
        force_next_ = true;
        poll();
    }

    void LiveValues::poll()
    {
        // The cadence, enable flag and interval are re-read every poll, so a
        // settings change applies without a restart and without a timer restart.
        refresh_from_settings();

        if (!enabled_)
        {
            if (!logged_idle_)
            {
                log::debug(log::category::ui, "live update idle: disabled");
                logged_idle_ = true;
            }
            return;
        }
        if (!target_.valid())
        {
            if (!logged_idle_)
            {
                log::debug(log::category::ui, "live update idle: no target attached");
                logged_idle_ = true;
            }
            return;
        }
        logged_idle_ = false;

        if (pending_.has_value())
        {
            return; // One pass in flight; skip this tick instead of queueing.
        }

        const auto now = std::chrono::steady_clock::now();
        if (!force_next_ && now - last_submit_ < std::chrono::milliseconds(interval_ms_))
        {
            return;
        }

        std::vector<process::ReadManyItem> items;
        std::vector<std::size_t>           counts;
        std::vector<std::size_t>           ids;
        counts.reserve(surfaces_.size());
        for (LiveSurface* surface : surfaces_)
        {
            std::vector<LiveRequest> requests = surface->next_live_request();
            for (const LiveRequest& request : requests)
            {
                items.push_back(process::ReadManyItem {.address = request.address, .size = request.size});
                ids.push_back(request.id);
            }
            counts.push_back(requests.size());
        }

        if (items.empty())
        {
            return; // Nothing displayed to read.
        }

        const process::JobId job_id = worker_.next_job_id();
        pending_                    = job_id;
        pending_counts_             = std::move(counts);
        pending_ids_                = std::move(ids);
        last_submit_                = now;
        force_next_                 = false;

        const bool submitted =
            worker_.submit_read_many(job_id,
                                     std::move(items),
                                     [this, job_id](process::JobResult&& result)
                                     {
                                         apply(job_id, std::get<process::ReadManyResult>(std::move(result)));
                                     });
        if (!submitted)
        {
            pending_.reset();
            pending_counts_.clear();
            pending_ids_.clear();
            log::warning(log::category::ui, "live read unavailable");
        }
    }

    void LiveValues::apply(process::JobId job_id, process::ReadManyResult&& result)
    {
        if (pending_ != job_id)
        {
            return; // Stale or superseded: the request set has moved on.
        }
        pending_.reset();

        std::size_t offset = 0;
        for (std::size_t index = 0; index < surfaces_.size(); ++index)
        {
            const std::size_t count = index < pending_counts_.size() ? pending_counts_[index] : 0;

            std::vector<LiveReading> readings;
            readings.reserve(count);
            for (std::size_t k = 0; k < count; ++k, ++offset)
            {
                LiveReading reading;
                if (offset < pending_ids_.size())
                {
                    reading.id = pending_ids_[offset];
                }
                if (offset < result.items.size())
                {
                    const auto& item = result.items[offset];
                    reading.readable = item.has_value();
                    if (item.has_value())
                    {
                        reading.bytes = *item;
                    }
                }
                readings.push_back(std::move(reading));
            }
            surfaces_[index]->apply_live_readings(readings);
        }

        pending_counts_.clear();
        pending_ids_.clear();
    }

} // namespace slopkit::ui
