#include <catch2/catch.hpp>

#include "support/access_worker_helpers.hpp"

namespace
{
    using slopkit::process::ScriptResult;
    using slopkit::process::ScriptUpdatesResult;

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

    // Submits one lifecycle job for a named script and returns its result.
    ScriptResult
    run_lifecycle_as(AccessWorker& worker, std::string description, std::string chunk, std::string function)
    {
        ScriptResult result;
        bool         done = false;
        worker.submit_script(worker.next_job_id(),
                             std::move(description),
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

    // Submits one update pass and returns its result once the completion arrived.
    ScriptUpdatesResult run_updates(AccessWorker& worker)
    {
        ScriptUpdatesResult result;
        bool                done = false;
        worker.submit_script_updates(worker.next_job_id(),
                                     [&](JobResult&& job)
                                     {
                                         result = std::get<ScriptUpdatesResult>(std::move(job));
                                         done   = true;
                                     });
        pump(worker,
             [&]
             {
                 return done;
             });
        return result;
    }

    // The captured records of one category, in order.
    std::vector<slopkit::log::Record> category_records(const std::vector<slopkit::log::Record>& records,
                                                       std::string_view                         category)
    {
        std::vector<slopkit::log::Record> filtered;
        for (const slopkit::log::Record& record : records)
        {
            if (record.category == category)
            {
                filtered.push_back(record);
            }
        }
        return filtered;
    }

    // True when any record's message contains `text`.
    bool any_message_contains(const std::vector<slopkit::log::Record>& records, std::string_view text)
    {
        for (const slopkit::log::Record& record : records)
        {
            if (record.message.find(text) != std::string::npos)
            {
                return true;
            }
        }
        return false;
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

TEST_CASE("a script update pass ticks every active script", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    SECTION("without a target the pass reports an empty result")
    {
        const ScriptUpdatesResult result = run_updates(worker);

        CHECK(result.updated == 0);
        CHECK(result.failed.empty());
    }

    SECTION("two active scripts are updated in activation order")
    {
        REQUIRE(attach_target(worker));

        const ScriptResult first =
            run_lifecycle_as(worker,
                             "first",
                             "function activate() return true end\nfunction update() order = (order or '') .. 'A' end",
                             std::string {slopkit::script::kActivateHook});
        REQUIRE(first.lifecycle.has_value());
        REQUIRE(first.lifecycle->ok);
        const ScriptResult second =
            run_lifecycle_as(worker,
                             "second",
                             "function activate() return true end\nfunction update() order = (order or '') .. 'B' end",
                             std::string {slopkit::script::kActivateHook});
        REQUIRE(second.lifecycle.has_value());
        REQUIRE(second.lifecycle->ok);

        const ScriptUpdatesResult result = run_updates(worker);

        CHECK(result.updated == 2);
        CHECK(result.failed.empty());
        // A later chunk reads what the two hooks left behind: 'AB' proves the
        // pass kept the activation order.
        CHECK(run_script(worker, "return order").run.returned == "AB");
    }

    SECTION("a script without its own update is skipped silently")
    {
        REQUIRE(attach_target(worker));

        const ScriptResult ticker =
            run_lifecycle_as(worker,
                             "ticker",
                             "function activate() return true end\nfunction update() ticks = (ticks or 0) + 1 end",
                             std::string {slopkit::script::kActivateHook});
        REQUIRE(ticker.lifecycle.has_value());
        REQUIRE(ticker.lifecycle->ok);
        const ScriptResult plain = run_lifecycle_as(worker,
                                                    "plain",
                                                    "function activate() return true end\nlocal x = 1",
                                                    std::string {slopkit::script::kActivateHook});
        REQUIRE(plain.lifecycle.has_value());
        REQUIRE(plain.lifecycle->ok);

        const ScriptUpdatesResult result = run_updates(worker);

        CHECK(result.updated == 1);
        CHECK(result.failed.empty());
        // The hook-less chunk never saw the ticker's lingering global.
        CHECK(run_script(worker, "return ticks").run.returned == "1");
    }

    SECTION("a failing update deactivates the script and reports it once")
    {
        REQUIRE(attach_target(worker));

        std::vector<slopkit::log::Record> records;
        SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                                {
                            records.push_back(record);
                                                }};

        const ScriptResult activated = run_lifecycle_as(worker,
                                                        "helper",
                                                        R"(
function activate() return true end
function deactivate()
    print("undone")
    return true
end
function update() return false, "nope" end
)",
                                                        std::string {slopkit::script::kActivateHook});
        REQUIRE(activated.lifecycle.has_value());
        REQUIRE(activated.lifecycle->ok);
        CHECK(worker.active_scripts() == 1);

        const ScriptUpdatesResult result = run_updates(worker);

        CHECK(result.updated == 0);
        REQUIRE(result.failed.size() == 1);
        CHECK(result.failed[0].description == "helper");
        CHECK(result.failed[0].reason == "nope");
        CHECK(worker.active_scripts() == 0);

        // The same job ran deactivate: its printed line and accepted verdict
        // both reached the log, exactly like the shutdown path.
        const auto script = category_records(records, "script");
        CHECK(any_message_contains(script, "undone"));
        CHECK(any_message_contains(script, "script 'helper' deactivated: ok"));

        // The next pass no longer ticks the unticked script.
        records.clear();
        const ScriptUpdatesResult again = run_updates(worker);
        CHECK(again.updated == 0);
        CHECK(again.failed.empty());
        CHECK_FALSE(any_message_contains(category_records(records, "script"), "undone"));
    }

    SECTION("the active-script count mirrors attach, activate and detach")
    {
        CHECK(worker.active_scripts() == 0);

        REQUIRE(attach_target(worker));
        CHECK(worker.active_scripts() == 0);

        const std::string chunk = "function activate() return true end\n"
                                  "function deactivate() return true end\n"
                                  "function update() end";

        const ScriptResult activated =
            run_lifecycle_as(worker, "helper", chunk, std::string {slopkit::script::kActivateHook});
        REQUIRE(activated.lifecycle.has_value());
        REQUIRE(activated.lifecycle->ok);
        CHECK(worker.active_scripts() == 1);

        const ScriptResult deactivated =
            run_lifecycle_as(worker, "helper", chunk, std::string {slopkit::script::kDeactivateHook});
        REQUIRE(deactivated.lifecycle.has_value());
        REQUIRE(deactivated.lifecycle->ok);
        CHECK(worker.active_scripts() == 0);

        const ScriptResult again =
            run_lifecycle_as(worker, "helper", chunk, std::string {slopkit::script::kActivateHook});
        REQUIRE(again.lifecycle.has_value());
        REQUIRE(again.lifecycle->ok);
        CHECK(worker.active_scripts() == 1);

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
        CHECK(worker.active_scripts() == 0);
    }
}

TEST_CASE("an active script is deactivated when the worker stops", "[process]")
{
    GatedAccess access {false};

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    {
        AccessWorker worker {access};
        REQUIRE(attach_target(worker));

        const ScriptResult result = run_lifecycle(worker,
                                                  R"(
function activate() return true end
function deactivate()
    print("shutdown marker")
    return true, "gone"
end
)",
                                                  std::string {slopkit::script::kActivateHook});
        REQUIRE(result.lifecycle.has_value());
        REQUIRE(result.lifecycle->ok);
    }

    // The worker thread stopped and ran the hook while the engine was alive:
    // its printed line and its accepted verdict both reached the log.
    const auto script = category_records(records, "script");
    CHECK(any_message_contains(script, "shutdown marker"));
    CHECK(any_message_contains(script, "script 'helper' deactivated: gone"));
}

TEST_CASE("a refusing deactivate at shutdown logs one warning and still stops the worker", "[process]")
{
    GatedAccess access {false};

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    {
        AccessWorker worker {access};
        REQUIRE(attach_target(worker));

        const ScriptResult result = run_lifecycle(worker,
                                                  R"(
function activate() return true end
function deactivate() return false, "nope" end
)",
                                                  std::string {slopkit::script::kActivateHook});
        REQUIRE(result.lifecycle.has_value());
        REQUIRE(result.lifecycle->ok);
    }

