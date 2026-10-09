#include <catch2/catch.hpp>

#include "support/access_worker_helpers.hpp"

namespace
{
    using slopkit::process::ScriptResult;

    // Attaches the fake target on the worker and waits for the completion.
    bool attach_target(AccessWorker& worker)
    {
        AttachResult attached;
        worker.submit_attach_app(worker.next_job_id(),
                                 7,
                                 "fake",
                                 [&](JobResult&& result)
                                 {
                                     attached = std::get<AttachResult>(std::move(result));
                                 });
        return pump(worker,
                    [&]
                    {
                        return attached.info.has_value();
                    });
    }

    // Submits one script job and returns its result once the completion arrived.
    ScriptResult run_script(AccessWorker& worker, std::string chunk, std::size_t pointer_size = 8)
    {
        ScriptResult result;
        bool         done = false;
        worker.submit_script(worker.next_job_id(),
                             "helper",
                             std::move(chunk),
                             std::string {},
                             pointer_size,
                             [&](JobResult&& job)
                             {
                                 result = std::get<ScriptResult>(std::move(job));
                                 done   = true;
                             });
        pump(worker,
             [&]
             {
                 return done;
             });
        return result;
    }

    // Submits one lifecycle job for `function` (activate/deactivate) and returns
    // its result once the completion arrived.
    ScriptResult run_lifecycle(AccessWorker& worker, std::string chunk, std::string function)
    {
        ScriptResult result;
        bool         done = false;
        worker.submit_script(worker.next_job_id(),
                             "helper",
                             std::move(chunk),
                             std::move(function),
                             8,
                             [&](JobResult&& job)
                             {
                                 result = std::get<ScriptResult>(std::move(job));
                                 done   = true;
                             });
        pump(worker,
             [&]
             {
                 return done;
             });
        return result;
    }
} // namespace

TEST_CASE("submissions do not block and completions arrive after the job finishes", "[process]")
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

TEST_CASE("completions run on the calling thread in submission order", "[process]")
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

TEST_CASE("a page read before attach fails and after attach sees the target", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    CHECK_FALSE(worker.attached());

    ReadResult before;
    worker.submit_read(worker.next_job_id(),
                       kBase,
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

    access.backend()->memory->flat[0] = std::byte {0x2A};
    ReadResult after;
    worker.submit_read(worker.next_job_id(),
                       kBase,
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
    CHECK(after.address == kBase);
}

TEST_CASE("an attach handoff moves the session out of the worker", "[process]")
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
                       kBase,
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
    access.backend()->memory->flat[0] = std::byte {0x11};
    const auto bytes                  = session.read(kBase, 1);
    REQUIRE(bytes.has_value());
    CHECK((*bytes)[0] == std::byte {0x11});
}

TEST_CASE("detach closes the worker session", "[process]")
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
                       kBase,
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

TEST_CASE("probe jobs report the access method and module/thread counts", "[process]")
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
    CHECK_FALSE(probe.read_error.has_value());
}

TEST_CASE("probe and attach report an unreadable target", "[process]")
{
    GatedAccess access {false};
    // Metadata and attach keep working; only the memory read is refused.
    access.read_error = AccessError::permission_denied;
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
    CHECK(probe.read_error == AccessError::permission_denied);

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
                     return attached.info.has_value() || attached.error.has_value();
                 }));
    REQUIRE_FALSE(attached.error.has_value());
    REQUIRE(attached.info.has_value());
    CHECK(attached.info->read_error == AccessError::permission_denied);
}

TEST_CASE("write and freeze jobs reach the target memory", "[process]")
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
    WriteItem              item {.id = 1, .address = kBase, .bytes = bytes};

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
    CHECK(access.backend()->memory->flat[0] == std::byte {0x05});

    WriteResult written;
    worker.submit_write(worker.next_job_id(),
                        1,
                        kBase + 4,
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
    CHECK(access.backend()->memory->flat[4] == std::byte {0x09});
}

TEST_CASE("destruction drops queued callbacks and waits for the in-flight job", "[process]")
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

TEST_CASE("memory map jobs report the target modules and regions", "[process]")
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

