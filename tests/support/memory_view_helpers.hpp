#pragma once

#include <catch2/catch.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include "core/log.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "scan/types.hpp"
#include "support/fake_process.hpp"
#include "ui/components/memory_view.hpp"
#include "ui/components/memory_view_document.hpp"
#include "ui/live_values.hpp"

namespace
{
    using slopkit::test::application;
    using slopkit::test::fake_target;
    using slopkit::test::FakeAccess;
    using slopkit::test::FakeBackend;
    using slopkit::test::FakeMemory;
    using slopkit::test::module_image;

    using slopkit::ui::components::MemoryView;
    using slopkit::ui::components::MemoryViewDocument;
    using slopkit::ui::components::TextEncoding;
    using slopkit::ui::components::ValueFormat;

    constexpr std::size_t   kRowBytes = 16;
    constexpr std::size_t   kRows     = 24;
    constexpr std::uint64_t kExtent   = kRowBytes * kRows;
    constexpr std::uint64_t kBase     = 0x1800; // A multiple of kExtent.

    template<typename Predicate>
    [[maybe_unused]] bool pump_worker(slopkit::process::AccessWorker& worker, Predicate done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            worker.drain();
            if (done())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    }

    [[maybe_unused]] void attach_session(slopkit::process::AccessWorker& worker)
    {
        bool attached = false;
        worker.submit_attach_app(worker.next_job_id(),
                                 42,
                                 "fake",
                                 [&](slopkit::process::JobResult&&)
                                 {
                                     attached = true;
                                 });
        REQUIRE(pump_worker(worker,
                            [&]
                            {
                                return attached;
                            }));
    }

    [[maybe_unused]] slopkit::process::AttachedTarget attached_target()
    {
        slopkit::process::AttachedTarget target;
        target.pid          = 42;
        target.session_live = true;
        target.method       = slopkit::process::AccessMethod::procfs_mem;
        return target;
    }

    [[maybe_unused]] void fill(FakeMemory& memory, std::uint64_t base, std::uint64_t size, std::byte value)
    {
        for (std::uint64_t index = 0; index < size; ++index)
        {
            memory.bytes[base + index] = value;
        }
    }

    [[maybe_unused]] std::vector<std::byte> block_bytes(const FakeMemory& memory, std::uint64_t base, std::size_t size)
    {
        std::vector<std::byte> bytes(size, std::byte {0});
        for (std::size_t index = 0; index < size; ++index)
        {
            if (const auto entry = memory.bytes.find(base + index); entry != memory.bytes.end())
            {
                bytes[index] = entry->second;
            }
        }
        return bytes;
    }

    [[maybe_unused]] slopkit::ui::LiveReading
    reading(std::size_t id, const FakeMemory& memory, std::uint64_t base, std::size_t size)
    {
        slopkit::ui::LiveReading value;
        value.id       = id;
        value.readable = true;
        value.bytes    = block_bytes(memory, base, size);
        return value;
    }

    // A document with a live target attached, its window parked on kBase.
    struct Fixture
    {
        FakeAccess                       access;
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target = attached_target();
        MemoryViewDocument               document {worker, target};

        Fixture()
        {
            attach_session(worker);
            document.set_visible(true);
            document.set_view(kBase, kRowBytes, kRows);
        }

        // Refills the visible window's three blocks from memory and applies them.
        void pass()
        {
            const auto                            requests = document.next_live_request();
            std::vector<slopkit::ui::LiveReading> readings;
            readings.reserve(requests.size());
            for (const slopkit::ui::LiveRequest& request : requests)
            {
                readings.push_back(reading(request.id, *access.memory, request.address, request.size));
            }
            document.apply_live_readings(readings);
        }
    };

    // Removes the registered sink even when a failing assertion unwinds the case.
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
