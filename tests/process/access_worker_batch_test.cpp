#include <catch2/catch.hpp>

#include "support/access_worker_helpers.hpp"

TEST_CASE("a batched read returns per-item results in request order", "[process]")
{
    SECTION("with a target attached, each item is read on its own")
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

        access.backend()->memory->flat[0] = std::byte {0x11};
        access.backend()->memory->flat[8] = std::byte {0x33};

        ReadManyResult batch;
        worker.submit_read_many(worker.next_job_id(),
                                std::vector<ReadManyItem> {
                                    {    .address = kBase + 8, .size = 1},
                                    {        .address = kBase, .size = 2},
                                    // Outside the backing store: fails on its own.
                                    {.address = kBase + 0x100, .size = 4},
        },
                                [&](JobResult&& result)
                                {
                                    batch = std::get<ReadManyResult>(std::move(result));
                                });
        REQUIRE(pump(worker,
                     [&]
                     {
                         return batch.items.size() == 3;
                     }));
        REQUIRE(batch.items.size() == 3);
        REQUIRE(batch.items[0].has_value());
        CHECK((*batch.items[0])[0] == std::byte {0x33});
        REQUIRE(batch.items[1].has_value());
        REQUIRE(batch.items[1]->size() == 2);
        CHECK((*batch.items[1])[0] == std::byte {0x11});
        CHECK((*batch.items[1])[1] == std::byte {0x00});
        REQUIRE_FALSE(batch.items[2].has_value());
        CHECK(batch.items[2].error() == AccessError::not_found);
    }

    SECTION("before attach every item fails")
    {
        GatedAccess  access {false};
        AccessWorker worker {access};

        ReadManyResult batch;
        worker.submit_read_many(worker.next_job_id(),
                                std::vector<ReadManyItem> {
                                    {    .address = kBase, .size = 4},
                                    {.address = kBase + 4, .size = 4},
        },
                                [&](JobResult&& result)
                                {
                                    batch = std::get<ReadManyResult>(std::move(result));
                                });
        REQUIRE(pump(worker,
                     [&]
                     {
                         return batch.items.size() == 2;
                     }));
        REQUIRE(batch.items.size() == 2);
        CHECK_FALSE(batch.items[0].has_value());
        CHECK_FALSE(batch.items[1].has_value());
        CHECK(batch.items[0].error() == AccessError::internal);
        CHECK(batch.items[1].error() == AccessError::internal);
    }
}

TEST_CASE("a batched resolve evaluates expressions with pointer chains", "[process]")
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

    // Store a pointer value at the module base: it points at base + 0x10.
    constexpr std::uint64_t kPointer = kBase + 0x10;
    auto&                   memory   = access.backend()->memory->flat;
    for (std::size_t i = 0; i < sizeof(kPointer); ++i)
    {
        memory[i] = static_cast<std::byte>((kPointer >> (8 * i)) & 0xFF);
    }

    const std::vector<slopkit::expr::ModuleRef> modules {
        {"firefox-bin", kBase}
    };

    ResolveResult batch;
    worker.submit_resolve_expressions(worker.next_job_id(),
                                      std::vector<ResolveRequest> {
                                          {.key = 1,             .expression = "0x2000"}, // absolute literal
                                          {.key = 2,   .expression = "firefox-bin+0+20"}, // pointer chain
                                          {.key = 3,     .expression = "firefox-bin+zz"}, // parse failure
                                          {.key = 4, .expression = "firefox-bin+1000+0"}, // unreadable pointer
    },
                                      modules,
                                      8,
                                      [&](JobResult&& result)
                                      {
                                          batch = std::get<ResolveResult>(std::move(result));
                                      });
    REQUIRE(pump(worker,
                 [&]
                 {
                     return batch.items.size() == 4;
                 }));

    REQUIRE_FALSE(batch.error.has_value());
    REQUIRE(batch.items.size() == 4);

    CHECK(batch.items[0].key == 1);
    REQUIRE(batch.items[0].address.has_value());
    CHECK(*batch.items[0].address == 0x2000);
    CHECK(batch.items[0].error.empty());

    CHECK(batch.items[1].key == 2);
    REQUIRE(batch.items[1].address.has_value());
    CHECK(*batch.items[1].address == 0x1010 + 0x20);

    // A bad expression fails only its own item.
    CHECK(batch.items[2].key == 3);
    CHECK_FALSE(batch.items[2].address.has_value());
    CHECK(batch.items[2].error == "invalid offset 'zz'");

    // An unreadable pointer level fails only its own item.
    CHECK(batch.items[3].key == 4);
    CHECK_FALSE(batch.items[3].address.has_value());
    CHECK(batch.items[3].error.find("cannot read pointer at level 1") != std::string::npos);
}
