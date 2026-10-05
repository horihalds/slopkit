#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"

#include <QObject>

namespace slopkit::ui
{
    class SettingsController;

    // One address a surface wants re-read, scoped to that surface: `id` is only
    // meaningful to the surface that produced it and is echoed back unchanged.
    struct LiveRequest
    {
        std::size_t   id {};
        std::uint64_t address {};
        std::size_t   size {};
    };

    // The outcome of one request. `readable` false marks an address that could
    // not be read; `bytes` then holds nothing.
    struct LiveReading
    {
        std::size_t            id {};
        bool                   readable {};
        std::vector<std::byte> bytes;
    };

    // A surface that shows memory data and can be kept live: it reports the
    // addresses it currently displays and applies the readings of one pass.
    struct LiveSurface
    {
        virtual ~LiveSurface() = default;

        [[nodiscard]] virtual std::vector<LiveRequest> next_live_request()                                        = 0;
        virtual void                                   apply_live_readings(std::span<const LiveReading> readings) = 0;
    };

    // The single owner of the live cadence. It collects the requests of every
    // registered surface on a poll, submits them as one batched worker job and
    // fans the readings back out, so one interval is one job and one session
    // lock. It never submits while disabled, detached or idle, and it never
    // queues a second pass while one is in flight.
    class LiveValues : public QObject
    {
        Q_OBJECT

    public:
        LiveValues(process::AccessWorker&   worker,
                   process::AttachedTarget& target,
                   SettingsController&      settings,
                   QObject*                 parent = nullptr);

        // Registers a surface; not owned, registered once at construction.
        void add_surface(LiveSurface* surface);

        // Called from MainWindow::on_tick(); submits a pass when the cadence
        // allows it.
        void poll();

        // Forces the next poll to submit without waiting for the interval.
        void request_now();

    private:
        void apply(process::JobId job_id, process::ReadManyResult&& result);
        void refresh_from_settings();

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;
        SettingsController&      settings_;

        std::vector<LiveSurface*> surfaces_;

        // The in-flight pass: the job id, the number of requests per surface and
        // the flat request ids in submission order.
        std::optional<process::JobId> pending_;
        std::vector<std::size_t>      pending_counts_;
        std::vector<std::size_t>      pending_ids_;

        bool                                  enabled_ {true};
        int                                   interval_ms_ {250};
        bool                                  force_next_ {false};
        std::chrono::steady_clock::time_point last_submit_ {};

        // Transition logging: one record per state change, never per tick.
        bool logged_idle_ {false};
    };

} // namespace slopkit::ui
