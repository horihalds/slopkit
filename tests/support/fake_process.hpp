#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
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

    // A QApplication may only exist once per process; every widget suite shares
    // the one `test_main.cpp` creates.
    QApplication& application();

    // Two processes both the UI and the worker suites can list.
    std::vector<process::ProcessInfo> sample_processes();

    // A file-backed image at `base`, and a label for `name`.
    process::ModuleInfo module_image(std::string_view name, std::uint64_t base, std::uint64_t size);

    // A fully-populated attached target for the suites that do not drive a real attach.
    process::AttachedTarget fake_target();

    // A fresh settings file under `SLOPKIT_TMP_DIR/<suite>`; the previous file is
    // removed so a case never reads another case's leftovers.
    QString scratch_settings_file(std::string_view suite, std::string_view name);

    // The bytes a fake target serves. Which member a suite seeds decides how the
    // fake reads:
    //   * `windows` — address-keyed fixed windows (the UI suites); an unmapped
    //     address reads as an empty buffer,
    //   * `flat` at `base` — one flat window (the worker suite); out-of-range
    //     reads fail with `not_found`,
    //   * `bytes` — sparse per-byte overrides (the memory-view suite); every other
    //     byte reads as zero.
    // Writes go back into `flat` or `bytes`; a window-only fake is read-only.
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

        // Sparse per-byte accessor: a read defaults to zero, a write stores the byte.
        [[nodiscard]] std::byte& at(std::uint64_t address)
        {
            return bytes[address];
        }

        [[nodiscard]] std::expected<std::vector<std::byte>, process::AccessError> read(std::uint64_t address,
                                                                                       std::size_t   size) const;
        std::expected<std::size_t, process::AccessError> write(std::uint64_t address, std::span<const std::byte> data);
    };

    // A tiny in-memory target a Session can wrap.
    class FakeBackend final : public process::SessionBackend
    {
    public:
        FakeBackend();
        explicit FakeBackend(std::shared_ptr<FakeMemory> memory);

        std::shared_ptr<FakeMemory>                        memory;
        std::shared_ptr<std::set<std::uint64_t>>           unreadable {std::make_shared<std::set<std::uint64_t>>()};
        // When set, every read fails with this error, modelling a target whose
        // metadata is readable but whose memory cannot be.
        std::optional<process::AccessError>                read_error;
        // `module_list` wins when non-empty; otherwise `module_count` blank images
        // are reported, with a file-backed main image at the flat window's base.
        std::vector<process::ModuleInfo>                   module_list;
        std::vector<process::RegionInfo>                   region_list;
        std::size_t                                        module_count {0};
        std::size_t                                        thread_count {0};
        process::ProcessId                                 fake_pid {42};
        std::string                                        fake_plugin_id {"fake"};
        std::atomic<int>                                   reads {0};
        std::atomic<int>                                   writes {0};
        // Suspend capability and recording. `can_suspend` gates the capability
        // the panel queries; `suspends`/`resumes` count the operations that
        // reach the backend, and the error knobs model a refused operation.
        bool                                               can_suspend {false};
        std::optional<process::AccessError>                suspend_error;
        std::optional<process::AccessError>                resume_error;
        std::atomic<int>                                   suspends {0};
        std::atomic<int>                                   resumes {0};
        // Shared counters owned by the FakeAccess that built this backend, so a
        // test observes suspend/resume jobs from outside the worker, whichever
        // session (app or handoff) the backend belongs to.
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
    };

    // A ProcessAccess serving a fixed process list that can be told to fail the
    // attach, so a dialog can be driven without a real target.
    class FakeAccess final : public process::ProcessAccess
    {
    public:
        std::vector<process::ProcessInfo>        processes;
        std::vector<process::ModuleInfo>         modules;
        std::vector<process::RegionInfo>         regions;
        bool                                     attach_fails {false};
        std::optional<process::AccessError>      read_error;
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
        std::atomic<int>                         attach_calls {0};
        std::atomic<int>                         list_calls {0};

        std::expected<std::vector<process::ProcessInfo>, process::AccessError> list_processes() override;
        std::expected<process::Session, process::AccessError> attach(process::ProcessId pid,
                                                                     std::string_view   plugin_id) override;
    };

    // Drains the worker (and the Qt event loop) until `done` holds or the timeout
    // elapses.
    template<typename Predicate>
    bool pump_ui(process::AccessWorker& worker, Predicate done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            worker.drain();
            QCoreApplication::processEvents();
            if (done())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    }

} // namespace slopkit::test