    // The refusal changed nothing and was not retried: exactly one record, a
    // warning carrying the hook's message.
    const auto script = category_records(records, "script");
    REQUIRE(script.size() == 1);
    CHECK(script[0].level == slopkit::log::Level::warning);
    CHECK(script[0].message == "script 'helper' deactivate failed: nope");
}

TEST_CASE("the shutdown deactivation runs in activation order", "[process]")
{
    GatedAccess access {false};

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    {
        AccessWorker worker {access};
        REQUIRE(attach_target(worker));

        const ScriptResult first = run_lifecycle_as(worker,
                                                    "first",
                                                    R"(
function activate() return true end
function deactivate() print("first-out") return true end
)",
                                                    std::string {slopkit::script::kActivateHook});
        REQUIRE(first.lifecycle.has_value());
        REQUIRE(first.lifecycle->ok);

        const ScriptResult second = run_lifecycle_as(worker,
                                                     "second",
                                                     R"(
function activate() return true end
function deactivate() print("second-out") return true end
)",
                                                     std::string {slopkit::script::kActivateHook});
        REQUIRE(second.lifecycle.has_value());
        REQUIRE(second.lifecycle->ok);
    }

    const auto  script    = category_records(records, "script");
    std::size_t first_at  = script.size();
    std::size_t second_at = script.size();
    for (std::size_t i = 0; i < script.size(); ++i)
    {
        if (script[i].message == "first-out")
        {
            first_at = i;
        }
        if (script[i].message == "second-out")
        {
            second_at = i;
        }
    }
    REQUIRE(first_at < script.size());
    REQUIRE(second_at < script.size());
    CHECK(first_at < second_at);
}

