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
    // Where a script's `alloc` and `hook.install` get their memory from. A
    // self-contained trainer cannot map memory inside the target the way the app's
    // plugins do, so an allocation here is a *cave*: a run of zero bytes in a
    // mapping the target already has, which is what an image's section padding and
    // alignment gaps are. Handing one out changes no target state and needs no
    // in-target allocator, and freeing it hands the same bytes back, so a hook can
    // be installed and removed as often as it is in slopkit.
    //
    // The two uses are not interchangeable, because the trainer writes with
    // `process_vm_writev`/`WriteProcessMemory`, which force a read-only page, while
    // the target's own CPU cannot: a slot the target writes must be in memory the
    // target itself may write, and code the target runs must be in memory it may
    // execute.
    class CaveAllocator
    {
    public:
        explicit CaveAllocator(const Target& target) noexcept : target_ {target} {}

        CaveAllocator(const CaveAllocator&)            = delete;
        CaveAllocator& operator=(const CaveAllocator&) = delete;

        // Hands out `size` bytes the *target* may write: what a script's `alloc`
        // returns for a hook to store into. `near` is the site whose instructions
        // have to reach the cave (`0` accepts any). The cave is zeroed through the
        // trainer's write path first, so the address returned is one it has proven
        // it may write.
        [[nodiscard]] std::expected<std::uint64_t, std::string> allocate(std::size_t size, std::uint64_t near);

        // Hands out `size` bytes the target may *execute*: where `hook.install`
        // writes its payload. A target that maps its allocations both ways need not
        // tell this apart from `allocate`, so a missing seam falls back to it.
        [[nodiscard]] std::expected<std::uint64_t, std::string> allocate_executable(std::size_t   size,
                                                                                    std::uint64_t near);

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

        enum class Use
        {
            writable,
            executable,
        };

        [[nodiscard]] std::expected<std::uint64_t, std::string> take(Use use, std::size_t size, std::uint64_t near);
        [[nodiscard]] static bool                               usable(const Region& region, Use use) noexcept;

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
