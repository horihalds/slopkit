#include "process/access_worker.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "platform/linux/desktop_entry.hpp"

namespace slopkit::process
{

    AccessWorker::AccessWorker(ProcessAccess& access)
        : access_(access), worker_(
                               [this](std::stop_token token)
                               {
                                   run(token);
                               })
    {
    }

    AccessWorker::~AccessWorker()
    {
        // Stop the worker and wait for the in-flight call before touching the
        // queues, so no callback can be invoked while its owner is destroyed and
        // no worker thread outlives the App.
        worker_.request_stop();
        cv_.notify_all();
        if (worker_.joinable())
        {
            worker_.join();
        }

        const std::lock_guard lock(mutex_);
        completions_.clear(); // Pending callbacks are dropped, never invoked.
        requests_.clear();
    }

    void AccessWorker::run(std::stop_token token)
    {
        while (!token.stop_requested())
        {
            Request request;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock,
                         [&]
                         {
                             return token.stop_requested() || !requests_.empty();
                         });
                if (token.stop_requested())
                {
                    break;
                }
                request = std::move(requests_.front());
                requests_.pop_front();
            }

            JobResult result = execute(request);

            // A stop request that arrived while the job ran drops the result
            // instead of delivering it after shutdown started.
            if (token.stop_requested())
            {
                break;
            }

