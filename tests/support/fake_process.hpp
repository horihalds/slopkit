#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QString>

#include "process/access.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"

namespace slopkit::test
{
    QApplication& application();

    std::vector<process::ProcessInfo> sample_processes();

    process::ModuleInfo module_image(std::string_view name, std::uint64_t base, std::uint64_t size);

    // A readable mapped region, the shape the scanner's memory map carries: the
    // pointer field tags an address with the kind of memory its region is.
    process::RegionInfo region_of(
        std::uint64_t base, std::uint64_t size, std::string path = {}, bool executable = false, bool writable = false);

    process::AttachedTarget fake_target();

    QString scratch_settings_file(std::string_view suite, std::string_view name);

    struct FakeMemory
    {
        std::map<std::uint64_t, std::vector<std::byte>> windows;
        std::map<std::uint64_t, std::byte>              bytes;
        std::vector<std::byte>                          flat;
        std::uint64_t                                   base {0};

        [[nodiscard]] std::vector<std::byte>& operator[](std::uint64_t address)
        {
            return windows[address];
        }

        [[nodiscard]] std::byte& at(std::uint64_t address)
        {
            return bytes[address];
        }

        [[nodiscard]] std::expected<std::vector<std::byte>, process::AccessError> read(std::uint64_t address,
                                                                                       std::size_t   size) const;
        std::expected<std::size_t, process::AccessError> write(std::uint64_t address, std::span<const std::byte> data);
    };

    class FakeBackend final : public process::SessionBackend
    {
    public:
        FakeBackend();
        explicit FakeBackend(std::shared_ptr<FakeMemory> memory);

        std::shared_ptr<FakeMemory>                        memory;
        std::shared_ptr<std::set<std::uint64_t>>           unreadable {std::make_shared<std::set<std::uint64_t>>()};
        std::optional<process::AccessError>                read_error;
        std::vector<process::ModuleInfo>                   module_list;
        // `modules_error` fails every listing with its error; `modules_failing`
        // is a shared switch a test can flip on a live backend, and
        // `modules_calls` counts every listing, including the attach-time check
        // `do_attach` performs.
        std::optional<process::AccessError>                modules_error;
        std::shared_ptr<std::atomic<bool>>                 modules_failing;
        std::shared_ptr<std::atomic<int>>                  modules_calls;
        std::vector<process::RegionInfo>                   region_list;
        std::size_t                                        module_count {0};
        std::size_t                                        thread_count {0};
        process::ProcessId                                 fake_pid {42};
        std::string                                        fake_plugin_id {"fake"};
        std::atomic<int>                                   reads {0};
        std::atomic<int>                                   writes {0};
        // Called at the end of a successful `write`, after the memory effect and
        // before the worker pushes the job's completion. A test uses it to force
        // that window open.
        std::function<void()>                              after_write;
        // Suspend capability and recording. `can_suspend` gates the capability
        // the panel queries; `suspends`/`resumes` count the operations that
        // reach the backend, and the error knobs model a refused operation.
        bool                                               can_suspend {false};
        std::optional<process::AccessError>                suspend_error;
        std::optional<process::AccessError>                resume_error;
        std::atomic<int>                                   suspends {0};
        std::atomic<int>                                   resumes {0};
        std::shared_ptr<std::atomic<int>>                  suspend_calls;
        std::shared_ptr<std::atomic<int>>                  resume_calls;
        // Allocation capability and recording. `can_allocate` gates the
        // capability the engine queries; `allocates`/`frees` count the calls
        // that reach the backend, the error knobs model a refused operation and
        // `allocations` is the address -> page-rounded-size pool a `free`
        // consults. A successful `allocate` grows the flat window so a script
        // can write through the returned address.
        bool                                               can_allocate {false};
        std::uint64_t                                      allocation_base {0x50000};
        std::optional<process::AccessError>                allocate_error;
        std::optional<process::AccessError>                free_error;
        std::map<std::uint64_t, std::size_t>               allocations;
        std::vector<std::pair<std::uint64_t, std::size_t>> allocate_requests;
        std::atomic<int>                                   allocates {0};
        std::atomic<int>                                   frees {0};
        // Validation capability and recording. `can_validate` gates a direct
        // answer; when false the base probe read decides. `validate_error` models
        // a refused operation, `validates` counts the calls that reach the
        // backend, and an address in `unreadable` answers invalid.
        bool                                               can_validate {true};
        std::optional<process::AccessError>                validate_error;
        std::atomic<int>                                   validates {0};

        [[nodiscard]] process::ProcessId    pid() const noexcept override;
        [[nodiscard]] std::string_view      plugin_id() const noexcept override;
        [[nodiscard]] process::AccessMethod advertised_methods() const noexcept override;
        [[nodiscard]] process::AccessMethod last_method() const noexcept override;

