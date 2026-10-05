#pragma once

#include <catch2/catch.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "expr/resolver.hpp"
#include "process/access.hpp"
#include "process/access_worker.hpp"
#include "process/types.hpp"
#include "support/fake_process.hpp"

namespace
{
    using namespace std::chrono_literals;

    using slopkit::process::AccessError;
    using slopkit::process::AccessMethod;
    using slopkit::process::AccessWorker;
    using slopkit::process::AttachResult;
    using slopkit::process::FreezeResult;
    using slopkit::process::JobResult;
    using slopkit::process::ListResult;
    using slopkit::process::MemoryMapResult;
    using slopkit::process::ProbeResult;
    using slopkit::process::ReadManyItem;
    using slopkit::process::ReadManyResult;
    using slopkit::process::ReadResult;
    using slopkit::process::ResolveRequest;
    using slopkit::process::ResolveResult;
    using slopkit::process::WriteItem;
    using slopkit::process::WriteResult;
    using slopkit::test::FakeBackend;

    // The fake's flat window base.
    constexpr std::uint64_t kBase = 0x1000;

    // A ProcessAccess whose listing and attach block on a gate when gated, so a
    // test can hold a job in flight and prove that submission did not block.
    class GatedAccess final : public slopkit::process::ProcessAccess
    {
    public:
        explicit GatedAccess(bool gated) : gated_(gated) {}

        // True when any job has entered a fake call.
        void wait_until_entered()
        {
            std::unique_lock lock(mutex_);
            entered_cv_.wait(lock,
                             [&]
                             {
                                 return entered_ > 0;
                             });
        }

        void open_gate()
        {
            const std::lock_guard lock(mutex_);
            gate_open_ = true;
            gate_cv_.notify_all();
        }

        [[nodiscard]] FakeBackend* backend() const noexcept
        {
            return backend_;
        }

        // Applied to each backend attach() creates, so a test can model a target
        // whose metadata is readable but whose memory cannot be.
        std::optional<AccessError> read_error;

        std::expected<std::vector<slopkit::process::ProcessInfo>, AccessError> list_processes() override
        {
            enter();

            slopkit::process::ProcessInfo info;
            info.pid       = 7;
            info.name      = "target";
            info.exe_path  = "/usr/bin/target";
            info.plugin_id = "fake";
            info.claimants = {"fake"};
            return std::vector<slopkit::process::ProcessInfo> {std::move(info)};
        }

        std::expected<slopkit::process::Session, AccessError> attach(slopkit::process::ProcessId,
                                                                     std::string_view) override
        {
            enter();

            auto owned          = std::make_unique<FakeBackend>();
            owned->module_count = 2;
            owned->thread_count = 3;
            owned->read_error   = read_error;
            owned->fake_pid     = 7;
            owned->memory->base = kBase;
            owned->memory->flat.assign(0x40, std::byte {0});
            backend_ = owned.get();
            return slopkit::process::Session {std::move(owned)};
        }

    private:
        void enter()
        {
            std::unique_lock lock(mutex_);
            ++entered_;
            entered_cv_.notify_all();
            if (gated_)
            {
                gate_cv_.wait(lock,
                              [&]
                              {
                                  return gate_open_;
                              });
            }
        }

        bool                    gated_ {true};
        mutable std::mutex      mutex_;
        std::condition_variable entered_cv_;
        std::condition_variable gate_cv_;
        std::size_t             entered_ {0};
        bool                    gate_open_ {false};
        FakeBackend*            backend_ {nullptr};
    };

    // Drains the worker until `done` reports true or the timeout elapses.
    template<typename Predicate>
    bool pump(AccessWorker& worker, Predicate done)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            worker.drain();
            if (done())
            {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return done();
    }

    // Restores the process-wide log level on scope exit.
    class LevelGuard
    {
    public:
        LevelGuard() : previous_(slopkit::log::Logger::instance().minimum_level()) {}

        LevelGuard(const LevelGuard&)            = delete;
        LevelGuard& operator=(const LevelGuard&) = delete;

        ~LevelGuard()
        {
            slopkit::log::Logger::instance().set_minimum_level(previous_);
        }

    private:
        slopkit::log::Level previous_;
    };

    // Registers a sink for the lifetime of the guard.
    class SinkGuard
    {
    public:
        explicit SinkGuard(slopkit::log::Sink sink) : id_(slopkit::log::Logger::instance().add_sink(std::move(sink))) {}

        SinkGuard(const SinkGuard&)            = delete;
        SinkGuard& operator=(const SinkGuard&) = delete;

        ~SinkGuard()
        {
            slopkit::log::Logger::instance().remove_sink(id_);
        }

    private:
        slopkit::log::SinkId id_;
    };
} // namespace
