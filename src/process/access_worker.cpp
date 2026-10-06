#include "process/access_worker.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "platform/linux/desktop_entry.hpp"

namespace slopkit::process
{

    namespace
    {
        // Reads one byte from the target to learn whether its memory is actually
        // reachable: an attach and the process metadata can succeed while every
        // read is refused (e.g. Yama denies a target slopkit is not an ancestor
        // of). Returns the reason on failure, or nullopt when the read succeeds
        // or there is no address worth probing.
        std::optional<AccessError> check_memory_access(Session& session, std::span<const ModuleInfo> modules)
        {
            std::uint64_t address = 0;

            const ModuleInfo* main = main_module(modules);
            if (main != nullptr && main->base != 0)
            {
                address = main->base;
            }
            else if (const auto regions = session.regions(); regions.has_value())
            {
                const auto readable = std::ranges::find_if(*regions,
                                                           [](const RegionInfo& region)
                                                           {
                                                               return region.readable;
                                                           });
                if (readable != regions->end())
                {
                    address = readable->start;
                }
            }

            if (address == 0)
            {
                return std::nullopt;
            }

            if (const auto read = session.read(address, 1); !read)
            {
                return read.error();
            }
            return std::nullopt;
        }

        // Decodes a little-endian pointer of at most eight bytes; a shorter read
        // is padded with zeroes.
        std::uint64_t decode_pointer(std::span<const std::byte> bytes)
        {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < bytes.size() && i < sizeof(value); ++i)
            {
                value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[i])) << (8 * i);
            }
            return value;
        }
    } // namespace

    AccessWorker::AccessWorker(ProcessAccess& access) : access_(access)
    {
        // A std::jthread may run its start routine before its constructor
        // returns, so run() must not be reachable while the members it touches
        // (mutex_, cv_, requests_, completion_hook_) are still being built.
        worker_ = std::jthread {[this](std::stop_token token)
                                {
                                    run(token);
                                }};
        log::debug(log::category::process, "access worker started");
    }

    AccessWorker::~AccessWorker()
    {
        // Stop the worker and wait for the in-flight call before touching the
        // queues, so no callback can be invoked while its owner is destroyed and
        // no worker thread outlives the App.
        worker_.request_stop();
        {
            // stopping_ is what the wait predicate reads, so it is published
            // under the mutex that guards it, like submit() does with the queue.
            const std::lock_guard lock(mutex_);
            stopping_ = true;
            cv_.notify_all();
        }
        if (worker_.joinable())
        {
            worker_.join();
        }

        const std::lock_guard lock(mutex_);
        completions_.clear(); // Pending callbacks are dropped, never invoked.
        requests_.clear();
        completion_hook_ = nullptr;

        log::debug(log::category::process, "access worker stopped");
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
                             return stopping_ || !requests_.empty();
                         });
                if (stopping_)
                {
                    break;
                }
                request = std::move(requests_.front());
                requests_.pop_front();
            }

            log::debug(log::category::process,
                       std::format("executing job {} ({})", request.id, job_kind_name(request.kind)));

            JobResult result = execute(request);

            log::debug(log::category::process,
                       std::format("completed job {} ({})", request.id, job_kind_name(request.kind)));

            // A stop request that arrived while the job ran drops the result
            // instead of delivering it after shutdown started.
            if (token.stop_requested())
            {
                break;
            }

            const std::lock_guard lock(mutex_);
            completions_.push_back(Completion {std::move(request.on_done), std::move(result)});
            if (completion_hook_)
            {
                // Worker thread: the hook only posts a wake-up, it never touches
                // the UI.
                completion_hook_();
            }
        }
    }

    bool AccessWorker::submit(Request request)
    {
        {
            const std::lock_guard lock(mutex_);
            if (stopping_)
            {
                return false;
            }
            requests_.push_back(std::move(request));
        }
        log::debug(log::category::process, std::format("queued job {} ({})", request.id, job_kind_name(request.kind)));
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
        if (drained > 0)
        {
            log::debug(log::category::process, std::format("drained {} completion(s)", drained));
        }
        return drained;
    }

    void AccessWorker::set_completion_hook(CompletionHook hook)
    {
        const std::lock_guard lock(mutex_);
        completion_hook_ = std::move(hook);
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

    bool AccessWorker::submit_memory_map(JobId id, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::memory_map;
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

    bool AccessWorker::submit_read_many(JobId id, std::vector<ReadManyItem> items, JobCallback on_done)
    {
        // The live pass batches a bounded set of displayed values; a runaway
        // list is a programming error, so refuse it instead of monopolising the
        // worker.
        constexpr std::size_t kMaxItems = 1024;
        if (items.size() > kMaxItems)
        {
            log::warning(log::category::process,
                         std::format("batched read of {} item(s) rejected (limit {})", items.size(), kMaxItems));
            return false;
        }

        Request request;
        request.kind       = JobKind::read_many;
        request.id         = id;
        request.read_items = std::move(items);
        request.on_done    = std::move(on_done);
        return submit(std::move(request));
    }

    bool AccessWorker::submit_resolve_expressions(JobId                        id,
                                                  std::vector<ResolveRequest>  requests,
                                                  std::vector<expr::ModuleRef> modules,
                                                  std::size_t                  pointer_size,
                                                  JobCallback                  on_done)
    {
        // The re-resolution pass batches a bounded set of displayed entries; a
        // runaway list is a programming error, so refuse it instead of
        // monopolising the worker.
        constexpr std::size_t kMaxItems = 1024;
        if (requests.size() > kMaxItems)
        {
            log::warning(log::category::process,
                         std::format("batched resolve of {} item(s) rejected (limit {})", requests.size(), kMaxItems));
            return false;
        }

        Request request;
        request.kind          = JobKind::resolve_expressions;
        request.id            = id;
        request.resolve_items = std::move(requests);
        request.module_refs   = std::move(modules);
        request.pointer_size  = pointer_size;
        request.on_done       = std::move(on_done);
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

    std::string_view AccessWorker::job_kind_name(JobKind kind) noexcept
    {
        switch (kind)
        {
        case JobKind::list:
            return "list";
        case JobKind::probe:
            return "probe";
        case JobKind::attach_app:
            return "attach";
        case JobKind::attach_handoff:
            return "attach-handoff";
        case JobKind::application_index:
            return "application-index";
        case JobKind::memory_map:
            return "memory-map";
        case JobKind::read:
            return "read";
        case JobKind::read_many:
            return "read-many";
        case JobKind::write:
            return "write";
        case JobKind::freeze:
            return "freeze";
        case JobKind::detach:
            return "detach";
        case JobKind::resolve_expressions:
            return "resolve-expressions";
        }
        return "unknown";
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
        case JobKind::memory_map:
            return do_memory_map();
        case JobKind::read:
            return do_read(request);
        case JobKind::read_many:
            return do_read_many(request);
        case JobKind::write:
            return do_write(request);
        case JobKind::freeze:
            return do_freeze(request);
        case JobKind::detach:
            return do_detach();
        case JobKind::resolve_expressions:
            return do_resolve(request);
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
            log::warning(log::category::process, std::format("listing processes failed: {}", describe(*result.error)));
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
            log::warning(
                log::category::process,
                std::format(
                    "probe of pid {} via {} failed: {}", request.pid, request.plugin_id, describe(*result.error)));
            return result;
        }

        result.method = probe->advertised_methods();

        if (auto modules = probe->modules())
        {
            result.modules = modules->size();

            if (const auto read_error = check_memory_access(*probe, *modules))
            {
                result.read_error = read_error;
                log::warning(log::category::process,
                             std::format("probe of pid {} cannot read memory via {}: {}",
                                         request.pid,
                                         describe(result.method),
                                         describe(*read_error)));
            }
        }
        else
        {
            result.modules_error = modules.error();
            log::warning(log::category::process,
                         std::format("probe of pid {} could not list modules: {}",
                                     request.pid,
                                     describe(*result.modules_error)));
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
            log::warning(
                log::category::process,
                std::format(
                    "attach to pid {} via {} failed: {}", request.pid, request.plugin_id, describe(*result.error)));
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

        AttachInfo info {.pid        = request.pid,
                         .plugin_id  = std::string(attached->plugin_id()),
                         .method     = attached->advertised_methods(),
                         .read_error = std::nullopt};

        if (const auto modules = attached->modules(); modules.has_value())
        {
            if (const auto read_error = check_memory_access(*attached, *modules))
            {
                info.read_error = read_error;
                log::warning(log::category::process,
                             std::format("attach to pid {} via {} cannot read memory: {}",
                                         request.pid,
                                         info.plugin_id,
                                         describe(*read_error)));
            }
        }

        result.info = std::move(info);
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
            log::warning(log::category::process,
                         std::format("read at {:X} requested without an attached target", request.address));
            return result;
        }

        if (auto bytes = session_->read(request.address, request.size))
        {
            result.bytes = std::move(*bytes);
        }
        else
        {
            result.error = bytes.error();
            log::debug(
                log::category::process,
                std::format(
                    "read of {} byte(s) at {:X} failed: {}", request.size, request.address, describe(*result.error)));
        }
        return result;
    }

    ReadManyResult AccessWorker::do_read_many(const Request& request)
    {
        ReadManyResult result;
        result.items.reserve(request.read_items.size());

        if (!session_)
        {
            // No session at all: every requested address is unreadable, but the
            // batch still completes so the caller's in-flight guard clears.
            result.items.assign(request.read_items.size(), std::unexpected(AccessError::internal));
            log::warning(log::category::process,
                         std::format("batched read of {} address(es) requested without an attached target",
                                     request.read_items.size()));
            return result;
        }

        std::size_t failed = 0;
        for (const auto& item : request.read_items)
        {
            std::vector<std::byte> bytes(item.size);
            if (auto read = session_->read_into(item.address, bytes))
            {
                if (*read < bytes.size())
                {
                    // A short read still carries usable bytes; keep only what
                    // the backend actually produced.
                    bytes.resize(*read);
                }
                result.items.emplace_back(std::move(bytes));
            }
            else
            {
                ++failed;
                result.items.emplace_back(std::unexpected(read.error()));
                log::debug(
                    log::category::process,
                    std::format(
                        "live read of {} byte(s) at {:X} failed: {}", item.size, item.address, describe(read.error())));
            }
        }

        log::debug(
            log::category::process,
            std::format("batched read of {} address(es) completed, {} failed", request.read_items.size(), failed));
        return result;
    }

    MemoryMapResult AccessWorker::do_memory_map()
    {
        MemoryMapResult result;

        if (!session_)
        {
            result.error = AccessError::internal;
            log::warning(log::category::process, "memory map requested without an attached target");
            return result;
        }

        if (auto modules = session_->modules())
        {
            result.modules = std::move(*modules);
        }
        else
        {
            result.error = modules.error();
            log::warning(log::category::process, std::format("could not list modules: {}", describe(*result.error)));
        }

        if (auto regions = session_->regions())
        {
            result.regions = std::move(*regions);
        }
        else if (!result.error)
        {
            result.error = regions.error();
            log::warning(log::category::process, std::format("could not list regions: {}", describe(*result.error)));
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
            log::warning(log::category::process, "write requested without an attached target");
            return result;
        }

        if (auto written = session_->write(request.address, request.bytes); !written)
        {
            result.error = written.error();
            log::warning(log::category::process,
                         std::format("write of {} byte(s) at {:X} failed: {}",
                                     request.bytes.size(),
                                     request.address,
                                     describe(*result.error)));
        }
        return result;
    }

    FreezeResult AccessWorker::do_freeze(const Request& request)
    {
        FreezeResult result;

        if (!session_)
        {
            result.error = AccessError::internal;
            log::warning(log::category::process, "freeze pass requested without an attached target");
            return result;
        }

        for (const auto& item : request.items)
        {
            if (auto written = session_->write(item.address, item.bytes); !written)
            {
                result.error = written.error();
                log::warning(log::category::process,
                             std::format("freeze pass failed writing {} byte(s) at {:X}: {}",
                                         item.bytes.size(),
                                         item.address,
                                         describe(*result.error)));
                return result;
            }
            ++result.written;
        }

        log::debug(log::category::process,
                   std::format("freeze pass wrote {} of {} value(s)", result.written, request.items.size()));
        return result;
    }

    AttachResult AccessWorker::do_detach()
    {
        session_.reset();
        attached_ = false;
        return AttachResult {};
    }

    ResolveResult AccessWorker::do_resolve(const Request& request)
    {
        ResolveResult result;

        if (!session_)
        {
            // No session at all: the whole batch fails so the caller's in-flight
            // guard clears, but nothing is read.
            result.error = AccessError::internal;
            log::warning(log::category::process,
                         std::format("resolve of {} expression(s) requested without an attached target",
                                     request.resolve_items.size()));
            return result;
        }

        const std::size_t pointer_size = request.pointer_size == 0 ? 8 : request.pointer_size;
        const auto reader = [this, pointer_size](std::uint64_t address) -> std::expected<std::uint64_t, std::string>
        {
            const auto bytes = session_->read(address, pointer_size);
            if (!bytes)
            {
                return std::unexpected(std::string {describe(bytes.error())});
            }
            return decode_pointer(*bytes);
        };

        result.items.reserve(request.resolve_items.size());
        std::size_t failed = 0;
        for (const auto& item : request.resolve_items)
        {
            ResolveItemResult entry;
            entry.key = item.key;

            const auto expression = expr::parse(item.expression);
            if (!expression)
            {
                entry.error = expression.error().message;
                ++failed;
                result.items.push_back(std::move(entry));
                continue;
            }

            const auto resolved =
                expr::evaluate(*expression, request.module_refs, reader, expr::Options {.pointer_size = pointer_size});
            if (resolved)
            {
                entry.address = *resolved;
            }
            else
            {
                entry.error = resolved.error().message;
                ++failed;
            }
            result.items.push_back(std::move(entry));
        }

        log::debug(log::category::process,
                   std::format("resolved {} expression(s), {} failed", request.resolve_items.size(), failed));
        return result;
    }

} // namespace slopkit::process
