#include <catch2/catch.hpp>

#include <atomic>
#include <variant>
#include <vector>

#include "debug/worker.hpp"
#include "support/fake_debug.hpp"

using slopkit::debug::JobResult;
using slopkit::debug::RegisterValue;
using slopkit::debug::Worker;
using slopkit::tests::FakeDebugBackend;
using slopkit::tests::wait_until;

TEST_CASE("worker drains completions in submission order", "[debug][worker]")
{
    FakeDebugBackend backend;
    Worker           worker(backend);

    const auto first_id  = worker.next_job_id();
    const auto second_id = worker.next_job_id();
    CHECK(second_id == first_id + 1);

    std::vector<int>  order;
    std::atomic<bool> done {false};

    REQUIRE(worker.submit_registers(first_id,
                                    1,
                                    [&](JobResult&& result)
                                    {
                                        const auto* registers = std::get_if<slopkit::debug::RegistersResult>(&result);
                                        REQUIRE(registers != nullptr);
                                        CHECK(registers->values.size() == 18);
                                        order.push_back(1);
                                    }));
    REQUIRE(worker.submit_backtrace(second_id,
                                    1,
                                    [&](JobResult&& result)
                                    {
                                        const auto* frames = std::get_if<slopkit::debug::BacktraceResult>(&result);
                                        REQUIRE(frames != nullptr);
                                        CHECK(frames->frames.size() == 1);
                                        order.push_back(2);
                                        done = true;
                                    }));

    REQUIRE(wait_until(
        [&]
        {
            worker.drain();
            return done.load();
        }));
    CHECK(order == std::vector<int> {1, 2});
}

TEST_CASE("worker reports a job error through the completion", "[debug][worker]")
{
    FakeDebugBackend backend;
    backend.arm_error = slopkit::process::AccessError::permission_denied;
    Worker worker(backend);

    std::atomic<bool> finished {false};
    REQUIRE(worker.submit_software_breakpoint(worker.next_job_id(),
                                              0,
                                              0x1000,
                                              true,
                                              [&](JobResult&& result)
                                              {
                                                  const auto* value = std::get_if<slopkit::debug::VoidResult>(&result);
                                                  REQUIRE(value != nullptr);
                                                  REQUIRE(value->error.has_value());
                                                  CHECK(*value->error
                                                        == slopkit::process::AccessError::permission_denied);
                                                  finished = true;
                                              }));

    REQUIRE(wait_until(
        [&]
        {
            worker.drain();
            return finished.load();
        }));
}

TEST_CASE("worker services an interrupt while a continue blocks", "[debug][worker]")
{
    FakeDebugBackend backend;
    backend.block_continue = true;
    Worker worker(backend);

    std::atomic<bool> continue_done {false};
    std::atomic<bool> interrupt_done {false};

    REQUIRE(worker.submit_continue(worker.next_job_id(),
                                   7,
                                   0,
                                   0,
                                   [&](JobResult&&)
                                   {
                                       continue_done = true;
                                   }));
    REQUIRE(wait_until(
        [&]
        {
            return backend.continue_entered.load();
        }));

    // The run thread is inside cont; the interrupt must still be serviced.
    REQUIRE(worker.submit_interrupt(worker.next_job_id(),
                                    7,
                                    [&](JobResult&& result)
                                    {
                                        const auto* value = std::get_if<slopkit::debug::VoidResult>(&result);
                                        REQUIRE(value != nullptr);
                                        CHECK_FALSE(value->error.has_value());
                                        interrupt_done = true;
                                    }));

    REQUIRE(wait_until(
        [&]
        {
            worker.drain();
            return interrupt_done.load();
        }));
    CHECK(backend.last_interrupt_tid == 7);

    REQUIRE(wait_until(
        [&]
        {
            worker.drain();
            return continue_done.load();
        }));
}

TEST_CASE("worker shutdown interrupts a blocking continue", "[debug][worker]")
{
    FakeDebugBackend backend;
    backend.block_continue = true;
    {
        Worker worker(backend);
        REQUIRE(worker.submit_continue(worker.next_job_id(), 7, 0, 0, [](JobResult&&) {}));
        REQUIRE(wait_until(
            [&]
            {
                return backend.continue_entered.load();
            }));
    } // The destructor must interrupt the run thread and join it, not hang.

    CHECK(backend.interrupted.load());
}
