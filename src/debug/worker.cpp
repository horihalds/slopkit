#include "debug/worker.hpp"

#include <utility>

namespace slopkit::debug
{

    Worker::Worker(DebugBackend& backend) : backend_(backend)
    {
        worker_ = std::jthread(
            [this](std::stop_token token)
            {
                run(token);
            });
    }

    Worker::~Worker()
    {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        worker_.request_stop();
        cv_.notify_all();

        // A ptrace job may be blocked in the kernel; the stop signal is not a
        // ptrace call, so it can be sent from here to make the job return.
        if (ptrace_busy_.load())
        {
            const std::uint32_t tid = current_tid_.load();
            if (tid != 0)
            {
                (void)backend_.interrupt(tid);
            }
        }
        run_thread_.request_stop();
        if (run_thread_.joinable())
        {
            run_thread_.join();
        }
    }

    bool Worker::submit_attach(JobId id, process::ProcessId pid, std::string plugin_id, JobCallback on_done)
    {
        Request request;
        request.kind      = JobKind::attach;
        request.id        = id;
        request.pid       = pid;
        request.plugin_id = std::move(plugin_id);
        request.on_done   = std::move(on_done);
        return submit(std::move(request));
    }

    bool Worker::submit_detach(JobId id, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::detach;
        request.id      = id;
        request.on_done = std::move(on_done);
        return submit(std::move(request));
    }

    bool Worker::submit_continue(
        JobId id, std::uint32_t tid, std::uint64_t resume_address, std::size_t resume_step_size, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::cont;
        request.id      = id;
        request.tid     = tid;
        request.address = resume_address;
        request.size    = resume_step_size;
        request.on_done = std::move(on_done);
        current_tid_.store(tid);
        return submit(std::move(request));
    }

    bool Worker::submit_step(JobId id, std::uint32_t tid, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::step;
        request.id      = id;
        request.tid     = tid;
        request.on_done = std::move(on_done);
        current_tid_.store(tid);
        return submit(std::move(request));
    }

    bool Worker::submit_interrupt(JobId id, std::uint32_t tid, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::interrupt;
        request.id      = id;
        request.tid     = tid;
        request.on_done = std::move(on_done);
        current_tid_.store(tid);

        // Not queued: while a ptrace job blocks the job thread the queue cannot
        // be serviced, and the interrupt must still reach the target.
        start_run(std::move(request));
        return true;
    }

    bool Worker::submit_registers(JobId id, std::uint32_t tid, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::registers;
        request.id      = id;
        request.tid     = tid;
        request.on_done = std::move(on_done);
        return submit(std::move(request));
    }

    bool
    Worker::submit_set_register(JobId id, std::uint32_t tid, std::string name, std::uint64_t value, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::set_register;
        request.id      = id;
        request.tid     = tid;
        request.name    = std::move(name);
        request.value   = value;
        request.on_done = std::move(on_done);
        return submit(std::move(request));
    }

    bool Worker::submit_software_breakpoint(
        JobId id, std::uint32_t slot, std::uint64_t address, bool insert, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::software_breakpoint;
        request.id      = id;
        request.slot    = slot;
        request.address = address;
        request.insert  = insert;
        request.on_done = std::move(on_done);
        return submit(std::move(request));
    }

    bool Worker::submit_hardware_breakpoint(JobId         id,
                                            std::uint32_t slot,
                                            HardwareKind  kind,
                                            std::uint64_t address,
                                            std::size_t   size,
                                            bool          insert,
                                            JobCallback   on_done)
    {
        Request request;
        request.kind          = JobKind::hardware_breakpoint;
        request.id            = id;
        request.slot          = slot;
        request.hardware_kind = kind;
        request.address       = address;
        request.size          = size;
        request.insert        = insert;
        request.on_done       = std::move(on_done);
        return submit(std::move(request));
    }

    bool Worker::submit_backtrace(JobId id, std::uint32_t tid, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::backtrace;
        request.id      = id;
        request.tid     = tid;
        request.on_done = std::move(on_done);
        return submit(std::move(request));
    }

    std::size_t Worker::drain(std::size_t max_jobs)
    {
        std::size_t drained = 0;
        while (drained < max_jobs)
        {
            Completion completion;
            {
                const std::lock_guard lock(mutex_);
                if (completions_.empty())
                {
                    break;
                }
                completion = std::move(completions_.front());
                completions_.pop_front();
            }
            completion.on_done(std::move(completion.result));
            ++drained;
        }
        return drained;
    }

