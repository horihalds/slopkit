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
    // Where a script's `alloc` and `hook.install` get their memory from. Borrowed
    // bytes are only *probably* free, so wherever the backend can map memory a slot
    // or a payload is a mapping of the target's own, placed within a near jump's
    // reach of the site that names it. Everywhere else — the Linux backend cannot
    // map memory inside the target at all — an allocation is a *cave*: a run of zero
    // bytes in a mapping the target already has, which is what an image's section
    // padding and alignment gaps are. Handing one out changes no target state and
    // needs no in-target allocator, and freeing it hands the same bytes back, so a
    // hook can be installed and removed as often as it is in slopkit.
    //
    // The two uses are not interchangeable, because the trainer writes with
    // `process_vm_writev`/`WriteProcessMemory`, which force a read-only page, while
    // the target's own CPU cannot: a slot the target writes must be in memory the
    // target itself may write, and code the target runs must be in memory it may
    // execute.
    class CaveAllocator
    {
    public:
        explicit CaveAllocator(const Target& target) noexcept : target_ {&target} {}

        CaveAllocator(const CaveAllocator&)            = delete;
        CaveAllocator& operator=(const CaveAllocator&) = delete;

        // Points the allocator at a new run of the target. The caves handed out
        // for the old one mean nothing there, so they are forgotten.
        void rebind(const Target& target) noexcept
        {
            target_ = &target;
            loans_.clear();
            refused_.clear();
        }

        // Hands out `size` bytes the *target* may write: what a script's `alloc`
        // returns for a hook to store into. `near` is the site whose instructions
        // have to address it (`0` accepts any). A backend that can map memory
        // returns a mapping of its own within reach of `near`, which needs no probe;
        // otherwise the cave is zeroed through the trainer's write path first, so
        // the address returned is one it has proven it may write.
        [[nodiscard]] std::expected<std::uint64_t, std::string> allocate(std::size_t size, std::uint64_t near);

        // Hands out `size` bytes the target may *execute*: where `hook.install`
        // writes its payload. `near` is the site its jump starts from (`0` accepts
        // any). A backend that can map executable memory returns a mapping of its
        // own within reach of `near`; otherwise the payload borrows the target's own
        // executable padding, which also keeps its jump short.
        [[nodiscard]] std::expected<std::uint64_t, std::string> allocate_executable(std::size_t   size,
                                                                                    std::uint64_t near);

        // Hands one allocation back: a cave is zeroed so the run is free again, a
        // mapping is unmapped through the backend. An address this session did not
        // hand out is refused, as the app's own allocator refuses a foreign address.
        [[nodiscard]] std::expected<void, std::string> deallocate(std::uint64_t address);

    private:
        // One cave this session handed out; `size` is what the caller asked for, so
        // freeing it zeroes exactly that much, and `mapped` says the bytes are a
        // mapping to unmap rather than a cave to zero.
        struct Loan
        {
            std::uint64_t address {};
            std::size_t   size {};
            bool          mapped {false};
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

        const Target*              target_;
        std::vector<Loan>          loans_;
        std::vector<std::uint64_t> refused_;
    };

}