TEST_CASE("an unticked script is not deactivated again when the worker stops", "[process]")
{
    GatedAccess access {false};

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    const std::string chunk = R"(
function activate() return true end
function deactivate() print("must not run") return true end
)";

    {
        AccessWorker worker {access};
        REQUIRE(attach_target(worker));

        const ScriptResult activated = run_lifecycle(worker, chunk, std::string {slopkit::script::kActivateHook});
        REQUIRE(activated.lifecycle.has_value());
        REQUIRE(activated.lifecycle->ok);

        const ScriptResult deactivated = run_lifecycle(worker, chunk, std::string {slopkit::script::kDeactivateHook});
        REQUIRE(deactivated.lifecycle.has_value());
        REQUIRE(deactivated.lifecycle->ok);
    }

    CHECK(category_records(records, "script").empty());
}

TEST_CASE("a detached target is not deactivated when the worker stops", "[process]")
{
    GatedAccess access {false};

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    {
        AccessWorker worker {access};
        REQUIRE(attach_target(worker));

        const ScriptResult activated = run_lifecycle(worker,
                                                     R"(
function activate() return true end
function deactivate() print("must not run") return true end
)",
                                                     std::string {slopkit::script::kActivateHook});
        REQUIRE(activated.lifecycle.has_value());
        REQUIRE(activated.lifecycle->ok);

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
    }

    CHECK(category_records(records, "script").empty());
}

TEST_CASE("a refused activate leaves nothing to deactivate when the worker stops", "[process]")
{
    GatedAccess access {false};

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    {
        AccessWorker worker {access};
        REQUIRE(attach_target(worker));

        const ScriptResult refused = run_lifecycle(worker,
                                                   R"(
function activate() return false, "blocked" end
function deactivate() print("must not run") return true end
)",
                                                   std::string {slopkit::script::kActivateHook});
        REQUIRE(refused.lifecycle.has_value());
        REQUIRE_FALSE(refused.lifecycle->ok);
    }

    CHECK(category_records(records, "script").empty());
}

TEST_CASE("a script's symbols resolve in a later resolve job", "[process]")
{
    GatedAccess                  access {false};
    slopkit::script::SymbolTable symbols;
    AccessWorker                 worker {access, symbols};
    REQUIRE(attach_target(worker));

    REQUIRE(run_script(worker, R"(rsymbol("hp", 0x1337))").run.ok);
    REQUIRE(symbols.lookup("hp") == 0x1337);

    ResolveResult batch;
    worker.submit_resolve_expressions(worker.next_job_id(),
                                      std::vector<ResolveRequest> {
                                          {.key = 1, .expression = "hp + 4"}
    },
                                      std::vector<slopkit::expr::ModuleRef> {},
                                      8,
                                      [&](JobResult&& result)
                                      {
                                          batch = std::get<ResolveResult>(std::move(result));
                                      });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return batch.items.size() == 1;
                 }));
    REQUIRE(batch.items[0].address.has_value());
    CHECK(*batch.items[0].address == 0x133B);

    // A second run changes the value the next resolve job sees.
    REQUIRE(run_script(worker, R"(ssymbol("hp", 0x2000))").run.ok);
    ResolveResult second;
    worker.submit_resolve_expressions(worker.next_job_id(),
                                      std::vector<ResolveRequest> {
                                          {.key = 1, .expression = "hp"}
    },
                                      std::vector<slopkit::expr::ModuleRef> {},
                                      8,
                                      [&](JobResult&& result)
                                      {
                                          second = std::get<ResolveResult>(std::move(result));
                                      });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return second.items.size() == 1;
                 }));
    REQUIRE(second.items[0].address.has_value());
    CHECK(*second.items[0].address == 0x2000);
}