TEST_CASE("a failed read job logs exactly one warning", "[process][log]")
{
    LevelGuard level;

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    SECTION("the failure is warned exactly once")
    {
        slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::debug);

        GatedAccess  access {false};
        AccessWorker worker {access};

        ReadResult read;
        worker.submit_read(worker.next_job_id(),
                           kBase,
                           4,
                           [&](JobResult&& result)
                           {
                               read = std::get<ReadResult>(std::move(result));
                           });
        REQUIRE(pump(worker,
                     [&]
                     {
                         return read.error.has_value();
                     }));

        std::size_t warnings = 0;
        for (const auto& record : records)
        {
            if (record.level == slopkit::log::Level::warning && record.category == "process"
                && record.message.find("requested without an attached target") != std::string::npos)
            {
                ++warnings;
            }
        }
        // The failure is decided (and therefore logged) exactly once, by the worker.
        CHECK(warnings == 1);
    }

    SECTION("job lifecycle detail is debug-only")
    {
        slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

        GatedAccess  access {false};
        AccessWorker worker {access};

        bool listed = false;
        worker.submit_list(worker.next_job_id(),
                           [&](JobResult&&)
                           {
                               listed = true;
                           });
        REQUIRE(pump(worker,
                     [&]
                     {
                         return listed;
                     }));

        for (const auto& record : records)
        {
            CHECK(record.level != slopkit::log::Level::debug);
        }
    }
}

TEST_CASE("a batched resolve without an attached target fails the job", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    ResolveResult batch;
    worker.submit_resolve_expressions(worker.next_job_id(),
                                      std::vector<ResolveRequest> {
                                          {.key = 1, .expression = "firefox-bin"}
    },
                                      std::vector<slopkit::expr::ModuleRef> {{"firefox-bin", kBase}},
                                      8,
                                      [&](JobResult&& result)
                                      {
                                          batch = std::get<ResolveResult>(std::move(result));
                                      });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return batch.error.has_value();
                 }));
    CHECK(batch.error == AccessError::internal);
    CHECK(batch.items.empty());
}

TEST_CASE("a suspend and resume job reach the attached backend", "[process]")
{
    GatedAccess access {false};
    access.can_suspend = true;
    AccessWorker worker {access};

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
    CHECK(attached.info->can_suspend);

    bool          suspend_done = false;
    SuspendResult suspended;
    worker.submit_suspend(worker.next_job_id(),
                          7,
                          [&](JobResult&& result)
                          {
                              suspended    = std::get<SuspendResult>(std::move(result));
                              suspend_done = true;
                          });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return suspend_done;
                 }));
    CHECK_FALSE(suspended.error.has_value());
    CHECK(access.backend()->suspends.load() == 1);

    bool          resume_done = false;
    SuspendResult resumed;
    worker.submit_resume(worker.next_job_id(),
                         7,
                         [&](JobResult&& result)
                         {
                             resumed     = std::get<SuspendResult>(std::move(result));
                             resume_done = true;
                         });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return resume_done;
                 }));
    CHECK_FALSE(resumed.error.has_value());
    CHECK(access.backend()->resumes.load() == 1);
}

TEST_CASE("a suspend job without an attached target is refused", "[process]")
{
    GatedAccess access {false};
    access.can_suspend = true;
    AccessWorker worker {access};

    bool          done = false;
    SuspendResult result;
    worker.submit_suspend(worker.next_job_id(),
                          7,
                          [&](JobResult&& job)
                          {
                              result = std::get<SuspendResult>(std::move(job));
                              done   = true;
                          });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return done;
                 }));
    CHECK(result.error == AccessError::unsupported);
}

TEST_CASE("a suspend job refuses a stale target before reaching the backend", "[process]")
{
    GatedAccess access {false};
    access.can_suspend = true;
    AccessWorker worker {access};

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

    bool          done = false;
    SuspendResult result;
    worker.submit_suspend(worker.next_job_id(),
                          8,
                          [&](JobResult&& job)
                          {
                              result = std::get<SuspendResult>(std::move(job));
                              done   = true;
                          });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return done;
                 }));
    CHECK(result.error == AccessError::not_found);
    CHECK(access.backend()->suspends.load() == 0);

    bool          resume_done = false;
    SuspendResult resumed;
    worker.submit_resume(worker.next_job_id(),
                         8,
                         [&](JobResult&& job)
                         {
                             resumed     = std::get<SuspendResult>(std::move(job));
                             resume_done = true;
                         });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return resume_done;
                 }));
    CHECK(resumed.error == AccessError::not_found);
    CHECK(access.backend()->resumes.load() == 0);
}

