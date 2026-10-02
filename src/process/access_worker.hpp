#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "process/access.hpp"
#include "process/types.hpp"

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

    // Metadata of a successful app attach; the session itself stays in the
    // worker.
    struct AttachInfo
    {
        ProcessId    pid {};
        std::string  plugin_id;
        AccessMethod method {AccessMethod::none};
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

    struct ReadResult
    {
        JobId                      id {};
        std::uint64_t              address {};
        std::vector<std::byte>     bytes;
        std::optional<AccessError> error;
    };

    struct WriteResult
    {
        JobId                      id {};
        std::uint64_t              entry_id {};
        std::optional<AccessError> error;
    };

    struct FreezeResult
    {
        std::size_t                written {};
        std::optional<AccessError> error;
    };

    using JobResult =
        std::variant<ListResult, ProbeResult, AttachResult, AppIndexResult, ReadResult, WriteResult, FreezeResult>;

    // Runs on the UI thread inside AccessWorker::drain().
    using JobCallback = std::move_only_function<void(JobResult&&)>;

    // Serializes every target access on one background thread. The worker owns
    // the application's Session; the UI only submits jobs and drains the
    // completions once per frame. Only the UI thread touches ImGui and OpenGL.
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
        bool submit_read(JobId id, std::uint64_t address, std::size_t size, JobCallback on_done);
        bool submit_write(
            JobId id, std::uint64_t entry_id, std::uint64_t address, std::vector<std::byte> bytes, JobCallback on_done);
        bool submit_freeze(JobId id, std::vector<WriteItem> items, JobCallback on_done);
        // Detaching reuses AttachResult with an empty info; the caller knows it
        // requested a detach.
        bool submit_detach(JobId id, JobCallback on_done);

        // Invokes pending callbacks on the calling (UI) thread; call once per
        // frame. Bounded so a single frame never drains an unbounded backlog.
        std::size_t drain(std::size_t max_jobs = 64);

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
            read,
            write,
            freeze,
            detach,
        };

        struct Request
        {
            JobKind                kind {};
            JobId                  id {};
            ProcessId              pid {};
            std::string            plugin_id;
            std::uint64_t          address {};
            std::size_t            size {};
            std::uint64_t          entry_id {};
            std::vector<std::byte> bytes;
            std::vector<WriteItem> items;
            JobCallback            on_done;
        };

        struct Completion
        {
            JobCallback on_done;
            JobResult   result;
        };

        void           run(std::stop_token token);
        JobResult      execute(Request& request);
        bool           submit(Request request);
        ListResult     do_list();
        ProbeResult    do_probe(const Request& request);
        AttachResult   do_attach(const Request& request, bool handoff);
        AppIndexResult do_application_index();
        ReadResult     do_read(const Request& request);
        WriteResult    do_write(const Request& request);
        FreezeResult   do_freeze(const Request& request);
        AttachResult   do_detach();

        ProcessAccess&          access_;
        std::jthread            worker_;
        std::mutex              mutex_;
        std::condition_variable cv_;
        std::deque<Request>     requests_;
        std::deque<Completion>  completions_;
        std::optional<Session>  session_; // worker thread only
        std::atomic<bool>       attached_ {false};
        std::atomic<JobId>      next_id_ {1};
    };

} // namespace slopkit::process
