#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "trainer/runtime/target.hpp"

namespace slopkit::trainer
{
    // Where a script's `alloc` gets its memory from. A self-contained trainer
    // cannot map memory inside the target the way the app's plugins do, so an
    // allocation here is a *code cave*: a run of zero bytes in a region the target
    // already mapped executable, which is what an image's section padding and
    // alignment gaps are. Handing one out changes no target state and needs no
    // in-target allocator, and freeing it hands the same bytes back, so a hook can
    // be installed and removed as often as it is in slopkit.
    class CaveAllocator
    {
    public:
        explicit CaveAllocator(const Target& target) noexcept : target_ {target} {}

        CaveAllocator(const CaveAllocator&)            = delete;
        CaveAllocator& operator=(const CaveAllocator&) = delete;

        // Hands out `size` bytes, preferring the cave nearest `near` (`0` accepts
        // any) so a hook's own jump can reach it. The cave is zeroed through the
        // target's own write path first, so the address returned is one the trainer
        // has proven it may write.
        [[nodiscard]] std::expected<std::uint64_t, std::string> allocate(std::size_t size, std::uint64_t near);

        // Hands one allocation back, zeroing its bytes so the run is a free cave
        // again. An address this session did not hand out is refused, as the app's
        // own allocator refuses a foreign address.
        [[nodiscard]] std::expected<void, std::string> deallocate(std::uint64_t address);

    private:
        // One cave this session handed out; `size` is what the caller asked for, so
        // freeing it zeroes exactly that much.
        struct Loan
        {
            std::uint64_t address {};
            std::size_t   size {};
        };

        [[nodiscard]] std::optional<std::uint64_t>
        search(const Region& region, std::size_t size, std::uint64_t near, std::size_t& budget) const;
        [[nodiscard]] std::optional<std::uint64_t>
        scan(std::uint64_t from, std::uint64_t to, std::size_t size, std::uint64_t near, std::size_t& budget) const;
        [[nodiscard]] std::optional<std::uint64_t>
        fit(std::uint64_t run_start, std::size_t run_length, std::size_t size, std::uint64_t near) const;
        [[nodiscard]] bool taken(std::uint64_t address, std::size_t size) const noexcept;
        [[nodiscard]] bool refused(const Region& region) const noexcept;

        const Target&              target_;
        std::vector<Loan>          loans_;
        std::vector<std::uint64_t> refused_;
    };

}