            const std::lock_guard lock(mutex_);
            completions_.push_back(Completion {std::move(request.on_done), std::move(result)});
        }
    }

    bool AccessWorker::submit(Request request)
    {
        if (worker_.get_stop_token().stop_requested())
        {
            return false;
        }

        {
            const std::lock_guard lock(mutex_);
            requests_.push_back(std::move(request));
        }
        cv_.notify_one();
        return true;
    }

    std::size_t AccessWorker::drain(std::size_t max_jobs)
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

            // Run outside the lock, on the calling (UI) thread.
            if (completion.on_done)
            {
                completion.on_done(std::move(completion.result));
            }
            ++drained;
        }
        return drained;
    }

    bool AccessWorker::attached() const noexcept
    {
        return attached_.load();
    }

    JobId AccessWorker::next_job_id() noexcept
    {
        return next_id_.fetch_add(1);
    }

    bool AccessWorker::submit_list(JobId id, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::list;
        request.id      = id;
        request.on_done = std::move(on_done);
        return submit(std::move(request));
    }

    bool AccessWorker::submit_probe(JobId id, ProcessId pid, std::string plugin_id, JobCallback on_done)
    {
        Request request;
        request.kind      = JobKind::probe;
        request.id        = id;
        request.pid       = pid;
        request.plugin_id = std::move(plugin_id);
        request.on_done   = std::move(on_done);
        return submit(std::move(request));
    }

    bool AccessWorker::submit_attach_app(JobId id, ProcessId pid, std::string plugin_id, JobCallback on_done)
    {
        Request request;
        request.kind      = JobKind::attach_app;
        request.id        = id;
        request.pid       = pid;
        request.plugin_id = std::move(plugin_id);
        request.on_done   = std::move(on_done);
        return submit(std::move(request));
    }

    bool AccessWorker::submit_attach_handoff(JobId id, ProcessId pid, std::string plugin_id, JobCallback on_done)
    {
        Request request;
        request.kind      = JobKind::attach_handoff;
        request.id        = id;
        request.pid       = pid;
        request.plugin_id = std::move(plugin_id);
        request.on_done   = std::move(on_done);
        return submit(std::move(request));
    }

    bool AccessWorker::submit_application_index(JobId id, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::application_index;
        request.id      = id;
        request.on_done = std::move(on_done);
        return submit(std::move(request));
    }

    bool AccessWorker::submit_read(JobId id, std::uint64_t address, std::size_t size, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::read;
        request.id      = id;
        request.address = address;
        request.size    = size;
        request.on_done = std::move(on_done);
        return submit(std::move(request));
    }

    bool AccessWorker::submit_write(
        JobId id, std::uint64_t entry_id, std::uint64_t address, std::vector<std::byte> bytes, JobCallback on_done)
    {
        Request request;
        request.kind     = JobKind::write;
        request.id       = id;
        request.address  = address;
        request.entry_id = entry_id;
        request.bytes    = std::move(bytes);
        request.on_done  = std::move(on_done);
        return submit(std::move(request));
    }

    bool AccessWorker::submit_freeze(JobId id, std::vector<WriteItem> items, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::freeze;
        request.id      = id;
        request.items   = std::move(items);
        request.on_done = std::move(on_done);
        return submit(std::move(request));
    }

    bool AccessWorker::submit_detach(JobId id, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::detach;
        request.id      = id;
        request.on_done = std::move(on_done);
        return submit(std::move(request));
    }

    JobResult AccessWorker::execute(Request& request)
    {
        switch (request.kind)
        {
        case JobKind::list:
            return do_list();
        case JobKind::probe:
            return do_probe(request);
        case JobKind::attach_app:
            return do_attach(request, false);
        case JobKind::attach_handoff:
            return do_attach(request, true);
        case JobKind::application_index:
            return do_application_index();
        case JobKind::read:
            return do_read(request);
        case JobKind::write:
            return do_write(request);
        case JobKind::freeze:
            return do_freeze(request);
        case JobKind::detach:
            return do_detach();
        }
        return ListResult {};
    }

    ListResult AccessWorker::do_list()
    {
        ListResult result;
        if (auto listed = access_.list_processes())
        {
            result.processes = std::move(*listed);
        }
        else
        {
            result.error = listed.error();
        }
        return result;
    }

    ProbeResult AccessWorker::do_probe(const Request& request)
    {
        ProbeResult result;

        // The probe uses a throwaway session: it never becomes the app session.
        auto probe = access_.attach(request.pid, request.plugin_id);
        if (!probe)
        {
            result.error = probe.error();
            return result;
        }

        result.method = probe->advertised_methods();

        if (auto modules = probe->modules())
        {
            result.modules = modules->size();
        }
        else
        {
            result.modules_error = modules.error();
        }

        if (auto threads = probe->threads())
        {
            result.threads = threads->size();
        }

        return result;
    }

    AttachResult AccessWorker::do_attach(const Request& request, bool handoff)
    {
        AttachResult result;

        auto attached = access_.attach(request.pid, request.plugin_id);
        if (!attached)
        {
            result.error = attached.error();
            if (!handoff)
            {
                // A failed re-attach leaves no session behind, matching the old
                // synchronous attach that cleared the target first.
                session_.reset();
                attached_ = false;
            }
            return result;
        }

        if (handoff)
        {
            result.handed_session = std::move(*attached);
            return result;
        }

        result.info = AttachInfo {.pid       = request.pid,
                                  .plugin_id = std::string(attached->plugin_id()),
                                  .method    = attached->advertised_methods()};
        session_    = std::move(*attached);
        attached_   = true;
        return result;
    }

    AppIndexResult AccessWorker::do_application_index()
    {
        AppIndexResult result;
        result.executables = platform::scan_desktop_executables(platform::default_application_dirs());
        return result;
    }

    ReadResult AccessWorker::do_read(const Request& request)
    {
        ReadResult result;
        result.id      = request.id;
        result.address = request.address;

        if (!session_)
        {
            result.error = AccessError::internal;
            return result;
        }

        if (auto bytes = session_->read(request.address, request.size))
        {
            result.bytes = std::move(*bytes);
        }
        else
        {
            result.error = bytes.error();
        }
        return result;
    }

    WriteResult AccessWorker::do_write(const Request& request)
    {
        WriteResult result;
        result.id       = request.id;
        result.entry_id = request.entry_id;

        if (!session_)
        {
            result.error = AccessError::internal;
            return result;
        }

        if (auto written = session_->write(request.address, request.bytes); !written)
        {
            result.error = written.error();
        }
        return result;
    }

    FreezeResult AccessWorker::do_freeze(const Request& request)
    {
        FreezeResult result;

        if (!session_)
        {
            result.error = AccessError::internal;
            return result;
        }

        for (const auto& item : request.items)
        {
            if (auto written = session_->write(item.address, item.bytes); !written)
            {
                result.error = written.error();
                return result;
            }
            ++result.written;
        }
        return result;
    }

    AttachResult AccessWorker::do_detach()
    {
        session_.reset();
        attached_ = false;
        return AttachResult {};
    }

} // namespace slopkit::process
