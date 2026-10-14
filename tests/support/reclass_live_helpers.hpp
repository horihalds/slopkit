#pragma once

#include <catch2/catch.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <thread>

#include "process/access_worker.hpp"
#include "support/fake_process.hpp"
#include "ui/live_values.hpp"

namespace slopkit::test
{
    inline constexpr process::ProcessId kFakePid = 42;

    // Waits for the worker to run everything it was handed, draining the
    // completions the way the UI event loop does.
    inline bool drain_until_idle(process::AccessWorker&    worker,
                                 std::chrono::milliseconds deadline = std::chrono::seconds(5))
    {
        const auto until = std::chrono::steady_clock::now() + deadline;
        while (std::chrono::steady_clock::now() < until)
        {
            worker.drain();
            if (worker.idle())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        worker.drain();
        return worker.idle();
    }

    inline void attach_session(process::AccessWorker& worker)
    {
        bool attached = false;
        worker.submit_attach_app(worker.next_job_id(),
                                 kFakePid,
                                 "fake",
                                 [&](process::JobResult&&)
                                 {
                                     attached = true;
                                 });
        REQUIRE(drain_until_idle(worker));
        REQUIRE(attached);
    }

    inline void stage_bytes(FakeAccess& access, std::uint64_t address, std::initializer_list<unsigned> values)
    {
        std::uint64_t offset = 0;
        for (const unsigned value : values)
        {
            access.memory->bytes[address + offset] = static_cast<std::byte>(value);
            ++offset;
        }
    }

    // The reading a live pass would report for one block of the fake target.
    [[nodiscard]] inline ui::LiveReading
    reading_for(const FakeAccess& access, std::size_t id, std::uint64_t address, std::size_t size)
    {
        ui::LiveReading value;
        value.id       = id;
        value.readable = true;
        value.bytes.assign(size, std::byte {0});
        for (std::size_t index = 0; index < size; ++index)
        {
            if (const auto entry = access.memory->bytes.find(address + index); entry != access.memory->bytes.end())
            {
                value.bytes[index] = entry->second;
            }
        }
        return value;
    }

}
