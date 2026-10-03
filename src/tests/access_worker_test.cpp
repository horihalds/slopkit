#include <catch2/catch.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "process/access.hpp"
#include "process/access_worker.hpp"
#include "process/types.hpp"

namespace
{
    using namespace std::chrono_literals;

    using slopkit::process::AccessError;
    using slopkit::process::AccessMethod;
    using slopkit::process::AccessWorker;
    using slopkit::process::AttachResult;
    using slopkit::process::FreezeResult;
    using slopkit::process::JobResult;
    using slopkit::process::ListResult;
    using slopkit::process::MemoryMapResult;
    using slopkit::process::ProbeResult;
    using slopkit::process::ReadResult;
    using slopkit::process::WriteItem;
    using slopkit::process::WriteResult;

    // A tiny in-memory target at a fixed base address.
    class FakeBackend final : public slopkit::process::SessionBackend
    {
    public:
        static constexpr std::uint64_t kBase = 0x1000;

        std::vector<std::byte>                    memory       = std::vector<std::byte>(0x40);
        std::size_t                               module_count = 0;
        std::size_t                               thread_count = 0;
        std::vector<slopkit::process::RegionInfo> region_list;

        [[nodiscard]] slopkit::process::ProcessId pid() const noexcept override
        {
            return 7;
        }

        [[nodiscard]] std::string_view plugin_id() const noexcept override
        {
            return "fake";
        }

        [[nodiscard]] AccessMethod advertised_methods() const noexcept override
        {
            return AccessMethod::procfs_mem;
        }

        [[nodiscard]] AccessMethod last_method() const noexcept override
        {
            return AccessMethod::procfs_mem;
        }

        std::expected<std::vector<std::byte>, AccessError> read(std::uint64_t address, std::size_t size) override
        {
            if (address < kBase || address - kBase + size > memory.size())
            {
                return std::unexpected(AccessError::not_found);
            }
            const auto offset = static_cast<std::size_t>(address - kBase);
            return std::vector<std::byte>(memory.begin() + static_cast<std::ptrdiff_t>(offset),
                                          memory.begin() + static_cast<std::ptrdiff_t>(offset + size));
        }

        std::expected<std::size_t, AccessError> write(std::uint64_t address, std::span<const std::byte> data) override
        {
            if (address < kBase || address - kBase + data.size() > memory.size())
            {
                return std::unexpected(AccessError::not_found);
            }
            const auto offset = static_cast<std::size_t>(address - kBase);
            std::copy(data.begin(), data.end(), memory.begin() + static_cast<std::ptrdiff_t>(offset));
            return data.size();
        }

        std::expected<std::vector<slopkit::process::ModuleInfo>, AccessError> modules() override
        {
            return std::vector<slopkit::process::ModuleInfo>(module_count);
        }

        std::expected<std::vector<slopkit::process::ThreadInfo>, AccessError> threads() override
        {
            return std::vector<slopkit::process::ThreadInfo>(thread_count);
        }

        std::expected<std::vector<slopkit::process::RegionInfo>, AccessError> regions() override
        {
            return region_list;
        }
    };

    // A ProcessAccess whose listing and attach block on a gate when gated, so a
    // test can hold a job in flight and prove that submission did not block.
    class GatedAccess final : public slopkit::process::ProcessAccess
    {
    public:
        explicit GatedAccess(bool gated) : gated_(gated) {}

        // True when any job has entered a fake call.
        void wait_until_entered()
        {
            std::unique_lock lock(mutex_);
            entered_cv_.wait(lock,
                             [&]
                             {
                                 return entered_ > 0;
                             });
        }

        void open_gate()
        {
            const std::lock_guard lock(mutex_);
            gate_open_ = true;
            gate_cv_.notify_all();
        }

        [[nodiscard]] FakeBackend* backend() const noexcept
        {
            return backend_;
        }

        std::expected<std::vector<slopkit::process::ProcessInfo>, AccessError> list_processes() override
        {
            enter();

            slopkit::process::ProcessInfo info;
            info.pid       = 7;
            info.name      = "target";
            info.exe_path  = "/usr/bin/target";
            info.plugin_id = "fake";
            info.claimants = {"fake"};
            return std::vector<slopkit::process::ProcessInfo> {std::move(info)};
        }

        std::expected<slopkit::process::Session, AccessError> attach(slopkit::process::ProcessId,
                                                                     std::string_view) override
        {
            enter();

            auto owned          = std::make_unique<FakeBackend>();
            owned->module_count = 2;
            owned->thread_count = 3;
            backend_            = owned.get();
            return slopkit::process::Session {std::move(owned)};
        }

    private:
        void enter()
        {
            std::unique_lock lock(mutex_);
            ++entered_;
            entered_cv_.notify_all();
            if (gated_)
            {
                gate_cv_.wait(lock,
                              [&]
                              {
                                  return gate_open_;
                              });
            }
        }

        bool                    gated_ {true};
        mutable std::mutex      mutex_;
        std::condition_variable entered_cv_;
        std::condition_variable gate_cv_;
        std::size_t             entered_ {0};
        bool                    gate_open_ {false};
        FakeBackend*            backend_ {nullptr};
    };

