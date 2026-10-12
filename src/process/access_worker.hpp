#pragma once

#include <atomic>
#include <chrono>
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

#include "analysis/string_references.hpp"
#include "disasm/decoder.hpp"
#include "expr/resolver.hpp"
#include "process/access.hpp"
#include "process/types.hpp"
#include "script/engine.hpp"
#include "script/symbols.hpp"
#include "script/types.hpp"

namespace slopkit::process
{
    using JobId = std::uint64_t;

    struct WriteItem
    {
        std::uint64_t          id {};
        std::uint64_t          address {};
        std::vector<std::byte> bytes;
    };

    struct ReadManyItem
    {
        std::uint64_t address {};
        std::size_t   size {};
    };

    struct AttachInfo
    {
        ProcessId                  pid {};
        std::string                plugin_id;
        AccessMethod               method {AccessMethod::none};
        bool                       can_suspend {false};
        std::optional<AccessError> read_error;
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
        std::optional<AccessError> error;
        std::optional<AccessError> modules_error;
        std::optional<AccessError> read_error;
    };

    struct AttachResult
    {
        std::optional<AttachInfo>  info;
        std::optional<AccessError> error;
        std::optional<Session>     handed_session;
    };

    struct AppIndexResult
    {
        std::vector<std::string> executables;
    };

    struct ResolveRequest
    {
        std::uint64_t key {};
        std::string   expression;
    };

    struct ResolveItemResult
    {
        std::uint64_t                key {};
        std::optional<std::uint64_t> address;
        std::string                  error;
    };

    struct ResolveResult
    {
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

    struct ScriptResult
    {
        JobId                                  id {};
        std::string                            description;
        script::RunResult                      run;
        std::optional<script::LifecycleResult> lifecycle;
    };

    struct ScriptUpdateFailure
    {
        std::string description;
        std::string reason;
    };

    struct ScriptUpdatesResult
    {
        std::size_t                      updated {};
        std::vector<ScriptUpdateFailure> failed;
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

    struct SuspendResult
    {
        std::optional<AccessError> error;
    };

    struct StringRefsResult
    {
        std::string                            module;
        std::uint64_t                          base {};
        std::vector<analysis::StringReference> entries;
        analysis::StringScanReport             report;
        std::optional<AccessError>             error;
    };

    using JobResult = std::variant<ListResult,
                                   ProbeResult,
                                   AttachResult,
                                   AppIndexResult,
                                   ReadResult,
                                   ReadManyResult,
                                   WriteResult,
                                   ScriptResult,
                                   ScriptUpdatesResult,
                                   FreezeResult,
                                   SuspendResult,
                                   MemoryMapResult,
                                   ResolveResult,
                                   StringRefsResult>;

    using JobCallback = std::move_only_function<void(JobResult&&)>;

    using CompletionHook = std::move_only_function<void()>;

    class AccessWorker
    {
    public:
        explicit AccessWorker(ProcessAccess& access, script::SymbolTable& symbols = script::default_symbol_table());
        ~AccessWorker();

        AccessWorker(const AccessWorker&)            = delete;
        AccessWorker& operator=(const AccessWorker&) = delete;