TEST_CASE("a backend without suspend support reports unsupported", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

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
    CHECK_FALSE(attached.info->can_suspend);

    bool          done = false;
    SuspendResult result;
    worker.submit_suspend(worker.next_job_id(),
                          7,
                          [&](JobResult&& job)
                          {
                              result = std::get<SuspendResult>(std::move(job));
                              done   = true;
                          });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return done;
                 }));
    CHECK(result.error == AccessError::unsupported);
    CHECK(access.backend()->suspends.load() == 0);
}

TEST_CASE("a script job is refused without an attached target", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    const ScriptResult result = run_script(worker, "return 1");

    CHECK(result.description == "helper");
    CHECK_FALSE(result.run.ok);
    CHECK(result.run.error == "no target is attached");
    CHECK(result.run.output.empty());
}

TEST_CASE("a script job reads and writes the attached target's memory", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};
    REQUIRE(attach_target(worker));

    access.backend()->memory->flat[0] = std::byte {0x2A};

    const ScriptResult result = run_script(worker, R"(
print("value", mem.read(0x1000, "u8"))
print(mem.read(0x1001, "u8"))
mem.write(0x1004, "u8", 0x42)
print(mem.pointer_size())
)");

    REQUIRE(result.run.ok);
    REQUIRE(result.run.output.size() == 3);
    CHECK(result.run.output[0] == "value\t42");
    CHECK(result.run.output[1] == "0");
    CHECK(result.run.output[2] == "8");

    // The write really reached the fake target.
    CHECK(access.backend()->memory->flat[4] == std::byte {0x42});
    CHECK(access.backend()->writes.load() >= 1);
}

TEST_CASE("a failing access becomes a script error", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};
    REQUIRE(attach_target(worker));

    const ScriptResult result = run_script(worker, "return mem.read(0x9999, 'u8')");

    CHECK_FALSE(result.run.ok);
    CHECK(result.run.error.find("mem.read") != std::string::npos);
}

TEST_CASE("two script runs on one session share the engine's state", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};
    REQUIRE(attach_target(worker));

    const ScriptResult first = run_script(worker, "counter = 41\nfunction bump() return counter + 1 end");
    REQUIRE(first.run.ok);

    const ScriptResult second = run_script(worker, "return bump()");
    REQUIRE(second.run.ok);
    CHECK(second.run.returned == "42");
}

TEST_CASE("a script job after detach reports the failure instead of running", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};
    REQUIRE(attach_target(worker));

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

    const ScriptResult result = run_script(worker, "return 1");

    CHECK_FALSE(result.run.ok);
    CHECK(result.run.error == "no target is attached");
}

TEST_CASE("a lifecycle job fills the verdict and leaves the plain run empty", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};
    REQUIRE(attach_target(worker));

    SECTION("an activate that refuses reports its message")
    {
        const ScriptResult result = run_lifecycle(
            worker, R"(function activate() return false, "blocked" end)", std::string {slopkit::script::kActivateHook});

        CHECK_FALSE(result.run.ok);
        CHECK(result.run.output.empty());
        REQUIRE(result.lifecycle.has_value());
        CHECK_FALSE(result.lifecycle->ok);
        CHECK(result.lifecycle->message == "blocked");
        CHECK(result.lifecycle->error.empty());
    }

    SECTION("a plain job still fills run and leaves lifecycle empty")
    {
        const ScriptResult result = run_script(worker, "print('x') return 1");

        REQUIRE(result.run.ok);
        CHECK(result.run.returned == "1");
        CHECK_FALSE(result.lifecycle.has_value());
    }

    SECTION("a deactivate job runs the deactivate half")
    {
        const ScriptResult result = run_lifecycle(worker,
                                                  R"(
function activate() return false, "wrong half" end
function deactivate() return true end
)",
                                                  std::string {slopkit::script::kDeactivateHook});

        REQUIRE(result.lifecycle.has_value());
        CHECK(result.lifecycle->ok);
    }
}

TEST_CASE("a lifecycle job is refused without an attached target", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    const ScriptResult result =
        run_lifecycle(worker, "function activate() return true end", std::string {slopkit::script::kActivateHook});

    REQUIRE(result.lifecycle.has_value());
    CHECK_FALSE(result.lifecycle->ok);
    CHECK(result.lifecycle->error == "no target is attached");
}
