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
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "expr/resolver.hpp"
#include "process/access.hpp"
#include "process/types.hpp"
#include "script/engine.hpp"
#include "script/types.hpp"

namespace slopkit::process
{

    // Monotonic job identifier. The UI drops completions whose id is no longer
    // the one it is waiting for, so stale results never reach the view.
    using JobId = std::uint64_t;

    // One address a freeze pass must rewrite; built on the UI thread from the
    // address table and executed on the worker.
    struct WriteItem
    {
        std::uint64_t          id {}; // table::AddressEntry::id
        std::uint64_t          address {};
        std::vector<std::byte> bytes;
    };

    // One address a batched live read must fetch; `size` is the value's width.
    struct ReadManyItem
    {
        std::uint64_t address {};
        std::size_t   size {};
    };

    // Metadata of a successful app attach; the session itself stays in the
    // worker.
    struct AttachInfo
    {
        ProcessId                  pid {};
        std::string                plugin_id;
        AccessMethod               method {AccessMethod::none};
        bool                       can_suspend {false};
        std::optional<AccessError> read_error; // set when the target is not readable
    };

    struct ListResult
    {
        std::vector<ProcessInfo>   processes;
        std::optional<AccessError> error;
    };

    struct ProbeResult
    {
        AccessMethod               method {AccessMethod::none};
        std::size_t                modules {};
        std::size_t                threads {};
        std::optional<AccessError> error;         // attach failure
        std::optional<AccessError> modules_error; // modules() failure
        std::optional<AccessError> read_error;    // memory read probe failure
    };

    struct AttachResult
    {
        std::optional<AttachInfo>  info;
        std::optional<AccessError> error;
        // Set only by handoff jobs: the created session is handed to the caller.
        std::optional<Session>     handed_session;
    };

    struct AppIndexResult
    {
        std::vector<std::string> executables;
    };

    // One expression a batched resolve must evaluate; `key` is echoed back so the
    // caller can match the result to the entry that requested it.
    struct ResolveRequest
    {
        std::uint64_t key {};
        std::string   expression;
    };

    // One resolved expression. `address` is set on success; otherwise `error`
    // carries the readable reason. An unreadable item never fails the batch.
    struct ResolveItemResult
    {
        std::uint64_t                key {};
        std::optional<std::uint64_t> address;
        std::string                  error;
    };

    struct ResolveResult
    {
        // In request order. Empty when the job itself fails (no session).
        std::vector<ResolveItemResult> items;
        std::optional<AccessError>     error;
    };

    struct ReadResult
    {
        JobId                      id {};
        std::uint64_t              address {};
        std::vector<std::byte>     bytes;
        std::optional<AccessError> error;
    };

    // One reading per batched request, kept in request order. A single
    // unreadable address reports its own error without failing the batch.
    using ReadManyItemResult = std::expected<std::vector<std::byte>, AccessError>;

    struct ReadManyResult
    {
        std::vector<ReadManyItemResult> items;
    };

    struct WriteResult
    {
        JobId                      id {};
        std::uint64_t              entry_id {};
        std::optional<AccessError> error;
    };

    // Outcome of running one script entry: the chunk's own run result plus the
    // entry's description so the caller can name it in its log record.
    struct ScriptResult
    {
        JobId             id {};
        std::string       description;
        script::RunResult run;
    };

    struct MemoryMapResult
    {
        std::vector<ModuleInfo>    modules;
        std::vector<RegionInfo>    regions;
        std::optional<AccessError> error;
    };

    struct FreezeResult
    {
        std::size_t                written {};
        std::optional<AccessError> error;
    };

    // Outcome of a suspend or resume job; null error means it succeeded.
    struct SuspendResult
    {
        std::optional<AccessError> error;
    };

    using JobResult = std::variant<ListResult,
                                   ProbeResult,
                                   AttachResult,
                                   AppIndexResult,
                                   ReadResult,
                                   ReadManyResult,
                                   WriteResult,
                                   ScriptResult,
                                   FreezeResult,
                                   SuspendResult,
                                   MemoryMapResult,
                                   ResolveResult>;

    // Runs on the UI thread inside AccessWorker::drain().
    using JobCallback = std::move_only_function<void(JobResult&&)>;

    // Invoked on the worker thread right after a completion is queued, so the UI
    // can post a wake-up to its own event loop. It must not touch a widget, a
    // session or any other UI state; keep it to a thread-safe notification.
    using CompletionHook = std::move_only_function<void()>;

    // Serializes every target access on one background thread. The worker owns
    // the application's Session; the UI only submits jobs and drains the
    // completions after the completion hook posts a wake-up. Only the UI thread
    // touches the widgets.
    class AccessWorker
    {
    public:
        explicit AccessWorker(ProcessAccess& access);
        ~AccessWorker();

        AccessWorker(const AccessWorker&)            = delete;
        AccessWorker& operator=(const AccessWorker&) = delete;