TEST_CASE("the symbol table survives a detach", "[process]")
{
    GatedAccess                  access {false};
    slopkit::script::SymbolTable symbols;
    AccessWorker                 worker {access, symbols};
    REQUIRE(attach_target(worker));

    REQUIRE(run_script(worker, R"(rsymbol("hp", 0x55) rsymbol("mp", 0x66))").run.ok);

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

    CHECK(symbols.lookup("hp") == 0x55);
    CHECK(symbols.lookup("mp") == 0x66);

    // A re-attach builds a fresh engine but keeps the names.
    REQUIRE(attach_target(worker));
    REQUIRE(run_script(worker, R"(ssymbol("hp", 0x77))").run.ok);
    CHECK(symbols.lookup("hp") == 0x77);
    CHECK(symbols.lookup("mp") == 0x66);
}

TEST_CASE("aobscan walks the attached target's regions", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};
    REQUIRE(attach_target(worker));

    SECTION("a chunkless scan hits inside the flat window")
    {
        access.backend()->region_list = {
            slopkit::process::RegionInfo {kBase, kBase + 0x40, 0, true, false, false, false, ""}
        };
        access.backend()->module_list        = {slopkit::test::module_image("libgame.so", kBase, 0x40)};
        access.backend()->memory->flat[0x10] = std::byte {0xDE};
        access.backend()->memory->flat[0x11] = std::byte {0xAD};
        access.backend()->memory->flat[0x12] = std::byte {0xBE};
        access.backend()->memory->flat[0x13] = std::byte {0xEF};

        const ScriptResult result = run_script(worker, R"(
local ok, addr = aobscan("origin", "de ad be ef")
print(ok, addr)
)");

        REQUIRE(result.run.ok);
        REQUIRE(result.run.output.size() == 1);
        CHECK(result.run.output[0] == "true\t" + std::to_string(kBase + 0x10));
    }

    SECTION("a module filter restricts the scan to that module")
    {
        access.backend()->region_list = {
            slopkit::process::RegionInfo {       kBase, kBase + 0x20, 0, true, false, false, false, ""},
            slopkit::process::RegionInfo {kBase + 0x20, kBase + 0x40, 0, true, false, false, false, ""}
        };
        access.backend()->module_list        = {slopkit::test::module_image("libother.so", kBase, 0x20),
                                                slopkit::test::module_image("libgame.so", kBase + 0x20, 0x20)};
        access.backend()->memory->flat[0x00] = std::byte {0xCA};
        access.backend()->memory->flat[0x01] = std::byte {0xFE};
        access.backend()->memory->flat[0x30] = std::byte {0xCA};
        access.backend()->memory->flat[0x31] = std::byte {0xFE};

        const ScriptResult unfiltered = run_script(worker, R"(
local _, addr = aobscan("origin", "ca fe")
print(addr)
)");
        REQUIRE(unfiltered.run.ok);
        CHECK(unfiltered.run.output[0] == std::to_string(kBase));

        const ScriptResult filtered = run_script(worker, R"(
local _, addr = aobscan("origin2", "ca fe", "libgame.so")
print(addr)
)");
        REQUIRE(filtered.run.ok);
        CHECK(filtered.run.output[0] == std::to_string(kBase + 0x30));
    }

    SECTION("the default region-less target reports nothing readable")
    {
        const ScriptResult result = run_script(worker, R"(aobscan("origin", "de ad"))");

        CHECK_FALSE(result.run.ok);
        CHECK(result.run.error.find("aobscan: the target reported no readable memory") != std::string::npos);
    }
}

TEST_CASE("a script maps, uses and frees target memory end to end", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};
    access.can_allocate = true;
    REQUIRE(attach_target(worker));

    const ScriptResult result = run_script(worker, R"(
local p = alloc("buf", 64)
print(p)
write(p, 7)
print(read_u8(p))
dealloc("buf")
)");

    REQUIRE(result.run.ok);
    REQUIRE(result.run.output.size() == 2);
    CHECK(result.run.output[0] == std::to_string(0x2000));
    CHECK(result.run.output[1] == "7");
    CHECK(access.backend()->allocates.load() == 1);
    CHECK(access.backend()->frees.load() == 1);
    CHECK(access.backend()->allocations.empty());
}

TEST_CASE("a plugin that cannot allocate memory is reported through the worker", "[process]")
{
    GatedAccess  access {false}; // the fake plugin does not support allocation
    AccessWorker worker {access};
    REQUIRE(attach_target(worker));

    const ScriptResult result = run_script(worker, R"(alloc("buf", 64))");

    CHECK_FALSE(result.run.ok);
    CHECK(result.run.error.find("alloc: the target's plugin cannot allocate memory") != std::string::npos);
}

TEST_CASE("a script job with allocation globals is refused without an attached target", "[process]")
{
    GatedAccess  access {false};
    AccessWorker worker {access};

    const ScriptResult result = run_script(worker, R"(alloc("buf", 64))");

    CHECK_FALSE(result.run.ok);
    CHECK(result.run.error == "no target is attached");
}
