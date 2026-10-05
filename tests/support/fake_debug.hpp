#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "debug/backend.hpp"
#include "debug/worker.hpp"
#include "process/types.hpp"

namespace slopkit::tests
{
    using slopkit::debug::Frame;
    using slopkit::debug::HardwareKind;
    using slopkit::debug::RegisterValue;
    using slopkit::debug::StopEvent;
    using slopkit::process::AccessError;
    using slopkit::process::ProcessId;

    // The 18-register file the debugger expects, with RIP set.
    inline std::vector<RegisterValue> default_registers(std::uint64_t rip = 0x1000)
    {
        std::vector<RegisterValue> registers;
        for (const char* name : {"RAX",
                                 "RBX",
                                 "RCX",
                                 "RDX",
                                 "RSI",
                                 "RDI",
                                 "RBP",
                                 "RSP",
                                 "R8",
                                 "R9",
                                 "R10",
                                 "R11",
                                 "R12",
                                 "R13",
                                 "R14",
                                 "R15",
                                 "RIP",
                                 "RFLAGS"})
        {
            registers.push_back(RegisterValue {name, 0});
        }
        for (RegisterValue& value : registers)
        {
            if (value.name == "RIP")
            {
                value.value = rip;
            }
        }
        return registers;
    }

    // A scriptable DebugBackend: records every call and returns what the test
    // queues, so the controller and the worker can be driven without a target.
    class FakeDebugBackend final : public slopkit::debug::DebugBackend
    {
    public:
        using StopReply = std::expected<StopEvent, AccessError>;

        std::vector<std::string>   calls;
        bool                       supported {true};
        std::optional<AccessError> attach_error;
        std::optional<AccessError> arm_error;
        std::deque<StopReply>      stop_replies;
        std::vector<RegisterValue> register_file {default_registers()};
        std::vector<Frame>         frames {
            {0x1000, 0}
        };

        std::optional<ProcessId>                                                               attached_pid;
        std::string                                                                            attached_plugin;
        std::uint64_t                                                                          last_resume_address {};
        std::size_t                                                                            last_resume_step_size {};
        std::optional<std::uint32_t>                                                           last_step_tid;
        std::optional<std::uint32_t>                                                           last_interrupt_tid;
        std::vector<std::tuple<std::uint32_t, std::uint64_t, bool>>                            software_calls;
        std::vector<std::tuple<std::uint32_t, HardwareKind, std::uint64_t, std::size_t, bool>> hardware_calls;
        std::vector<std::tuple<std::uint32_t, std::string, std::uint64_t>>                     register_writes;

        // Blocking simulation for the run-thread test.
        bool                    block_continue {false};
        std::mutex              continue_mutex;
        std::condition_variable continue_cv;
        std::atomic<bool>       continue_entered {false};
        std::atomic<bool>       continue_released {false};
        std::atomic<bool>       interrupted {false};
        std::uint32_t           interrupt_release_tid {};

        std::expected<void, AccessError> attach(ProcessId pid, std::string_view plugin_id) override
        {
            calls.emplace_back("attach");
            attached_pid    = pid;
            attached_plugin = std::string(plugin_id);
            if (attach_error)
            {
                return std::unexpected(*attach_error);
            }
            return {};
        }

        std::expected<void, AccessError> detach() override
        {
            calls.emplace_back("detach");
            return {};
        }

        std::expected<StopEvent, AccessError> cont(std::uint64_t resume_address, std::size_t resume_step_size) override
        {
            calls.emplace_back("cont");
            last_resume_address   = resume_address;
            last_resume_step_size = resume_step_size;
            if (block_continue)
            {
                continue_entered = true;
                std::unique_lock lock(continue_mutex);
                continue_cv.wait(lock,
                                 [this]
                                 {
                                     return continue_released.load();
                                 });
                // Consume the release, so the next continue blocks again: a real
                // continue stops on every stop, and the maintenance round-trip
                // relies on the resume blocking until the next stop.
                continue_released = false;
            }
            return next_stop();
        }

        std::expected<StopEvent, AccessError> step(std::uint32_t tid) override
        {
            calls.emplace_back("step");
            last_step_tid = tid;
            return next_stop();
        }

        std::expected<void, AccessError> interrupt(std::uint32_t tid) override
        {
            calls.emplace_back("interrupt");
            last_interrupt_tid = tid;
            interrupted        = true;
            {
                const std::lock_guard lock(continue_mutex);
                continue_released = true;
            }
            continue_cv.notify_all();
            return {};
        }

        std::expected<std::vector<RegisterValue>, AccessError> registers(std::uint32_t tid) override
        {
            calls.emplace_back("registers");
            (void)tid;
            return register_file;
        }

        std::expected<void, AccessError>
        set_register(std::uint32_t tid, std::string_view name, std::uint64_t value) override
        {
            calls.emplace_back("set_register");
            register_writes.emplace_back(tid, std::string(name), value);
            for (RegisterValue& entry : register_file)
            {
                if (entry.name == name)
                {
                    entry.value = value;
                }
            }
            return {};
        }

        std::expected<void, AccessError>
        set_software_breakpoint(std::uint32_t slot, std::uint64_t address, bool insert) override
        {
            calls.emplace_back("set_software_breakpoint");
            software_calls.emplace_back(slot, address, insert);
            if (arm_error)
            {
                return std::unexpected(*arm_error);
            }
            return {};
        }

        std::expected<void, AccessError> set_hardware_breakpoint(
            std::uint32_t slot, HardwareKind kind, std::uint64_t address, std::size_t size, bool insert) override
        {
            calls.emplace_back("set_hardware_breakpoint");
            hardware_calls.emplace_back(slot, kind, address, size, insert);
            if (arm_error)
            {
                return std::unexpected(*arm_error);
            }
            return {};
        }

        std::expected<std::vector<Frame>, AccessError> backtrace(std::uint32_t tid) override
        {
            calls.emplace_back("backtrace");
            (void)tid;
            return frames;
        }

        [[nodiscard]] bool supports_debug() const override
        {
            return supported;
        }

        [[nodiscard]] std::size_t count(const std::string& call) const
        {
            std::size_t total = 0;
            for (const std::string& entry : calls)
            {
                total += entry == call ? 1 : 0;
            }
            return total;
        }

    private:
        std::expected<StopEvent, AccessError> next_stop()
        {
            if (stop_replies.empty())
            {
                return StopEvent {slopkit::debug::StopReason::single_step, 4242, 0x1001, 0x1001, std::nullopt, 0};
            }
            auto reply = std::move(stop_replies.front());
            stop_replies.pop_front();
            return reply;
        }
    };

    // Drives a controller/worker until `predicate` is true, pumping completions.
    template<typename Controller, typename Predicate>
    bool pump_until(Controller& controller, Predicate predicate, int iterations = 2000)
    {
        for (int i = 0; i < iterations; ++i)
        {
            controller.drain();
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        controller.drain();
        return predicate();
    }

    // Waits until a plain predicate holds, without a controller to pump.
    template<typename Predicate>
    bool wait_until(Predicate predicate, int iterations = 2000)
    {
        for (int i = 0; i < iterations; ++i)
        {
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return predicate();
    }

} // namespace slopkit::tests
