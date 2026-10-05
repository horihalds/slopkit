#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "debug/backend.hpp"
#include "process/types.hpp"

namespace slopkit::debug
{

    using JobId = std::uint64_t;

    struct AttachResult
    {
        std::optional<process::AccessError> error;
    };

    struct VoidResult
    {
        std::optional<process::AccessError> error;
    };

    struct StopResult
    {
        std::optional<StopEvent>            stop;
        std::optional<process::AccessError> error;
    };

    struct RegistersResult
    {
        std::vector<RegisterValue>          values;
        std::optional<process::AccessError> error;
    };

    struct BacktraceResult
    {
        std::vector<Frame>                  frames;
        std::optional<process::AccessError> error;
    };

    using JobResult = std::variant<AttachResult, VoidResult, StopResult, RegistersResult, BacktraceResult>;

    // Runs on the UI thread inside Worker::drain().
    using JobCallback = std::move_only_function<void(JobResult&&)>;

    // Invoked on the worker thread after a completion is queued; the UI turns it
    // into a wake-up post. It must not touch a widget.
    using CompletionHook = std::move_only_function<void()>;

    // Owns every backend call. Every ptrace operation runs on the one job thread
    // that owns the ptrace relationship, because a tracee may only be ptraced by
    // the thread that attached it. `interrupt` therefore runs out-of-band on a
    // helper thread and stops the target with a signal instead of a ptrace call.
    class Worker
    {
    public:
        explicit Worker(DebugBackend& backend);
        ~Worker();

        Worker(const Worker&)            = delete;
        Worker& operator=(const Worker&) = delete;

        bool submit_attach(JobId id, process::ProcessId pid, std::string plugin_id, JobCallback on_done);
        bool submit_detach(JobId id, JobCallback on_done);
        bool submit_continue(JobId         id,
                             std::uint32_t tid,
                             std::uint64_t resume_address,
                             std::size_t   resume_step_size,
                             JobCallback   on_done);
        bool submit_step(JobId id, std::uint32_t tid, JobCallback on_done);
        bool submit_interrupt(JobId id, std::uint32_t tid, JobCallback on_done);
        bool submit_registers(JobId id, std::uint32_t tid, JobCallback on_done);
        bool
        submit_set_register(JobId id, std::uint32_t tid, std::string name, std::uint64_t value, JobCallback on_done);
        bool submit_software_breakpoint(
            JobId id, std::uint32_t slot, std::uint64_t address, bool insert, JobCallback on_done);
        bool submit_hardware_breakpoint(JobId         id,
                                        std::uint32_t slot,
                                        HardwareKind  kind,
                                        std::uint64_t address,
                                        std::size_t   size,
                                        bool          insert,
                                        JobCallback   on_done);
        bool submit_backtrace(JobId id, std::uint32_t tid, JobCallback on_done);

        // Invokes pending callbacks on the calling thread. Bounded.
        std::size_t drain(std::size_t max_jobs = 32);

        void set_completion_hook(CompletionHook hook);

        [[nodiscard]] JobId next_job_id() noexcept;

    private:
        enum class JobKind
        {
            attach,
            detach,
            cont,
            step,
            interrupt,
            registers,
            set_register,
            software_breakpoint,
            hardware_breakpoint,
            backtrace,
        };

        struct Request
        {
            JobKind            kind {};
            JobId              id {};
            process::ProcessId pid {};
            std::string        plugin_id;
            std::string        name;
            std::uint64_t      address {};
            std::uint64_t      value {};
            std::size_t        size {};
            std::uint32_t      slot {};
            std::uint32_t      tid {};
            HardwareKind       hardware_kind {HardwareKind::execute};
            bool               insert {false};
            JobCallback        on_done;
        };

        struct Completion
        {
            JobCallback on_done;
            JobResult   result;
        };

        void      run(std::stop_token token);
        void      start_run(Request request);
        JobResult execute(Request& request);
        bool      submit(Request request);
        void      complete(JobCallback on_done, JobResult result);

        DebugBackend&              backend_;
        std::mutex                 mutex_;
        std::condition_variable    cv_;
        std::deque<Request>        requests_;
        std::deque<Completion>     completions_;
        CompletionHook             completion_hook_;  // guarded by mutex_
        bool                       stopping_ {false}; // guarded by mutex_
        // True while a ptrace job runs on the job thread; the destructor uses it
        // to decide whether a stop signal must be sent before joining.
        std::atomic<bool>          ptrace_busy_ {false};
        // True while the out-of-band interrupt helper runs.
        std::atomic<bool>          interrupt_active_ {false};
        std::atomic<std::uint32_t> current_tid_ {0};
        std::atomic<JobId>         next_id_ {1};

        // Declared last: the threads started by the constructor touch the members
        // above before they can process a job.
        std::jthread worker_;
        std::jthread run_thread_;
    };

} // namespace slopkit::debug
