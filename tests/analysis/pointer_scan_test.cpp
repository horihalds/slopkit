#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <span>
#include <vector>

#include "analysis/pointer_scan.hpp"
#include "process/types.hpp"

namespace
{
    using slopkit::analysis::find_pointers;
    using slopkit::analysis::ModuleRange;
    using slopkit::analysis::PointerPath;
    using slopkit::analysis::PointerScanConfig;
    using slopkit::analysis::PointerScanResult;
    using slopkit::analysis::ReadIntoFn;
    using slopkit::process::AccessError;
    using slopkit::process::RegionInfo;

    constexpr std::uint64_t kCodeBase = 0x1000;
    constexpr std::uint64_t kCodeEnd  = 0x2000;
    constexpr std::uint64_t kDataBase = 0x3000;
    constexpr std::uint64_t kDataEnd  = 0x4000;
    constexpr std::uint64_t kHeapBase = 0x5000;
    constexpr std::uint64_t kHeapEnd  = 0x6000;
    constexpr std::uint64_t kTarget   = 0x7000;

    [[nodiscard]] std::vector<RegionInfo> regions(bool heap_readable = true)
    {
        return {
            RegionInfo {.start      = kCodeBase,
                        .end        = kCodeEnd,
                        .offset     = 0,
                        .readable   = true,
                        .writable   = false,
                        .executable = true,
                        .shared     = false,
                        .path       = "mod"   },
            RegionInfo {.start      = kDataBase,
                        .end        = kDataEnd,
                        .offset     = 0,
                        .readable   = true,
                        .writable   = true,
                        .executable = false,
                        .shared     = false,
                        .path       = "mod"   },
            RegionInfo {.start      = kHeapBase,
                        .end        = kHeapEnd,
                        .offset     = 0,
                        .readable   = heap_readable,
                        .writable   = true,
                        .executable = false,
                        .shared     = false,
                        .path       = "[heap]"},
        };
    }

    struct Memory
    {
        std::map<std::uint64_t, std::uint64_t> words;

        void put(std::uint64_t address, std::uint64_t value)
        {
            words[address] = value;
        }

        [[nodiscard]] ReadIntoFn reader() const
        {
            return [this](std::uint64_t address, std::span<std::byte> out) -> std::expected<std::size_t, AccessError>
            {
                for (std::size_t offset = 0; offset + 8 <= out.size(); offset += 8)
                {
                    std::uint64_t value = 0;
                    if (const auto found = words.find(address + offset); found != words.end())
                    {
                        value = found->second;
                    }
                    for (std::size_t index = 0; index < 8; ++index)
                    {
                        out[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 0xFF);
                    }
                }
                return out.size();
            };
        }
    };

    [[nodiscard]] ModuleRange module()
    {
        return ModuleRange {.name = "mod", .base = kCodeBase, .end = kDataEnd};
    }

    [[nodiscard]] bool
    has_path(const PointerScanResult& scan, std::uint64_t base, std::span<const std::int64_t> offsets)
    {
        return std::ranges::any_of(
            scan.paths,
            [&](const PointerPath& path)
            {
                return path.base == base
                    && std::equal(path.offsets.begin(), path.offsets.end(), offsets.begin(), offsets.end());
            });
    }
}

TEST_CASE("a one-level pointer scan keeps only module data bases", "[analysis]")
{
    Memory memory;
    memory.put(0x1100, kTarget); // code: never indexed
    memory.put(0x3010, kTarget); // module data base, exact match
    memory.put(0x3030, kTarget + 0x10);
    memory.put(0x3020, kTarget - 0x20);
    memory.put(0x5000, kTarget); // the heap slot is not a static base

    const PointerScanResult scan =
        find_pointers(memory.reader(), module(), regions(), kTarget, PointerScanConfig {.levels = 1});

    CHECK(has_path(scan, 0x3010, std::vector<std::int64_t> {0}));
    CHECK(has_path(scan, 0x3020, std::vector<std::int64_t> {0x20}));
    CHECK_FALSE(has_path(scan, 0x3030, std::vector<std::int64_t> {-0x10}));
    CHECK_FALSE(has_path(scan, 0x5000, std::vector<std::int64_t> {0}));
    CHECK_FALSE(has_path(scan, 0x1100, std::vector<std::int64_t> {0}));
    CHECK(scan.report.paths == 2);
    CHECK(scan.report.data_regions == 1);
    CHECK(scan.report.regions_scanned == 2);
    CHECK_FALSE(scan.report.truncated);
}

TEST_CASE("negative offsets are opt-in", "[analysis]")
{
    Memory memory;
    memory.put(0x3010, kTarget);
    memory.put(0x3020, kTarget + 0x20);

    const PointerScanResult positive =
        find_pointers(memory.reader(), module(), regions(), kTarget, PointerScanConfig {.levels = 1});
    CHECK(positive.report.paths == 1);

    const PointerScanResult negative = find_pointers(memory.reader(),
                                                     module(),
                                                     regions(),
                                                     kTarget,
                                                     PointerScanConfig {
                                                         .levels         = 1,
                                                         .max_offset     = 0x1000,
                                                         .allow_negative = true,
                                                     });
    CHECK(negative.report.paths == 2);
    CHECK(has_path(negative, 0x3020, std::vector<std::int64_t> {-0x20}));
}