    // Drains the worker until `done` reports true or the timeout elapses.
    template<typename Predicate>
    bool pump(AccessWorker& worker, Predicate done)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            worker.drain();
            if (done())
            {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return done();
    }
} // namespace

TEST_CASE("submissions do not block and completions arrive after the job finishes", "[worker]")
{
    GatedAccess  access {true};
    AccessWorker worker {access};

    std::atomic<bool> completed {false};
    ListResult        captured;
    worker.submit_list(worker.next_job_id(),
                       [&](JobResult&& result)
                       {
                           captured  = std::get<ListResult>(std::move(result));
                           completed = true;
                       });

    // The call returned while the worker is still blocked in the fake.
    access.wait_until_entered();
    CHECK_FALSE(completed.load());

    access.open_gate();
    REQUIRE(pump(worker,
                 [&]
                 {
                     return completed.load();
                 }));
    REQUIRE(captured.processes.size() == 1);
    CHECK(captured.processes[0].pid == 7);
    CHECK_FALSE(captured.error.has_value());
}

TEST_CASE("completions run on the calling thread in submission order", "[worker]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    std::vector<int> order;
    std::thread::id  callback_thread;
    const auto       calling_thread = std::this_thread::get_id();

    worker.submit_list(worker.next_job_id(),
                       [&](JobResult&&)
                       {
                           callback_thread = std::this_thread::get_id();
                           order.push_back(1);
                       });
    worker.submit_list(worker.next_job_id(),
                       [&](JobResult&&)
                       {
                           order.push_back(2);
                       });

    REQUIRE(pump(worker,
                 [&]
                 {
                     return order.size() == 2;
                 }));
    CHECK(order == std::vector<int> {1, 2});
    CHECK(callback_thread == calling_thread);
}

TEST_CASE("a page read before attach fails and after attach sees the target", "[worker]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    CHECK_FALSE(worker.attached());

    ReadResult before;
    worker.submit_read(worker.next_job_id(),
                       FakeBackend::kBase,
                       4,
                       [&](JobResult&& result)
                       {
                           before = std::get<ReadResult>(std::move(result));
                       });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return !before.bytes.empty() || before.error.has_value();
                 }));
    REQUIRE(before.error.has_value());
    CHECK(before.error == AccessError::internal);

    AttachResult attached;
    worker.submit_attach_app(worker.next_job_id(),
                             7,
                             "fake",
                             [&](JobResult&& result)
                             {
                                 attached = std::get<AttachResult>(std::move(result));
                             });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return attached.info.has_value();
                 }));
    CHECK(worker.attached());
    CHECK(attached.info->pid == 7);
    CHECK(attached.info->plugin_id == "fake");
    CHECK(attached.info->method == AccessMethod::procfs_mem);

    access.backend()->memory[0] = std::byte {0x2A};
    ReadResult after;
    worker.submit_read(worker.next_job_id(),
                       FakeBackend::kBase,
                       1,
                       [&](JobResult&& result)
                       {
                           after = std::get<ReadResult>(std::move(result));
                       });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return !after.bytes.empty() || after.error.has_value();
                 }));
    REQUIRE(after.bytes.size() == 1);
    CHECK(after.bytes[0] == std::byte {0x2A});
    CHECK(after.address == FakeBackend::kBase);
}

TEST_CASE("an attach handoff moves the session out of the worker", "[worker]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    AttachResult handed;
    worker.submit_attach_handoff(worker.next_job_id(),
                                 7,
                                 "fake",
                                 [&](JobResult&& result)
                                 {
                                     handed = std::get<AttachResult>(std::move(result));
                                 });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return handed.handed_session.has_value();
                 }));
    CHECK_FALSE(worker.attached());

    // The worker no longer owns a session...
    ReadResult after;
    worker.submit_read(worker.next_job_id(),
                       FakeBackend::kBase,
                       1,
                       [&](JobResult&& result)
                       {
                           after = std::get<ReadResult>(std::move(result));
                       });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return after.error.has_value();
                 }));
    CHECK(after.error == AccessError::internal);

    // ...but the handed-over session is fully usable.
    auto session = std::move(*handed.handed_session);
    CHECK(session.pid() == 7);
    access.backend()->memory[0] = std::byte {0x11};
    const auto bytes            = session.read(FakeBackend::kBase, 1);
    REQUIRE(bytes.has_value());
    CHECK((*bytes)[0] == std::byte {0x11});
}

TEST_CASE("detach closes the worker session", "[worker]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    bool attached = false;
    worker.submit_attach_app(worker.next_job_id(),
                             7,
                             "fake",
                             [&](JobResult&&)
                             {
                                 attached = true;
                             });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return attached;
                 }));
    REQUIRE(worker.attached());

    bool detached = false;
    worker.submit_detach(worker.next_job_id(),
                         [&](JobResult&&)
                         {
                             detached = true;
                         });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return detached;
                 }));
    CHECK_FALSE(worker.attached());

    ReadResult after;
    worker.submit_read(worker.next_job_id(),
                       FakeBackend::kBase,
                       1,
                       [&](JobResult&& result)
                       {
                           after = std::get<ReadResult>(std::move(result));
                       });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return after.error.has_value();
                 }));
    CHECK(after.error == AccessError::internal);
}

