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

    struct LiveRequest
    {
        std::size_t   id {};
        std::uint64_t address {};
        std::size_t   size {};
    };

    struct LiveReading
    {
        std::size_t            id {};
        bool                   readable {};
        std::vector<std::byte> bytes;
    };

    struct LiveSurface
    {
        virtual ~LiveSurface() = default;

        [[nodiscard]] virtual std::vector<LiveRequest> next_live_request()                                        = 0;
        virtual void                                   apply_live_readings(std::span<const LiveReading> readings) = 0;
    };

    // A component that wants work done on the same cadence as the read pass.
    // It is asked once per interval while a target is attached and the live
    // update toggle is on, after the read pass has been submitted, so a slow
    // implementation can only delay one batched read. It applies its own
    // in-flight and spacing rules.
    struct LiveTicker
    {
        virtual ~LiveTicker() = default;

        [[nodiscard]] virtual bool tick(std::chrono::milliseconds interval) = 0;
    };

    class LiveValues : public QObject
    {
        Q_OBJECT

    public:
        LiveValues(process::AccessWorker&   worker,
                   process::AttachedTarget& target,
                   SettingsController&      settings,
                   QObject*                 parent = nullptr);

        void add_surface(LiveSurface* surface);

        void add_ticker(LiveTicker* ticker);

        void poll();

        void request_now();

    private:
        void apply(process::JobId job_id, process::ReadManyResult&& result);
        void refresh_from_settings();

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;
        SettingsController&      settings_;

        std::vector<LiveSurface*> surfaces_;

        std::vector<LiveTicker*> tickers_;

        std::optional<process::JobId> pending_;
        std::vector<std::size_t>      pending_counts_;
        std::vector<std::size_t>      pending_ids_;

        bool                                  enabled_ {true};
        int                                   interval_ms_ {250};
        bool                                  force_next_ {false};
        std::chrono::steady_clock::time_point last_submit_ {};

        bool logged_idle_ {false};
    };

}
