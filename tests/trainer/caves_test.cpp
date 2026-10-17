#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "trainer/runtime/caves.hpp"
#include "trainer/runtime/target.hpp"

namespace
{
    using slopkit::trainer::CaveAllocator;
    using slopkit::trainer::Module;
    using slopkit::trainer::Region;
    using slopkit::trainer::Target;

    constexpr std::uint64_t kBase {0x1000};

    // A target whose bytes are this test's own. The allocator only reads the
    // regions and writes the cave it chose, so a flat buffer with a filler the
    // test picks is the whole of what it touches.
    class CodeTarget : public Target
    {
    public:
        explicit CodeTarget(std::vector<Region> regions, std::byte filler = std::byte {0x00})
            : regions_ {std::move(regions)}
        {
            std::uint64_t end = kBase;
            for (const Region& region : regions_)
            {
                end = std::max(end, region.end);
            }
            bytes_.assign(static_cast<std::size_t>(end - kBase), filler);
            modules_.push_back(Module {"game.exe", kBase});
        }

        void fill(std::uint64_t address, std::size_t size, std::byte value)
        {
            std::fill_n(bytes_.begin() + static_cast<std::ptrdiff_t>(address - kBase), size, value);
        }

        void refuse_writes()
        {
            writes_allowed_ = false;
        }

        // A backend that can map memory in the target hands out a page of its own.
        // This fake places it where the test picks and remembers taking it back.
        void map_at(std::uint64_t address)
        {
            mapped_ = address;
        }

        [[nodiscard]] std::uint64_t unmapped() const
        {
            return unmapped_;
        }

        [[nodiscard]] std::size_t mapping_requests() const
        {
            return mapping_requests_;
        }

        [[nodiscard]] std::uint32_t pid() const override
        {
            return 1;
        }

        [[nodiscard]] std::uint32_t pointer_size() const override
        {
            return 8;
        }

        [[nodiscard]] const std::vector<Module>& modules() const override
        {
            return modules_;
        }

        [[nodiscard]] const std::vector<Region>& regions() const override
        {
            return regions_;
        }

        [[nodiscard]] std::expected<std::vector<std::byte>, std::string> read(std::uint64_t address,
                                                                              std::size_t   size) const override
        {
            if (!inside(address, size))
            {
                return std::unexpected(std::string {"address is not mapped"});
            }
            const auto offset = static_cast<std::size_t>(address - kBase);
            return std::vector<std::byte>(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                                          bytes_.begin() + static_cast<std::ptrdiff_t>(offset + size));
        }