TEST_CASE("probe jobs report the access method and module/thread counts", "[worker]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    ProbeResult probe;
    worker.submit_probe(worker.next_job_id(),
                        7,
                        "fake",
                        [&](JobResult&& result)
                        {
                            probe = std::get<ProbeResult>(std::move(result));
                        });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return probe.method != AccessMethod::none || probe.error.has_value();
                 }));
    CHECK_FALSE(probe.error.has_value());
    CHECK(probe.method == AccessMethod::procfs_mem);
    CHECK(probe.modules == 2);
    CHECK(probe.threads == 3);
}

TEST_CASE("write and freeze jobs reach the target memory", "[worker]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    bool attached = false;
    worker.submit_attach_app(worker.next_job_id(),
                             7,
                             "fake",
                             [&](JobResult&&)
                             {
                                 attached = true;
                             });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return attached;
                 }));

    std::vector<std::byte> bytes {std::byte {0x05}, std::byte {0}, std::byte {0}, std::byte {0}};
    WriteItem              item {.id = 1, .address = FakeBackend::kBase, .bytes = bytes};

    FreezeResult frozen;
    worker.submit_freeze(worker.next_job_id(),
                         std::vector<WriteItem> {item},
                         [&](JobResult&& result)
                         {
                             frozen = std::get<FreezeResult>(std::move(result));
                         });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return frozen.written != 0 || frozen.error.has_value();
                 }));
    CHECK_FALSE(frozen.error.has_value());
    CHECK(frozen.written == 1);
    CHECK(access.backend()->memory[0] == std::byte {0x05});

    WriteResult written;
    worker.submit_write(worker.next_job_id(),
                        1,
                        FakeBackend::kBase + 4,
                        std::vector<std::byte> {std::byte {0x09}},
                        [&](JobResult&& result)
                        {
                            written = std::get<WriteResult>(std::move(result));
                        });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return written.id != 0;
                 }));
    CHECK_FALSE(written.error.has_value());
    CHECK(written.entry_id == 1);
    CHECK(access.backend()->memory[4] == std::byte {0x09});
}

TEST_CASE("destruction drops queued callbacks and waits for the in-flight job", "[worker]")
{
    GatedAccess access {true};

    std::atomic<int> called {0};
    {
        auto worker = std::make_unique<AccessWorker>(access);
        worker->submit_list(worker->next_job_id(),
                            [&](JobResult&&)
                            {
                                ++called;
                            });
        worker->submit_list(worker->next_job_id(),
                            [&](JobResult&&)
                            {
                                ++called;
                            });

        access.wait_until_entered();

        // Release the blocked job so the destructor's join can return.
        std::thread releaser {[&]
                              {
                                  access.open_gate();
                              }};
        worker.reset();
        releaser.join();
    }

    // Neither the in-flight nor the queued callback may run after shutdown.
    CHECK(called.load() == 0);
}

TEST_CASE("memory map jobs report the target modules and regions", "[worker]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    // Before any attach the worker owns no session.
    MemoryMapResult before;
    worker.submit_memory_map(worker.next_job_id(),
                             [&](JobResult&& result)
                             {
                                 before = std::get<MemoryMapResult>(std::move(result));
                             });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return before.error.has_value();
                 }));
    CHECK(before.error == AccessError::internal);

    bool attached = false;
    worker.submit_attach_app(worker.next_job_id(),
                             7,
                             "fake",
                             [&](JobResult&&)
                             {
                                 attached = true;
                             });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return attached;
                 }));

    // Place a readable region on the target so the map has something to report.
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x2000;
    region.readable = true;
    access.backend()->region_list.push_back(region);

    MemoryMapResult map;
    worker.submit_memory_map(worker.next_job_id(),
                             [&](JobResult&& result)
                             {
                                 map = std::get<MemoryMapResult>(std::move(result));
                             });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return !map.modules.empty() || map.error.has_value();
                 }));
    CHECK_FALSE(map.error.has_value());
    CHECK(map.modules.size() == 2);
    REQUIRE(map.regions.size() == 1);
    CHECK(map.regions[0].start == 0x1000);
    CHECK(map.regions[0].end == 0x2000);
    CHECK(map.regions[0].readable);
}

TEST_CASE("the default session read_into copies through read", "[worker]")
{
    auto backend       = std::make_unique<FakeBackend>();
    backend->memory[0] = std::byte {0x2a};
    backend->memory[1] = std::byte {0x2b};

    slopkit::process::Session session {std::move(backend)};

    std::array<std::byte, 4> buffer {};
    const auto               count = session.read_into(FakeBackend::kBase, buffer);
    REQUIRE(count.has_value());
    REQUIRE(*count == buffer.size());
    CHECK(buffer[0] == std::byte {0x2a});
    CHECK(buffer[1] == std::byte {0x2b});

    // Errors propagate exactly like read().
    const auto missing = session.read_into(FakeBackend::kBase + 0x100, buffer);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == AccessError::not_found);
}