    void Worker::set_completion_hook(CompletionHook hook)
    {
        const std::lock_guard lock(mutex_);
        completion_hook_ = std::move(hook);
    }

    JobId Worker::next_job_id() noexcept
    {
        return next_id_++;
    }

    void Worker::run(std::stop_token token)
    {
        for (;;)
        {
            Request request;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock,
                         [this, &token]
                         {
                             return stopping_ || token.stop_requested() || !requests_.empty();
                         });
                if ((stopping_ || token.stop_requested()) && requests_.empty())
                {
                    return;
                }
                request = std::move(requests_.front());
                requests_.pop_front();
            }

            // The interrupt never reaches this queue; every queued job is a
            // ptrace job and runs here, on the one thread that owns the ptrace
            // relationship.
            JobCallback on_done = std::move(request.on_done);
            ptrace_busy_        = true;
            JobResult result    = execute(request);
            ptrace_busy_        = false;
            complete(std::move(on_done), std::move(result));
        }
    }

    void Worker::start_run(Request request)
    {
        if (interrupt_active_.load())
        {
            // A second interrupt while one is in flight is dropped.
            complete(std::move(request.on_done), VoidResult {});
            return;
        }
        if (run_thread_.joinable())
        {
            run_thread_.join();
        }

        interrupt_active_ = true;
        run_thread_       = std::jthread(
            [this, request = std::move(request)](std::stop_token) mutable
            {
                JobCallback on_done = std::move(request.on_done);
                JobResult   result  = execute(request);
                interrupt_active_   = false;
                complete(std::move(on_done), std::move(result));
            });
    }

    JobResult Worker::execute(Request& request)
    {
        switch (request.kind)
        {
        case JobKind::attach:
        {
            AttachResult result;
            if (const auto attached = backend_.attach(request.pid, request.plugin_id); !attached)
            {
                result.error = attached.error();
            }
            return result;
        }
        case JobKind::detach:
        {
            VoidResult result;
            if (const auto detached = backend_.detach(); !detached)
            {
                result.error = detached.error();
            }
            return result;
        }
        case JobKind::cont:
        {
            StopResult result;
            if (auto stop = backend_.cont(request.address, request.size); stop)
            {
                result.stop = std::move(*stop);
            }
            else
            {
                result.error = stop.error();
            }
            return result;
        }
        case JobKind::step:
        {
            StopResult result;
            if (auto stop = backend_.step(request.tid); stop)
            {
                result.stop = std::move(*stop);
            }
            else
            {
                result.error = stop.error();
            }
            return result;
        }
        case JobKind::interrupt:
        {
            VoidResult result;
            if (const auto interrupted = backend_.interrupt(request.tid); !interrupted)
            {
                result.error = interrupted.error();
            }
            return result;
        }
        case JobKind::registers:
        {
            RegistersResult result;
            if (auto values = backend_.registers(request.tid); values)
            {
                result.values = std::move(*values);
            }
            else
            {
                result.error = values.error();
            }
            return result;
        }
        case JobKind::set_register:
        {
            VoidResult result;
            if (const auto written = backend_.set_register(request.tid, request.name, request.value); !written)
            {
                result.error = written.error();
            }
            return result;
        }
        case JobKind::software_breakpoint:
        {
            VoidResult result;
            if (const auto armed = backend_.set_software_breakpoint(request.slot, request.address, request.insert);
                !armed)
            {
                result.error = armed.error();
            }
            return result;
        }
        case JobKind::hardware_breakpoint:
        {
            VoidResult result;
            if (const auto armed = backend_.set_hardware_breakpoint(
                    request.slot, request.hardware_kind, request.address, request.size, request.insert);
                !armed)
            {
                result.error = armed.error();
            }
            return result;
        }
        case JobKind::backtrace:
        {
            BacktraceResult result;
            if (auto frames = backend_.backtrace(request.tid); frames)
            {
                result.frames = std::move(*frames);
            }
            else
            {
                result.error = frames.error();
            }
            return result;
        }
        }
        return VoidResult {process::AccessError::internal};
    }

    bool Worker::submit(Request request)
    {
        {
            const std::lock_guard lock(mutex_);
            if (stopping_)
            {
                return false;
            }
            requests_.push_back(std::move(request));
        }
        cv_.notify_one();
        return true;
    }

    void Worker::complete(JobCallback on_done, JobResult result)
    {
        const std::lock_guard lock(mutex_);
        completions_.push_back(Completion {std::move(on_done), std::move(result)});
        if (completion_hook_)
        {
            completion_hook_();
        }
    }

} // namespace slopkit::debug
