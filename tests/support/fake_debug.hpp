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
    //
    // Threading: the debug worker thread records through the overrides while the
    // test thread reads and clears. Every recorded and scripted member therefore
    // lives behind `state_mutex_` and is only touched through an accessor or a
    // mutator, so a reader gets a snapshot and never sees a half-written value.
    //
    // Lock order: `state_mutex_` and `continue_mutex` are never held together.
    // The overrides record under `state_mutex_`, release it, and only then touch
    // `continue_mutex`/`continue_cv` — `cont()` records before it blocks, and
    // `interrupt()` records before it releases the blocked continue.
    class FakeDebugBackend final : public slopkit::debug::DebugBackend
    {
    public:
        using StopReply = std::expected<StopEvent, AccessError>;

        // --- scripted input (written by the test thread) ----------------------
        void fail_attach(AccessError error)
        {
            const std::lock_guard lock(state_mutex_);
            attach_error_ = error;
        }

        void fail_arm(AccessError error)
        {
            const std::lock_guard lock(state_mutex_);
            arm_error_ = error;
        }

        void set_supported(bool value)
        {
            const std::lock_guard lock(state_mutex_);
            supported_ = value;
        }

        void set_register_file(std::vector<RegisterValue> registers)
        {
            const std::lock_guard lock(state_mutex_);
            register_file_ = std::move(registers);
        }

        void set_frames(std::vector<Frame> frames)
        {
            const std::lock_guard lock(state_mutex_);
            frames_ = std::move(frames);
        }

        void queue_stop(StopReply reply)
        {
            const std::lock_guard lock(state_mutex_);
            stop_replies_.push_back(std::move(reply));
        }

        // The blocking simulation: a flag crossed between threads, so an atomic.
        std::atomic<bool>       block_continue {false};
        std::mutex              continue_mutex;
        std::condition_variable continue_cv;
        std::atomic<bool>       continue_entered {false};
        std::atomic<bool>       continue_released {false};
        std::atomic<bool>       interrupted {false};

        // --- recorded output (written by the debug worker thread) -------------
        [[nodiscard]] std::vector<std::string> calls() const
        {
            const std::lock_guard lock(state_mutex_);
            return calls_;
        }

        [[nodiscard]] std::size_t count(std::string_view call) const
        {
            const std::lock_guard lock(state_mutex_);
            std::size_t           total = 0;
            for (const std::string& entry : calls_)
            {
                total += entry == call ? 1 : 0;
            }
            return total;
        }

        void clear_calls()
        {
            const std::lock_guard lock(state_mutex_);
            calls_.clear();
        }

        [[nodiscard]] std::optional<ProcessId> attached_pid() const
        {
            const std::lock_guard lock(state_mutex_);
            return attached_pid_;
        }

        [[nodiscard]] std::string attached_plugin() const
        {
            const std::lock_guard lock(state_mutex_);
            return attached_plugin_;
        }

        [[nodiscard]] std::uint64_t last_resume_address() const
        {
            const std::lock_guard lock(state_mutex_);
            return last_resume_address_;
        }

        [[nodiscard]] std::size_t last_resume_step_size() const
        {
            const std::lock_guard lock(state_mutex_);
            return last_resume_step_size_;
        }

        [[nodiscard]] std::optional<std::uint32_t> last_step_tid() const
        {
            const std::lock_guard lock(state_mutex_);
            return last_step_tid_;
        }

        [[nodiscard]] std::optional<std::uint32_t> last_interrupt_tid() const
        {
            const std::lock_guard lock(state_mutex_);
            return last_interrupt_tid_;
        }

        [[nodiscard]] std::vector<std::tuple<std::uint32_t, std::uint64_t, bool>> software_calls() const
        {
            const std::lock_guard lock(state_mutex_);
            return software_calls_;
        }

        [[nodiscard]] std::vector<std::tuple<std::uint32_t, HardwareKind, std::uint64_t, std::size_t, bool>>
        hardware_calls() const
        {
            const std::lock_guard lock(state_mutex_);
            return hardware_calls_;
        }

        void clear_hardware_calls()
        {
            const std::lock_guard lock(state_mutex_);
            hardware_calls_.clear();
        }

        [[nodiscard]] std::vector<std::tuple<std::uint32_t, std::string, std::uint64_t>> register_writes() const
        {
            const std::lock_guard lock(state_mutex_);
            return register_writes_;
        }

        [[nodiscard]] std::vector<RegisterValue> register_file() const
        {
            const std::lock_guard lock(state_mutex_);
            return register_file_;
        }

        std::expected<void, AccessError> attach(ProcessId pid, std::string_view plugin_id) override
        {
            std::optional<AccessError> failure;
            {
                const std::lock_guard lock(state_mutex_);
                calls_.emplace_back("attach");
                attached_pid_    = pid;
                attached_plugin_ = std::string(plugin_id);
                failure          = attach_error_;
            }
            if (failure)
            {
                return std::unexpected(*failure);
            }
            return {};
        }

        std::expected<void, AccessError> detach() override
        {
            const std::lock_guard lock(state_mutex_);
            calls_.emplace_back("detach");
            return {};
        }

        std::expected<StopEvent, AccessError> cont(std::uint64_t resume_address, std::size_t resume_step_size) override
        {
            {
                const std::lock_guard lock(state_mutex_);
                calls_.emplace_back("cont");
                last_resume_address_   = resume_address;
                last_resume_step_size_ = resume_step_size;
            }
            if (block_continue.load())
            {
                continue_entered = true;
                std::unique_lock lock(continue_mutex); // state_mutex_ is free here
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
            {
                const std::lock_guard lock(state_mutex_);
                calls_.emplace_back("step");
                last_step_tid_ = tid;
            }
            return next_stop();
        }

        std::expected<void, AccessError> interrupt(std::uint32_t tid) override
        {
            {
                const std::lock_guard lock(state_mutex_);
                calls_.emplace_back("interrupt");
                last_interrupt_tid_ = tid;
            }
            interrupted = true;
            {
                const std::lock_guard lock(continue_mutex); // state_mutex_ is free here
                continue_released = true;
            }
            continue_cv.notify_all();
            return {};
        }

        std::expected<std::vector<RegisterValue>, AccessError> registers(std::uint32_t tid) override
        {
            (void)tid;
            const std::lock_guard lock(state_mutex_);
            calls_.emplace_back("registers");
            return register_file_;
        }

        std::expected<void, AccessError>
        set_register(std::uint32_t tid, std::string_view name, std::uint64_t value) override
        {
            const std::lock_guard lock(state_mutex_);
            calls_.emplace_back("set_register");
            register_writes_.emplace_back(tid, std::string(name), value);
            for (RegisterValue& entry : register_file_)
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
            std::optional<AccessError> failure;
            {
                const std::lock_guard lock(state_mutex_);
                calls_.emplace_back("set_software_breakpoint");
                software_calls_.emplace_back(slot, address, insert);
                failure = arm_error_;
            }
            if (failure)
            {
                return std::unexpected(*failure);
            }
            return {};
        }

        std::expected<void, AccessError> set_hardware_breakpoint(
            std::uint32_t slot, HardwareKind kind, std::uint64_t address, std::size_t size, bool insert) override
        {
            std::optional<AccessError> failure;
            {
                const std::lock_guard lock(state_mutex_);
                calls_.emplace_back("set_hardware_breakpoint");
                hardware_calls_.emplace_back(slot, kind, address, size, insert);
                failure = arm_error_;
            }
            if (failure)
            {
                return std::unexpected(*failure);
            }
            return {};
        }

        std::expected<std::vector<Frame>, AccessError> backtrace(std::uint32_t tid) override
        {
            (void)tid;
            const std::lock_guard lock(state_mutex_);
            calls_.emplace_back("backtrace");
            return frames_;
        }

        [[nodiscard]] bool supports_debug() const override
        {
            const std::lock_guard lock(state_mutex_);
            return supported_;
        }

    private:
        std::expected<StopEvent, AccessError> next_stop()
        {
            const std::lock_guard lock(state_mutex_);
            if (stop_replies_.empty())
            {
                return StopEvent {slopkit::debug::StopReason::single_step, 4242, 0x1001, 0x1001, std::nullopt, 0};
            }
            auto reply = std::move(stop_replies_.front());
            stop_replies_.pop_front();
            return reply;
        }

        mutable std::mutex         state_mutex_;
        std::vector<std::string>   calls_;
        std::optional<AccessError> attach_error_;
        std::optional<AccessError> arm_error_;
        bool                       supported_ {true};
        std::deque<StopReply>      stop_replies_;
        std::vector<RegisterValue> register_file_ {default_registers()};
        std::vector<Frame>         frames_ {
            {0x1000, 0}
        };

        std::optional<ProcessId>                                    attached_pid_;
        std::string                                                 attached_plugin_;
        std::uint64_t                                               last_resume_address_ {};
        std::size_t                                                 last_resume_step_size_ {};
        std::optional<std::uint32_t>                                last_step_tid_;
        std::optional<std::uint32_t>                                last_interrupt_tid_;
        std::vector<std::tuple<std::uint32_t, std::uint64_t, bool>> software_calls_;
        std::vector<std::tuple<std::uint32_t, HardwareKind, std::uint64_t, std::size_t, bool>> hardware_calls_;
        std::vector<std::tuple<std::uint32_t, std::string, std::uint64_t>>                     register_writes_;
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