        [[nodiscard]] std::expected<void, std::string> write(std::uint64_t              address,
                                                             std::span<const std::byte> bytes) const override
        {
            if (!writes_allowed_)
            {
                return std::unexpected(std::string {"the page is not writable"});
            }
            if (!inside(address, bytes.size()))
            {
                return std::unexpected(std::string {"address is not mapped"});
            }
            const auto offset = static_cast<std::size_t>(address - kBase);
            std::copy(bytes.begin(), bytes.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
            return {};
        }

        [[nodiscard]] std::expected<std::uint64_t, std::string> allocate(std::size_t) const override
        {
            ++mapping_requests_;
            if (mapped_ == 0)
            {
                return std::unexpected(std::string {"this target cannot map memory"});
            }
            return mapped_;
        }

        [[nodiscard]] std::expected<void, std::string> deallocate(std::uint64_t address) const override
        {
            if (address != mapped_)
            {
                return std::unexpected(std::string {"not a mapping this trainer made"});
            }
            unmapped_ = address;
            return {};
        }

    private:
        [[nodiscard]] bool inside(std::uint64_t address, std::size_t size) const
        {
            return address >= kBase && address - kBase + size <= bytes_.size();
        }

        mutable std::vector<std::byte> bytes_;
        std::vector<Region>            regions_;
        std::vector<Module>            modules_;
        bool                           writes_allowed_ {true};
        std::uint64_t                  mapped_ {0};
        mutable std::uint64_t          unmapped_ {0};
        mutable std::size_t            mapping_requests_ {0};
    };
}

TEST_CASE("a code cave is a run of zeros in the target's executable memory", "[trainer][caves]")
{
    // Code up to 0x1043, zeros from there: the cave is the zero run, handed out at
    // the 16-byte alignment a hook payload is written at.
    CodeTarget target {{Region {kBase, kBase + 0x80, true, false, true, "game.exe"}}, std::byte {0xCC}};
    target.fill(kBase + 0x43, 0x30, std::byte {0x00});

    CaveAllocator allocator {target};
    const auto    cave = allocator.allocate_executable(0x10, 0);

    REQUIRE(cave.has_value());
    CHECK(*cave == kBase + 0x50);
}

TEST_CASE("only executable memory is searched for a code cave", "[trainer][caves]")
{
    // A data region is mostly zeros and free, but it is no place to write a hook,
    // so nothing is offered.
    CodeTarget target {{Region {kBase, kBase + 0x80, true, true, false, "game.exe"}}};

    CaveAllocator allocator {target};
    const auto    cave = allocator.allocate_executable(0x10, 0);

    REQUIRE_FALSE(cave.has_value());
    CHECK(cave.error().find("no free code cave") != std::string::npos);
}

TEST_CASE("a zero run too small for the request is skipped", "[trainer][caves]")
{
    CodeTarget target {{Region {kBase, kBase + 0x100, true, false, true, "game.exe"}}, std::byte {0xFF}};
    target.fill(kBase, 0x08, std::byte {0x00});
    target.fill(kBase + 0x40, 0x20, std::byte {0x00});

    CaveAllocator allocator {target};
    const auto    cave = allocator.allocate_executable(0x20, 0);

    REQUIRE(cave.has_value());
    CHECK(*cave == kBase + 0x40);
}

TEST_CASE("the cave nearest the site is preferred", "[trainer][caves]")
{
    CodeTarget target {
        {Region {kBase, kBase + 0x40, true, false, true, "far.so"},
         Region {kBase + 0x1000, kBase + 0x1040, true, false, true, "near.so"}}
    };

    CaveAllocator allocator {target};

    const auto near_site = allocator.allocate_executable(0x10, kBase + 0x1080);
    REQUIRE(near_site.has_value());
    CHECK(*near_site == kBase + 0x1000);

    const auto far_site = allocator.allocate_executable(0x10, kBase + 0x10);
    REQUIRE(far_site.has_value());
    CHECK(*far_site == kBase + 0x10);
}

TEST_CASE("freeing a cave hands the same bytes back", "[trainer][caves]")
{
    CodeTarget target {{Region {kBase, kBase + 0x100, true, false, true, "game.exe"}}};

    CaveAllocator allocator {target};

    const auto first  = allocator.allocate_executable(0x20, 0);
    const auto second = allocator.allocate_executable(0x20, 0);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == kBase);
    CHECK(*second == kBase + 0x20);

    // A cave a script holds is not offered again until it frees it.
    const auto held = allocator.allocate_executable(0x20, 0);
    REQUIRE(held.has_value());
    CHECK(*held == kBase + 0x40);

    REQUIRE(allocator.deallocate(*first).has_value());
    const auto reused = allocator.allocate_executable(0x20, 0);
    REQUIRE(reused.has_value());
    CHECK(*reused == *first);
}

TEST_CASE("only this session's allocations can be freed", "[trainer][caves]")
{
    CodeTarget target {{Region {kBase, kBase + 0x100, true, false, true, "game.exe"}}};

    CaveAllocator allocator {target};
    const auto    cave = allocator.allocate_executable(0x20, 0);
    REQUIRE(cave.has_value());

    // An address in the target the allocator never handed out is not its to free,
    // and neither is one that only lies inside an allocation.
    for (const std::uint64_t foreign : {kBase + 0x80, *cave + 0x10})
    {
        const auto freed = allocator.deallocate(foreign);
        REQUIRE_FALSE(freed.has_value());
        CHECK(freed.error() == "not this session's allocation");
    }
}

TEST_CASE("a target that refuses the write offers no cave", "[trainer][caves]")
{
    CodeTarget target {{Region {kBase, kBase + 0x100, true, false, true, "game.exe"}}};
    target.refuse_writes();

    CaveAllocator allocator {target};
    const auto    cave = allocator.allocate_executable(0x10, 0);

    REQUIRE_FALSE(cave.has_value());
    CHECK(cave.error().find("no free code cave") != std::string::npos);
}

