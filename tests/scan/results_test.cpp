#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <type_traits>
#include <vector>

#include "scan/results.hpp"

namespace
{
    std::vector<std::byte> bytes_of(std::size_t count, std::uint8_t seed)
    {
        std::vector<std::byte> bytes(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            bytes[i] = static_cast<std::byte>((seed + i) & 0xFF);
        }
        return bytes;
    }

    slopkit::scan::ScanHit append(slopkit::scan::ScanResults&   set,
                                  std::uint64_t                 address,
                                  const std::vector<std::byte>& value,
                                  const std::vector<std::byte>& before)
    {
        set.hits.push_back(slopkit::scan::make_hit(address, value, before, set.blob));
        return set.hits.back();
    }
}

TEST_CASE("a hit record is 32 bytes and trivially copyable", "[scan]")
{
    STATIC_REQUIRE(sizeof(slopkit::scan::ScanHit) == 32);
    STATIC_REQUIRE(std::is_trivially_copyable_v<slopkit::scan::ScanHit>);
}

TEST_CASE("an inline value round-trips through the accessors", "[scan]")
{
    for (const std::size_t width : {std::size_t {1}, std::size_t {2}, std::size_t {4}, std::size_t {8}})
    {
        const std::vector<std::byte> value = bytes_of(width, 0x10);

        slopkit::scan::ScanResults set;
        const auto                 hit = append(set, 0x1000, value, {});

        CHECK(hit.width == width);
        CHECK(hit.wide == 0);
        CHECK(hit.readable == 1);
        CHECK(set.blob.empty());
        CHECK(std::ranges::equal(set.value(hit), value));
        CHECK(set.previous(hit).empty());
        CHECK(set.bytes() == sizeof(slopkit::scan::ScanHit));
    }
}

TEST_CASE("a wide value travels through the blob", "[scan]")
{
    for (const std::size_t width : {std::size_t {9}, std::size_t {16}, std::size_t {40}})
    {
        const std::vector<std::byte> value = bytes_of(width, 0x20);

        slopkit::scan::ScanResults set;
        const auto                 hit = append(set, 0x2000, value, {});

        CHECK(hit.wide == 1);
        CHECK(hit.blob_offset == 0);
        CHECK(set.blob.size() == width);
        CHECK(std::ranges::equal(set.value(hit), value));
        CHECK(set.previous(hit).empty());
        CHECK(set.bytes() == sizeof(slopkit::scan::ScanHit) + width);
    }
}

TEST_CASE("a refinement set keeps the previous value bytes", "[scan]")
{
    const std::vector<std::byte> value  = bytes_of(9, 0x30);
    const std::vector<std::byte> before = bytes_of(9, 0x60);

    slopkit::scan::ScanResults set;
    set.keeps_previous = true;
    const auto hit     = append(set, 0x3000, value, before);

    CHECK(set.blob.size() == 18);
    CHECK(std::ranges::equal(set.value(hit), value));
    CHECK(std::ranges::equal(set.previous(hit), before));
}

TEST_CASE("an inline refinement keeps the previous value inline", "[scan]")
{
    const std::vector<std::byte> value  = bytes_of(4, 0x40);
    const std::vector<std::byte> before = bytes_of(4, 0x70);

    slopkit::scan::ScanResults set;
    set.keeps_previous = true;
    const auto hit     = append(set, 0x4000, value, before);

    CHECK(hit.wide == 0);
    CHECK(set.blob.empty());
    CHECK(std::ranges::equal(set.value(hit), value));
    CHECK(std::ranges::equal(set.previous(hit), before));
}

TEST_CASE("several wide values each address their own blob slice", "[scan]")
{
    const std::vector<std::byte> first  = bytes_of(12, 0x01);
    const std::vector<std::byte> second = bytes_of(12, 0x81);

    slopkit::scan::ScanResults set;
    const auto                 left  = append(set, 0x5000, first, {});
    const auto                 right = append(set, 0x6000, second, {});

    CHECK(right.blob_offset == 12);
    CHECK(std::ranges::equal(set.value(left), first));
    CHECK(std::ranges::equal(set.value(right), second));
    CHECK(set.bytes() == 2 * sizeof(slopkit::scan::ScanHit) + 24);
}

TEST_CASE("head builds a self-contained page", "[scan]")
{
    const std::vector<std::byte> first  = bytes_of(12, 0x01);
    const std::vector<std::byte> second = bytes_of(20, 0x41);
    const std::vector<std::byte> third  = bytes_of(12, 0x81);

    slopkit::scan::ScanResults set;
    append(set, 0x1000, first, {});
    append(set, 0x2000, second, {});
    append(set, 0x3000, third, {});

    const slopkit::scan::ScanResults page = set.head(2);

    REQUIRE(page.hits.size() == 2);
    CHECK_FALSE(page.keeps_previous);
    CHECK(page.hits[0].address == 0x1000);
    CHECK(page.hits[1].address == 0x2000);
    CHECK(page.blob.size() == 32);
    CHECK(page.hits[0].blob_offset == 0);
    CHECK(page.hits[1].blob_offset == 12);
    CHECK(std::ranges::equal(page.value(page.hits[0]), first));
    CHECK(std::ranges::equal(page.value(page.hits[1]), second));
    CHECK(page.bytes() == 2 * sizeof(slopkit::scan::ScanHit) + 32);

    // The page owns its bytes: mutating the full set cannot change it.
    set.blob.assign(set.blob.size(), std::byte {0});
    CHECK(std::ranges::equal(page.value(page.hits[1]), second));
}

TEST_CASE("head keeps the previous bytes of its wide rows", "[scan]")
{
    const std::vector<std::byte> value  = bytes_of(9, 0x11);
    const std::vector<std::byte> before = bytes_of(9, 0x51);

    slopkit::scan::ScanResults set;
    set.keeps_previous = true;
    append(set, 0x7000, value, before);
    append(set, 0x8000, value, before);

    const slopkit::scan::ScanResults page = set.head(1);

    REQUIRE(page.hits.size() == 1);
    CHECK(page.blob.size() == 18);
    CHECK(std::ranges::equal(page.value(page.hits[0]), value));
    CHECK(std::ranges::equal(page.previous(page.hits[0]), before));
}

TEST_CASE("head of an empty or shorter set is well behaved", "[scan]")
{
    const slopkit::scan::ScanResults empty;
    CHECK(empty.head(8).hits.empty());
    CHECK(empty.head(8).blob.empty());
    CHECK(empty.bytes() == 0);

    const std::vector<std::byte> value = bytes_of(4, 0x01);

    slopkit::scan::ScanResults set;
    const auto                 hit = append(set, 0x9000, value, {});

    const slopkit::scan::ScanResults page = set.head(256);
    REQUIRE(page.hits.size() == 1);
    CHECK(page.hits[0].address == hit.address);
    CHECK(std::ranges::equal(page.value(page.hits[0]), value));
}