TEST_CASE("a multi-level pointer scan follows the chain back to a static base", "[analysis]")
{
    Memory memory;
    memory.put(0x3000, 0x5008);
    memory.put(0x5008, kTarget - 0x30);

    const PointerScanResult scan =
        find_pointers(memory.reader(), module(), regions(), kTarget, PointerScanConfig {.levels = 2});

    CHECK(has_path(scan, 0x3000, std::vector<std::int64_t> {0, 0x30}));
    CHECK(scan.report.paths >= 1);
}

TEST_CASE("the pointer scan stops at the result cap", "[analysis]")
{
    Memory memory;
    memory.put(0x3010, kTarget);
    memory.put(0x3020, kTarget);

    const PointerScanResult scan =
        find_pointers(memory.reader(), module(), regions(), kTarget, PointerScanConfig {.levels = 1, .max_results = 1});

    CHECK(scan.paths.size() == 1);
    CHECK(scan.report.truncated);
}

TEST_CASE("an unreadable region is skipped and counted", "[analysis]")
{
    Memory memory;
    memory.put(0x3010, kTarget);

    const PointerScanResult scan =
        find_pointers(memory.reader(), module(), regions(false), kTarget, PointerScanConfig {.levels = 1});

    CHECK(has_path(scan, 0x3010, std::vector<std::int64_t> {0}));
    CHECK(scan.report.regions_scanned == 1);
}

TEST_CASE("values that cannot reach the target or a writable slot are not indexed", "[analysis]")
{
    Memory memory;
    memory.put(0x3010, 0x9000);

    const PointerScanResult scan =
        find_pointers(memory.reader(), module(), regions(), kTarget, PointerScanConfig {.levels = 1});

    CHECK(scan.paths.empty());
    CHECK(scan.report.pointer_slots > 0);
    CHECK(scan.report.candidate_slots == 0);
}

TEST_CASE("overlapping writable regions are indexed once", "[analysis]")
{
    Memory memory;
    memory.put(0x3010, kTarget);

    std::vector<RegionInfo> duplicated = regions();
    duplicated.push_back(duplicated[1]);

    const PointerScanResult scan =
        find_pointers(memory.reader(), module(), duplicated, kTarget, PointerScanConfig {.levels = 1});

    REQUIRE(scan.paths.size() == 1);
    CHECK(scan.paths.front().base == 0x3010);
}

TEST_CASE("shared intermediate slots keep one path per base", "[analysis]")
{
    Memory memory;
    memory.put(0x3000, 0x5000);
    memory.put(0x3008, 0x5000);
    memory.put(0x5000, kTarget);

    const PointerScanResult scan =
        find_pointers(memory.reader(), module(), regions(), kTarget, PointerScanConfig {.levels = 2});

    REQUIRE(scan.paths.size() == 2);
    CHECK(scan.paths[0].base == 0x3000);
    CHECK(scan.paths[1].base == 0x3008);
    CHECK(scan.paths[0].offsets == std::vector<std::int64_t> {0, 0});
    CHECK(scan.paths[1].offsets == std::vector<std::int64_t> {0, 0});
}

TEST_CASE("the thread count does not change the result", "[analysis]")
{
    Memory memory;
    memory.put(0x3000, 0x5008);
    memory.put(0x5008, kTarget - 0x30);
    memory.put(0x3020, kTarget);
    memory.put(0x5010, kTarget - 0x10);

    const PointerScanResult single =
        find_pointers(memory.reader(), module(), regions(), kTarget, PointerScanConfig {.levels = 2, .threads = 1});
    const PointerScanResult parallel =
        find_pointers(memory.reader(), module(), regions(), kTarget, PointerScanConfig {.levels = 2, .threads = 4});

    REQUIRE(single.paths.size() == parallel.paths.size());
    for (std::size_t index = 0; index < single.paths.size(); ++index)
    {
        CHECK(single.paths[index].base == parallel.paths[index].base);
        CHECK(single.paths[index].offsets == parallel.paths[index].offsets);
    }
}

TEST_CASE("the level count is an upper bound", "[analysis]")
{
    Memory memory;
    memory.put(0x3010, kTarget);
    memory.put(0x3020, 0x5000);
    memory.put(0x5000, kTarget);

    const PointerScanResult scan =
        find_pointers(memory.reader(), module(), regions(), kTarget, PointerScanConfig {.levels = 3});

    CHECK(has_path(scan, 0x3010, std::vector<std::int64_t> {0}));
    CHECK(has_path(scan, 0x3020, std::vector<std::int64_t> {0, 0}));
    CHECK(scan.paths.size() == 2);
}