        std::expected<std::vector<std::byte>, process::AccessError> read(std::uint64_t address,
                                                                         std::size_t   size) override;
        std::expected<std::size_t, process::AccessError>            write(std::uint64_t              address,
                                                                          std::span<const std::byte> data) override;
        std::expected<std::vector<process::ModuleInfo>, process::AccessError> modules() override;
        std::expected<std::vector<process::ThreadInfo>, process::AccessError> threads() override;
        std::expected<std::vector<process::RegionInfo>, process::AccessError> regions() override;
        [[nodiscard]] bool                                 supports_suspend() const noexcept override;
        std::expected<void, process::AccessError>          suspend() override;
        std::expected<void, process::AccessError>          resume() override;
        [[nodiscard]] bool                                 supports_allocation() const noexcept override;
        std::expected<std::uint64_t, process::AccessError> allocate(std::size_t   size,
                                                                    std::uint64_t near_address) override;
        std::expected<void, process::AccessError>          free(std::uint64_t address) override;
        std::expected<bool, process::AccessError>          validate(std::uint64_t address, std::size_t size) override;
    };

    class FakeAccess final : public process::ProcessAccess
    {
    public:
        std::vector<process::ProcessInfo>        processes;
        std::vector<process::ModuleInfo>         modules;
        std::vector<process::RegionInfo>         regions;
        bool                                     attach_fails {false};
        std::optional<process::AccessError>      read_error;
        std::optional<process::AccessError>      modules_error;
        std::shared_ptr<std::atomic<bool>>       modules_failing;
        std::shared_ptr<std::atomic<int>>        modules_calls;
        std::shared_ptr<FakeMemory>              memory {std::make_shared<FakeMemory>()};
        std::shared_ptr<std::set<std::uint64_t>> unreadable {std::make_shared<std::set<std::uint64_t>>()};
        process::ProcessId                       fake_pid {42};
        bool                                     can_suspend {false};
        std::optional<process::AccessError>      suspend_error;
        std::optional<process::AccessError>      resume_error;
        std::shared_ptr<std::atomic<int>>        suspend_calls {std::make_shared<std::atomic<int>>(0)};
        std::shared_ptr<std::atomic<int>>        resume_calls {std::make_shared<std::atomic<int>>(0)};
        bool                                     can_allocate {false};
        std::uint64_t                            allocation_base {0x50000};
        std::optional<process::AccessError>      allocate_error;
        std::optional<process::AccessError>      free_error;
        bool                                     can_validate {true};
        std::optional<process::AccessError>      validate_error;
        std::atomic<int>                         attach_calls {0};
        std::atomic<int>                         list_calls {0};
        std::function<void()>                    after_write;

        std::expected<std::vector<process::ProcessInfo>, process::AccessError> list_processes() override;
        std::expected<process::Session, process::AccessError> attach(process::ProcessId pid,
                                                                     std::string_view   plugin_id) override;
    };

    // Holds a write job after its memory effect and before the worker pushes the
    // job's completion, so a test can force that window open: the fake memory is
    // already updated while the completion that produces the status text is
    // still pending. Install `hook()` on the access before the target attaches.
    class WriteGate
    {
    public:
        [[nodiscard]] std::function<void()> hook()
        {
            return [this]
            {
                std::unique_lock lock(mutex_);
                entered_ = true;
                entered_cv_.notify_all();
                release_cv_.wait(lock,
                                 [this]
                                 {
                                     return !held_;
                                 });
            };
        }

        // Makes the next write park after its memory effect.
        void hold()
        {
            const std::lock_guard lock(mutex_);
            held_    = true;
            entered_ = false;
        }

        [[nodiscard]] bool wait_until_held(std::chrono::milliseconds timeout = std::chrono::seconds(5))
        {
            std::unique_lock lock(mutex_);
            return entered_cv_.wait_for(lock,
                                        timeout,
                                        [this]
                                        {
                                            return entered_;
                                        });
        }

        void release()
        {
            const std::lock_guard lock(mutex_);
            held_ = false;
            release_cv_.notify_all();
        }

    private:
        std::mutex              mutex_;
        std::condition_variable entered_cv_;
        std::condition_variable release_cv_;
        bool                    held_ {false};
        bool                    entered_ {false};
    };

    // Drains the worker (and the Qt event loop) until `done` holds and the
    // worker is quiescent, or the timeout elapses. A job's side effect and its
    // completion are two steps, and only the drained completion produces what a
    // caller asserts (the status text, a model update), so evaluating `done`
    // before the worker settles would observe the previous job's result.
    template<typename Predicate>
    bool pump_ui(process::AccessWorker& worker, Predicate done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            worker.drain();
            QCoreApplication::processEvents();
            if (worker.idle() && done())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        worker.drain();
        return worker.idle() && done();
    }

}