        // Every submit* returns false without blocking when the worker is
        // stopping.
        bool submit_list(JobId id, JobCallback on_done);
        bool submit_probe(JobId id, ProcessId pid, std::string plugin_id, JobCallback on_done);
        bool submit_attach_app(JobId id, ProcessId pid, std::string plugin_id, JobCallback on_done);
        bool submit_attach_handoff(JobId id, ProcessId pid, std::string plugin_id, JobCallback on_done);
        bool submit_application_index(JobId id, JobCallback on_done);
        bool submit_memory_map(JobId id, JobCallback on_done);
        bool submit_read(JobId id, std::uint64_t address, std::size_t size, JobCallback on_done);
        bool submit_read_many(JobId id, std::vector<ReadManyItem> items, JobCallback on_done);
        // Evaluates every expression (pointer reads included) in one job. The
        // module map is a snapshot the caller keeps current; `pointer_size` is
        // the target's pointer width.
        bool submit_resolve_expressions(JobId                        id,
                                        std::vector<ResolveRequest>  requests,
                                        std::vector<expr::ModuleRef> modules,
                                        std::size_t                  pointer_size,
                                        JobCallback                  on_done);
        bool submit_write(
            JobId id, std::uint64_t entry_id, std::uint64_t address, std::vector<std::byte> bytes, JobCallback on_done);
        bool submit_freeze(JobId id, std::vector<WriteItem> items, JobCallback on_done);
        // Runs `chunk` as a Lua script against the attached target, with
        // `pointer_size` as the target's pointer width. The engine and its state
        // outlive the job, so globals persist between runs on one session.
        bool submit_script(
            JobId id, std::string description, std::string chunk, std::size_t pointer_size, JobCallback on_done);
        // Stops / resumes the attached target through the worker's session. `pid`
        // must match that session, so a stale target between the click and the
        // job is refused instead of stopped.
        bool submit_suspend(JobId id, ProcessId pid, JobCallback on_done);
        bool submit_resume(JobId id, ProcessId pid, JobCallback on_done);
        // Detaching reuses AttachResult with an empty info; the caller knows it
        // requested a detach.
        bool submit_detach(JobId id, JobCallback on_done);

        // Invokes pending callbacks on the calling (UI) thread; call after the
        // completion hook posted a wake-up. Bounded so a single call never drains
        // an unbounded backlog.
        std::size_t drain(std::size_t max_jobs = 64);

        // Registers the notification invoked on the worker thread after a
        // completion is queued; pass an empty hook to clear it.
        void set_completion_hook(CompletionHook hook);

        // Atomic mirror of the worker-owned session slot.
        [[nodiscard]] bool attached() const noexcept;

        // Monotonic ids for call sites.
        [[nodiscard]] JobId next_job_id() noexcept;

    private:
        enum class JobKind
        {
            list,
            probe,
            attach_app,
            attach_handoff,
            application_index,
            memory_map,
            read,
            read_many,
            write,
            freeze,
            script,
            suspend,
            resume,
            detach,
            resolve_expressions,
        };

        struct Request
        {
            JobKind                      kind {};
            JobId                        id {};
            ProcessId                    pid {};
            std::string                  plugin_id;
            std::uint64_t                address {};
            std::size_t                  size {};
            std::uint64_t                entry_id {};
            std::vector<std::byte>       bytes;
            std::vector<WriteItem>       items;
            std::vector<ReadManyItem>    read_items;
            std::vector<ResolveRequest>  resolve_items;
            std::vector<expr::ModuleRef> module_refs;
            std::size_t                  pointer_size {};
            std::string                  description;
            std::string                  chunk;
            JobCallback                  on_done;
        };

        struct Completion
        {
            JobCallback on_done;
            JobResult   result;
        };

        void                    run(std::stop_token token);
        JobResult               execute(Request& request);
        bool                    submit(Request request);
        static std::string_view job_kind_name(JobKind kind) noexcept;
        ListResult              do_list();
        ProbeResult             do_probe(const Request& request);
        AttachResult            do_attach(const Request& request, bool handoff);
        AppIndexResult          do_application_index();
        MemoryMapResult         do_memory_map();
        ReadResult              do_read(const Request& request);
        ReadManyResult          do_read_many(const Request& request);
        WriteResult             do_write(const Request& request);
        FreezeResult            do_freeze(const Request& request);
        ScriptResult            do_script(const Request& request);
        // The seam a script sees the target through. Its lambdas read the
        // worker's current session, so one engine stays valid across attaches.
        script::MemoryApi       memory_api();
        SuspendResult           do_suspend(const Request& request);
        SuspendResult           do_resume(const Request& request);
        AttachResult            do_detach();
        ResolveResult           do_resolve(const Request& request);

        ProcessAccess&                access_;
        std::mutex                    mutex_;
        std::condition_variable       cv_;
        std::deque<Request>           requests_;
        std::deque<Completion>        completions_;
        CompletionHook                completion_hook_;         // guarded by mutex_
        std::optional<Session>        session_;                 // worker thread only
        std::optional<script::Engine> engine_;                  // worker thread only, follows session_
        std::size_t                   script_pointer_size_ {8}; // worker thread only
        std::atomic<bool>             attached_ {false};
        std::atomic<JobId>            next_id_ {1};
        bool                          stopping_ {false}; // guarded by mutex_

        // Declared last: the worker thread started by the constructor touches
        // mutex_, cv_, requests_ and completion_hook_ before it can process a
        // job, so it must not exist while those members are still being built.
        std::jthread worker_;
    };

} // namespace slopkit::process