        bool submit_list(JobId id, JobCallback on_done);
        bool submit_probe(JobId id, ProcessId pid, std::string plugin_id, JobCallback on_done);
        bool submit_attach_app(JobId id, ProcessId pid, std::string plugin_id, JobCallback on_done);
        bool submit_attach_handoff(JobId id, ProcessId pid, std::string plugin_id, JobCallback on_done);
        bool submit_application_index(JobId id, JobCallback on_done);
        bool submit_memory_map(JobId id, JobCallback on_done);
        bool submit_read(JobId id, std::uint64_t address, std::size_t size, JobCallback on_done);
        bool submit_read_many(JobId id, std::vector<ReadManyItem> items, JobCallback on_done);
        bool submit_resolve_expressions(JobId                        id,
                                        std::vector<ResolveRequest>  requests,
                                        std::vector<expr::ModuleRef> modules,
                                        std::size_t                  pointer_size,
                                        JobCallback                  on_done);
        bool submit_string_references(JobId               id,
                                      std::string         module,
                                      std::uint64_t       base,
                                      std::uint64_t       size,
                                      disasm::MachineMode mode,
                                      JobCallback         on_done);
        bool submit_write(
            JobId id, std::uint64_t entry_id, std::uint64_t address, std::vector<std::byte> bytes, JobCallback on_done);
        bool submit_freeze(JobId id, std::vector<WriteItem> items, JobCallback on_done);
        bool submit_script(JobId       id,
                           std::string description,
                           std::string chunk,
                           std::string function,
                           std::size_t pointer_size,
                           JobCallback on_done);
        bool submit_script_updates(JobId id, JobCallback on_done);
        bool submit_suspend(JobId id, ProcessId pid, JobCallback on_done);
        bool submit_resume(JobId id, ProcessId pid, JobCallback on_done);
        bool submit_detach(JobId id, JobCallback on_done);

        std::size_t drain(std::size_t max_jobs = 64);

        // No queued request, no job in flight and no undelivered completion, so
        // a drain() would find nothing to do. What a test has to wait for
        // before asserting on anything a completion produces.
        [[nodiscard]] bool idle() const noexcept;

        void set_completion_hook(CompletionHook hook);

        [[nodiscard]] bool attached() const noexcept;

        [[nodiscard]] std::size_t active_scripts() const noexcept;

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
            script_update,
            suspend,
            resume,
            detach,
            resolve_expressions,
            string_references,
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
            std::string                  function;
            std::string                  module_name;
            disasm::MachineMode          machine_mode {disasm::MachineMode::long_64};
            JobCallback                  on_done;
        };

        struct Completion
        {
            JobCallback on_done;
            JobResult   result;
        };

        struct ActiveScript
        {
            std::string                                          description;
            std::string                                          chunk;
            // What the accepted `activate` last passed to `update_interval`;
            // absent (or 0) means the script is due on every pass.
            std::optional<std::uint64_t>                         interval_ms;
            std::optional<std::chrono::steady_clock::time_point> last_tick;
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
        ScriptUpdatesResult     do_script_updates();
        void                    remember_lifecycle(const Request& request, const ScriptResult& result);
        void                    note_script_tick(std::string_view                      description,
                                                 const std::optional<std::uint64_t>&   interval_ms,
                                                 std::chrono::steady_clock::time_point now);
        void                    deactivate_active_scripts();
        void                    deactivate_and_report(const ActiveScript& script, std::string_view why);
        void                    clear_active_scripts();
        script::MemoryApi       memory_api();
        script::SymbolApi       symbol_api();
        SuspendResult           do_suspend(const Request& request);
        SuspendResult           do_resume(const Request& request);
        AttachResult            do_detach();
        ResolveResult           do_resolve(const Request& request);
        StringRefsResult        do_string_references(const Request& request);

        ProcessAccess&                access_;
        script::SymbolTable&          symbols_;
        std::mutex                    mutex_;
        std::condition_variable       cv_;
        std::deque<Request>           requests_;
        std::deque<Completion>        completions_;
        CompletionHook                completion_hook_;
        std::optional<Session>        session_;
        std::optional<script::Engine> engine_;
        std::vector<ActiveScript>     active_scripts_;
        std::size_t                   script_pointer_size_ {8};
        std::atomic<bool>             attached_ {false};
        std::atomic<std::size_t>      active_script_count_ {0};
        std::atomic<JobId>            next_id_ {1};
        // Jobs counted from submit() (or the direct interrupt path) until their
        // completion is drained; a request abandoned at shutdown is dropped in
        // ~AccessWorker so idle() cannot latch false.
        std::atomic<std::size_t>      pending_ {0};
        bool                          stopping_ {false};

        std::jthread worker_;
    };

}
