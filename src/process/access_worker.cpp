#include "process/access_worker.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
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

        // The name a region belongs to: the loaded module whose image contains
        // the region's start, else the filename of the region's backing file. An
        // anonymous mapping has no name.
        std::string module_name_for(const RegionInfo& region, std::span<const ModuleInfo> modules)
        {
            const auto owner =
                std::ranges::find_if(modules,
                                     [&region](const ModuleInfo& module)
                                     {
                                         return module.base <= region.start && region.start - module.base < module.size;
                                     });
            if (owner != modules.end() && !owner->name.empty())
            {
                return owner->name;
            }
            if (!region.path.empty())
            {
                return std::filesystem::path(region.path).filename().string();
            }
            return {};
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

        // A log record is one line (docs/LOGGING.md), so a printed string or a
        // Lua error that carries a newline is flattened before it is logged.
        std::string one_line(std::string_view text)
        {
            std::string line {text};
            std::ranges::replace(line, '\n', ' ');
            std::ranges::replace(line, '\r', ' ');
            return line;
        }

        // The interesting part of a Lua error: its message, without the
        // traceback that follows it.
        std::string error_message(std::string_view text)
        {
            return one_line(text.substr(0, text.find('\n')));
        }
    } // namespace

    AccessWorker::AccessWorker(ProcessAccess& access, script::SymbolTable& symbols) : access_(access), symbols_(symbols)
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

        // Both loop exits (the stopping_ check at the top and the mid-job
        // break) land here, so the hooks always run before the thread returns
        // and the destructor's join makes the caller's exit wait for them.
        deactivate_active_scripts();
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

    bool AccessWorker::submit_script(JobId       id,
                                     std::string description,
                                     std::string chunk,
                                     std::string function,
                                     std::size_t pointer_size,
                                     JobCallback on_done)
    {
        Request request;
        request.kind         = JobKind::script;
        request.id           = id;
        request.pointer_size = pointer_size;
        request.description  = std::move(description);
        request.chunk        = std::move(chunk);
        request.function     = std::move(function);
        request.on_done      = std::move(on_done);
        return submit(std::move(request));
    }

    bool AccessWorker::submit_suspend(JobId id, ProcessId pid, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::suspend;
        request.id      = id;
        request.pid     = pid;
        request.on_done = std::move(on_done);
        return submit(std::move(request));
    }

    bool AccessWorker::submit_resume(JobId id, ProcessId pid, JobCallback on_done)
    {
        Request request;
        request.kind    = JobKind::resume;
        request.id      = id;
        request.pid     = pid;
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
        case JobKind::script:
            return "script";
        case JobKind::suspend:
            return "suspend";
        case JobKind::resume:
            return "resume";
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
        case JobKind::script:
            return do_script(request);
        case JobKind::suspend:
            return do_suspend(request);
        case JobKind::resume:
            return do_resume(request);
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
                engine_.reset();
                active_scripts_.clear();
                attached_ = false;
            }
            return result;
        }

        AttachInfo info {.pid         = request.pid,
                         .plugin_id   = std::string(attached->plugin_id()),
                         .method      = attached->advertised_methods(),
                         .can_suspend = attached->supports_suspend(),
                         .read_error  = std::nullopt};

        if (handoff)
        {
            // The panel takes over the session and reads the target itself, but
            // it still needs the metadata, notably the suspend capability.
            result.info           = std::move(info);
            result.handed_session = std::move(*attached);
            return result;
        }

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

        result.info          = std::move(info);
        session_             = std::move(*attached);
        // One engine per session: its Lua state, globals included, is reused by
        // every run against this target and dropped with the session.
        script_pointer_size_ = 8;
        engine_.emplace(memory_api(), symbol_api());
        active_scripts_.clear();
        attached_ = true;
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

    script::MemoryApi AccessWorker::memory_api()
    {
        script::MemoryApi api;
        api.pointer_size = [this]()
        {
            return script_pointer_size_ == 0 ? sizeof(void*) : script_pointer_size_;
        };
        // The lambdas read the worker's session rather than capture it, so they
        // never outlive it: a script only ever runs inside a script job, where
        // the session is present.
        api.read = [this](std::uint64_t address, std::size_t size) -> std::expected<std::vector<std::byte>, std::string>
        {
            if (!session_)
            {
                return std::unexpected(std::string {"no target is attached"});
            }
            auto bytes = session_->read(address, size);
            if (!bytes)
            {
                return std::unexpected(std::string {describe(bytes.error())});
            }
            return std::move(*bytes);
        };
        api.write = [this](std::uint64_t address, std::span<const std::byte> data) -> std::expected<void, std::string>
        {
            if (!session_)
            {
                return std::unexpected(std::string {"no target is attached"});
            }
            if (auto written = session_->write(address, data); !written)
            {
                return std::unexpected(std::string {describe(written.error())});
            }
            return {};
        };
        // The region list `aobscan` walks. Module names are best-effort: a target
        // that cannot list modules still scans, its regions just carry no name.
        api.regions = [this]() -> std::expected<std::vector<script::MemoryRegion>, std::string>
        {
            if (!session_)
            {
                return std::unexpected(std::string {"no target is attached"});
            }
            const std::expected<std::vector<RegionInfo>, AccessError> listed = session_->regions();
            if (!listed)
            {
                return std::unexpected(std::string {describe(listed.error())});
            }

            std::vector<ModuleInfo> modules;
            if (const std::expected<std::vector<ModuleInfo>, AccessError> found = session_->modules())
            {
                modules = *found;
                std::ranges::sort(modules, {}, &ModuleInfo::base);
            }

            std::vector<script::MemoryRegion> regions;
            regions.reserve(listed->size());
            for (const RegionInfo& region : *listed)
            {
                script::MemoryRegion entry;
                entry.base     = region.start;
                entry.size     = region.end > region.start ? region.end - region.start : 0;
                entry.readable = region.readable;
                entry.module   = module_name_for(region, modules);
                regions.push_back(std::move(entry));
            }
            return regions;
        };
        // Target allocation, from the plugin ABI 1.6 operations. A session whose
        // plugin leaves the slots null reports `unsupported` here.
        api.allocate = [this](std::size_t size, std::uint64_t near) -> std::expected<std::uint64_t, std::string>
        {
            if (!session_)
            {
                return std::unexpected(std::string {"no target is attached"});
            }
            auto address = session_->allocate(size, near);
            if (!address)
            {
                if (address.error() == AccessError::unsupported)
                {
                    return std::unexpected(std::string {"the target's plugin cannot allocate memory"});
                }
                return std::unexpected(std::string {describe(address.error())});
            }
            log::debug(log::category::script,
                       std::format("allocated {} byte(s) at {:#x} (near {:#x})", size, *address, near));
            return *address;
        };
        api.deallocate = [this](std::uint64_t address) -> std::expected<void, std::string>
        {
            if (!session_)
            {
                return std::unexpected(std::string {"no target is attached"});
            }
            if (auto freed = session_->free(address); !freed)
            {
                if (freed.error() == AccessError::unsupported)
                {
                    return std::unexpected(std::string {"the target's plugin cannot allocate memory"});
                }
                if (freed.error() == AccessError::not_found)
                {
                    return std::unexpected(std::string {"not this session's allocation"});
                }
                return std::unexpected(std::string {describe(freed.error())});
            }
            log::debug(log::category::script, std::format("freed allocation at {:#x}", address));
            return {};
        };
        // The module snapshot `expression` resolves module names against, in the
        // same shape the resolve job passes to `expr::evaluate`.
        api.modules = [this]() -> std::expected<std::vector<expr::ModuleRef>, std::string>
        {
            if (!session_)
            {
                return std::unexpected(std::string {"no target is attached"});
            }
            const std::expected<std::vector<ModuleInfo>, AccessError> found = session_->modules();
            if (!found)
            {
                return std::unexpected(std::string {describe(found.error())});
            }
            std::vector<expr::ModuleRef> modules;
            modules.reserve(found->size());
            for (const ModuleInfo& module : *found)
            {
                modules.push_back(expr::ModuleRef {module.name, module.base});
            }
            return modules;
        };
        return api;
    }

    script::SymbolApi AccessWorker::symbol_api()
    {
        script::SymbolApi api = symbols_.api();
        // Wrap the table's closures so each write is traced under the `script`
        // category; a rejected name is reported by the engine, not here.
        script::SymbolApi logging;
        logging.set = [inner = api.set](std::string_view name, std::uint64_t value) -> std::expected<void, std::string>
        {
            auto result = inner(name, value);
            if (result)
            {
                log::debug(log::category::script, std::format("symbol '{}' = {:#x}", name, value));
            }
            return result;
        };
        logging.remove = [inner = api.remove](std::string_view name) -> std::expected<void, std::string>
        {
            auto result = inner(name);
            if (result)
            {
                log::debug(log::category::script, std::format("symbol '{}' removed", name));
            }
            return result;
        };
        // A read, not a mutation, so it is passed through without a log record.
        logging.lookup = [inner = api.lookup](std::string_view name) -> std::optional<std::uint64_t>
        {
            return inner(name);
        };
        return logging;
    }

    ScriptResult AccessWorker::do_script(const Request& request)
    {
        ScriptResult result;
        result.id          = request.id;
        result.description = request.description;

        // Refuse before running anything when there is no target, so a script
        // can never touch a stale session. The caller turns this into the
        // message it shows and logs.
        if (!session_ || !engine_)
        {
            if (request.function.empty())
            {
                result.run.error = "no target is attached";
            }
            else
            {
                result.lifecycle.emplace();
                result.lifecycle->error = "no target is attached";
            }
            log::debug(log::category::process,
                       std::format("script '{}' refused: no attached target", request.description));
            return result;
        }

        script_pointer_size_ = request.pointer_size == 0 ? sizeof(void*) : request.pointer_size;

        if (request.function.empty())
        {
            log::debug(log::category::process,
                       std::format("running script '{}' ({} byte(s))", request.description, request.chunk.size()));
            result.run = engine_->run(request.chunk);
            log::debug(
                log::category::process,
                std::format("script '{}' finished: {}", request.description, result.run.ok ? "ok" : result.run.error));
            return result;
        }

        log::debug(log::category::process,
                   std::format("running script '{}' hook '{}' ({} byte(s))",
                               request.description,
                               request.function,
                               request.chunk.size()));
        result.lifecycle = engine_->run_lifecycle(request.chunk, request.function);
        remember_lifecycle(request, result);
        log::debug(log::category::process,
                   std::format("script '{}' hook '{}' finished: {}",
                               request.description,
                               request.function,
                               result.lifecycle->ok ? "ok" : result.lifecycle->error));
        return result;
    }

    void AccessWorker::remember_lifecycle(const Request& request, const ScriptResult& result)
    {
        // A plain run or a refusal leaves the tracked set alone, so it mirrors
        // the Active checkbox: only an accepted hook changes it.
        if (!result.lifecycle.has_value() || !result.lifecycle->ok)
        {
            return;
        }

        if (request.function == script::kActivateHook)
        {
            active_scripts_.push_back(ActiveScript {.description = request.description, .chunk = request.chunk});
        }
        else if (request.function == script::kDeactivateHook)
        {
            std::erase_if(active_scripts_,
                          [&](const ActiveScript& script)
                          {
                              return script.description == request.description;
                          });
        }
    }

    void AccessWorker::deactivate_active_scripts()
    {
        if (!engine_ || active_scripts_.empty())
        {
            return;
        }

        // The verdict cannot be acted on: nobody is left to follow a refusal
        // and the exit must not be delayed by trying again.
        std::vector<ActiveScript> pending = std::move(active_scripts_);
        active_scripts_.clear();

        for (const ActiveScript& script : pending)
        {
            log::debug(log::category::process,
                       std::format("deactivating script '{}' before shutdown", script.description));
            const script::LifecycleResult outcome =
                engine_->run_lifecycle(script.chunk, std::string {script::kDeactivateHook});
            for (const std::string& line : outcome.output)
            {
                log::info(log::category::script, one_line(line));
            }
            if (outcome.ok)
            {
                log::info(log::category::script,
                          std::format("script '{}' deactivated: {}",
                                      script.description,
                                      outcome.message.empty() ? "ok" : outcome.message));
                continue;
            }
            std::string reason = outcome.message;
            if (reason.empty())
            {
                reason = error_message(outcome.error);
            }
            if (reason.empty())
            {
                log::warning(log::category::script, std::format("script '{}' deactivate failed", script.description));
            }
            else
            {
                log::warning(log::category::script,
                             std::format("script '{}' deactivate failed: {}", script.description, reason));
            }
        }
    }

    SuspendResult AccessWorker::do_suspend(const Request& request)
    {
        SuspendResult result;

        // Refuse without touching the target when there is no session or the
        // worker's session is not the pid the caller prepared the job for: a
        // target replaced between the click and the job must not be stopped.
        if (!session_)
        {
            result.error = AccessError::unsupported;
            log::debug(log::category::process, "suspend requested without an attached target");
            return result;
        }
        if (session_->pid() != request.pid)
        {
            result.error = AccessError::not_found;
            log::debug(
                log::category::process,
                std::format("suspend refused: the attached target is pid {}, not {}", session_->pid(), request.pid));
            return result;
        }
        if (!session_->supports_suspend())
        {
            result.error = AccessError::unsupported;
            log::debug(log::category::process, "suspend refused: the plugin cannot suspend its target");
            return result;
        }

        if (auto suspended = session_->suspend(); !suspended)
        {
            result.error = suspended.error();
            log::debug(log::category::process,
                       std::format("suspend of pid {} failed: {}", request.pid, describe(*result.error)));
        }
        return result;
    }

    SuspendResult AccessWorker::do_resume(const Request& request)
    {
        SuspendResult result;

        if (!session_)
        {
            result.error = AccessError::unsupported;
            log::debug(log::category::process, "resume requested without an attached target");
            return result;
        }
        if (session_->pid() != request.pid)
        {
            result.error = AccessError::not_found;
            log::debug(
                log::category::process,
                std::format("resume refused: the attached target is pid {}, not {}", session_->pid(), request.pid));
            return result;
        }
        if (!session_->supports_suspend())
        {
            result.error = AccessError::unsupported;
            log::debug(log::category::process, "resume refused: the plugin cannot resume its target");
            return result;
        }

        if (auto resumed = session_->resume(); !resumed)
        {
            result.error = resumed.error();
            log::debug(log::category::process,
                       std::format("resume of pid {} failed: {}", request.pid, describe(*result.error)));
        }
        return result;
    }

    AttachResult AccessWorker::do_detach()
    {
        session_.reset();
        // The Lua state belongs to the session: dropping it drops the script
        // globals, so a later attach starts from a clean state.
        engine_.reset();
        active_scripts_.clear();
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
        std::size_t                        failed  = 0;
        // One snapshot for the whole batch: the symbols are resolved on this
        // thread while scripts may write the table on it, guarded by the table.
        const std::vector<expr::SymbolRef> symbols = symbols_.snapshot();
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

            const auto resolved = expr::evaluate(
                *expression, request.module_refs, symbols, reader, expr::Options {.pointer_size = pointer_size});
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