TEST_CASE("a cave is offered only within the site's jump reach", "[trainer][caves]")
{
    CodeTarget target {{Region {kBase, kBase + 0x100, true, false, true, "game.exe"}}};

    CaveAllocator allocator {target};

    // The far edge of a near jump's 32-bit displacement still reaches.
    const auto reached = allocator.allocate_executable(0x10, kBase + 0x7FFF0000);
    REQUIRE(reached.has_value());
    CHECK(*reached == kBase);

    // A site beyond it is not served: a hook there could not jump to the cave.
    const auto beyond = allocator.allocate_executable(0x10, kBase + 0x1000'0000'0000);
    REQUIRE_FALSE(beyond.has_value());
    CHECK(beyond.error().find("no free code cave") != std::string::npos);
}

TEST_CASE("a zero-sized allocation is refused", "[trainer][caves]")
{
    CodeTarget target {{Region {kBase, kBase + 0x40, true, false, true, "game.exe"}}};

    CaveAllocator allocator {target};
    const auto    cave = allocator.allocate(0, 0);

    REQUIRE_FALSE(cave.has_value());
    CHECK(cave.error() == "the size must be larger than 0");
}

TEST_CASE("a script's allocation is looked for in writable memory only", "[trainer][caves]")
{
    // The executable region is all zeros and closer to the site, but the target's
    // own CPU cannot write it, so a slot must come from the writable one.
    CodeTarget target {
        {Region {kBase, kBase + 0x200, true, false, true, "game.exe"},
         Region {kBase + 0x1000, kBase + 0x1200, true, true, false, "game.exe"}}
    };

    CaveAllocator allocator {target};
    const auto    slot = allocator.allocate(0x10, kBase);

    REQUIRE(slot.has_value());
    CHECK(*slot == kBase + 0x1000);
}

TEST_CASE("a script's slot is mapped where the target can map memory", "[trainer][caves]")
{
    // The region has a zero run, but it is the target's own memory: it may be the
    // zero-initialized data a running program fills, so a slot a script publishes
    // is not borrowed from it where a mapping of its own is possible.
    CodeTarget target {{Region {kBase, kBase + 0x200, true, true, false, "game.exe"}}};
    target.map_at(0x9000'0000);

    CaveAllocator allocator {target};
    const auto    slot = allocator.allocate(0x10, kBase);

    REQUIRE(slot.has_value());
    CHECK(*slot == 0x9000'0000);

    // Freeing it is the backend's unmap, not a cave zeroed through the write path.
    REQUIRE(allocator.deallocate(*slot).has_value());
    CHECK(target.unmapped() == 0x9000'0000);
}

TEST_CASE("a hook's cave stays the target's own code even where it can map memory", "[trainer][caves]")
{
    // A mapping is placed by the target's allocator, usually out of a near jump's
    // reach, so code is still borrowed from the target's own executable padding.
    CodeTarget target {{Region {kBase, kBase + 0x80, true, false, true, "game.exe"}}};
    target.map_at(0x9000'0000);

    CaveAllocator allocator {target};
    const auto    cave = allocator.allocate_executable(0x10, kBase);

    REQUIRE(cave.has_value());
    CHECK(*cave == kBase);
    CHECK(target.mapping_requests() == 0);
}

TEST_CASE("a hook's cave and a script's slot never overlap", "[trainer][caves]")
{
    // A mapping the target can both write and execute serves either use, so the
    // second request must not hand out the first one's bytes.
    CodeTarget target {{Region {kBase, kBase + 0x100, true, true, true, "game.exe"}}};

    CaveAllocator allocator {target};
    const auto    slot = allocator.allocate(0x10, 0);
    const auto    cave = allocator.allocate_executable(0x10, 0);

    REQUIRE(slot.has_value());
    REQUIRE(cave.has_value());
    CHECK(*slot == kBase);
    CHECK(*cave == kBase + 0x10);
}

TEST_CASE("a named mapping is preferred to an anonymous one", "[trainer][caves]")
{
    // A heap is closer, but the target keeps handing that memory out and may reuse
    // it under a borrowed cave, while a module's own padding stays padding.
    CodeTarget target {
        {Region {kBase, kBase + 0x200, true, true, false, "[heap]"},
         Region {kBase + 0x1000, kBase + 0x1200, true, true, false, "game.exe"}}
    };

    CaveAllocator allocator {target};
    const auto    slot = allocator.allocate(0x10, 0);

    REQUIRE(slot.has_value());
    CHECK(*slot == kBase + 0x1000);
}
