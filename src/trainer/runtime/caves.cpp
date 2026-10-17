#include "trainer/runtime/caves.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::trainer
{
    namespace
    {
        // A zero byte is what an image's section padding and alignment gaps are made
        // of, so a zero run inside a mapping the target already has is memory nobody
        // wrote.
        constexpr std::byte kFree {0x00};

        // The reach between a hook's site and its cave, which is a near jump's signed
        // 32-bit displacement (`kCaveReach` in `src/script/hook.cpp`).
        constexpr std::uint64_t kReach {0x7FFF0000};

        // The alignment `script::hook` sizes its caves to and writes its payload at.
        constexpr std::uint64_t kAlign {16};

        // Reading a target's memory costs real time and a target can map gigabytes
        // of it, so one allocation looks at this much before giving up. A cave is
        // section padding, so it is found long before that.
        constexpr std::size_t kScanBudget {64 * 1024 * 1024};
        constexpr std::size_t kChunk {256 * 1024};

        [[nodiscard]] std::uint64_t align_up(std::uint64_t value)
        {
            return (value + kAlign - 1) / kAlign * kAlign;
        }

        // How far `near` is from a region: 0 when it lies inside it.
        [[nodiscard]] std::uint64_t distance(std::uint64_t near, const Region& region)
        {
            if (near < region.start)
            {
                return region.start - near;
            }
            return near < region.end ? 0 : near - region.end;
        }

        [[nodiscard]] bool within_reach(std::uint64_t near, std::uint64_t address)
        {
            return (near > address ? near - address : address - near) <= kReach;
        }

        [[nodiscard]] bool out_of_reach(std::uint64_t near, const Region& region)
        {
            return near != 0 && distance(near, region) > kReach;
        }

        // A mapping the target has a name for (a module's own region, or a mapped
        // file) is padding it keeps, while a bracketed one is a heap, stack or JIT
        // area it is still handing out and may reuse under a borrowed cave.
        [[nodiscard]] bool named(const Region& region)
        {
            return !region.name.empty() && region.name.front() != '[';
        }
    }

    std::expected<std::uint64_t, std::string> CaveAllocator::allocate(std::size_t size, std::uint64_t near)
    {
        return take(Use::writable, size, near);
    }

    std::expected<std::uint64_t, std::string> CaveAllocator::allocate_executable(std::size_t size, std::uint64_t near)
    {
        return take(Use::executable, size, near);
    }

    bool CaveAllocator::usable(const Region& region, Use use) noexcept
    {
        if (!region.readable)
        {
            return false;
        }
        return use == Use::executable ? region.executable : region.writable;
    }

    std::expected<std::uint64_t, std::string> CaveAllocator::take(Use use, std::size_t size, std::uint64_t near)
    {
        const auto kind  = use == Use::executable ? "code" : "data";
        const auto pages = use == Use::executable ? "executable" : "writable";

        if (size == 0)
        {
            return std::unexpected(std::string {"the size must be larger than 0"});
        }

        // Borrowed bytes are only *probably* free: a module's zero-initialized data
        // is zero at attach time and filled as the target runs, and a game that
        // writes over a slot turns a published pointer into whatever it stored
        // there. A slot a script publishes — and the payload a hook installs — is
        // therefore a mapping of the target's own wherever the backend can make one,
        // within a near jump's reach of `near`, because the code that names it can
        // reach no further, and a borrowed cave only where it cannot map at all.
        const auto mapped =
            use == Use::executable ? target_->allocate_executable(size, near) : target_->allocate(size, near);
        if (mapped)
        {
            loans_.push_back(Loan {*mapped, size, true});
            log::debug(log::category::script, std::format("alloc: mapped {} byte(s) at 0x{:x}", size, *mapped));
            return *mapped;
        }
        log::debug(log::category::script,
                   std::format("alloc: no mapping to hand out ({}), looking for a cave", mapped.error()));

        std::vector<const Region*> candidates;
        for (const Region& region : target_->regions())
        {
            if (!usable(region, use) || region.end <= region.start || refused(region) || out_of_reach(near, region))
            {
                continue;
            }
            candidates.push_back(&region);
        }
        std::ranges::sort(candidates,
                          [near](const Region* left, const Region* right)
                          {
                              if (named(*left) != named(*right))
                              {
                                  return named(*left);
                              }
                              return distance(near, *left) < distance(near, *right);
                          });

        std::size_t budget = kScanBudget;
        for (const Region* region : candidates)
        {
            const std::optional<std::uint64_t> found = search(*region, size, near, budget);
            if (!found)
            {
                continue;
            }

            // Zero the cave through the trainer's write path: an address it cannot
            // write is no use to a hook, and a region whose pages refuse that write
            // is not searched again. That path forces a read-only page, so the region
            // flags above, not this probe, decide whether the target may use the cave.
            const std::vector<std::byte> zeros(size, kFree);
            if (const auto wrote = target_->write(*found, zeros); !wrote)
            {
                log::debug(log::category::script,
                           std::format("alloc: region 0x{:x} ({}) refuses the write probe: {}",
                                       region->start,
                                       region->name,
                                       wrote.error()));
                refused_.push_back(region->start);
                continue;
            }
            loans_.push_back(Loan {*found, size, false});
            log::debug(log::category::script,
                       std::format("alloc: {} {} cave of {} byte(s) at 0x{:x}", pages, kind, size, *found));
            return *found;
        }

        const auto message =
            std::format("the target has no free {} cave of {} byte(s) in its {} memory", kind, size, pages);
        log::warning(log::category::script, message);
        return std::unexpected(message);
    }

    std::expected<void, std::string> CaveAllocator::deallocate(std::uint64_t address)
    {
        const auto loan = std::ranges::find(loans_, address, &Loan::address);
        if (loan == loans_.end())
        {
            log::debug(log::category::script, std::format("free: 0x{:x} is not this session's allocation", address));
            return std::unexpected(std::string {"not this session's allocation"});
        }

        if (loan->mapped)
        {
            const std::expected<void, std::string> unmapped = target_->deallocate(address);
            loans_.erase(loan);
            if (!unmapped)
            {
                log::warning(log::category::script,
                             std::format("free: 0x{:x} could not be unmapped: {}", address, unmapped.error()));
                return std::unexpected(unmapped.error());
            }
            log::debug(log::category::script, std::format("free: 0x{:x} unmapped", address));
            return {};
        }

        const std::vector<std::byte>           zeros(loan->size, kFree);
        const std::expected<void, std::string> wrote = target_->write(address, zeros);
        loans_.erase(loan);
        if (!wrote)
        {
            log::warning(log::category::script,
                         std::format("free: 0x{:x} could not be zeroed: {}", address, wrote.error()));
            return std::unexpected(wrote.error());
        }
        log::debug(log::category::script, std::format("free: 0x{:x}", address));
        return {};
    }

    std::optional<std::uint64_t>
    CaveAllocator::search(const Region& region, std::size_t size, std::uint64_t near, std::size_t& budget) const
    {
        // When the caller named a site inside this region, start at the site: a cave
        // in the image's own neighbourhood keeps the hook's jump short.
        if (near >= region.start && near < region.end)
        {
            if (const std::optional<std::uint64_t> after = scan(near, region.end, size, near, budget))
            {
                return after;
            }
            return scan(region.start, near, size, near, budget);
        }
        return scan(region.start, region.end, size, near, budget);
    }

    std::optional<std::uint64_t> CaveAllocator::scan(
        std::uint64_t from, std::uint64_t to, std::size_t size, std::uint64_t near, std::size_t& budget) const
    {
        std::uint64_t address    = from;
        std::uint64_t run_start  = from;
        std::size_t   run_length = 0;

        while (address < to && budget > 0)
        {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, to - address));
            const auto bytes = target_->read(address, chunk);
            if (!bytes)
            {
                // Nothing past a page the target will not read is the trainer's to
                // hand out.
                break;
            }
            budget -= bytes->size();

            for (std::size_t index = 0; index < bytes->size(); ++index)
            {
                if ((*bytes)[index] != kFree)
                {
                    if (const std::optional<std::uint64_t> cave = fit(run_start, run_length, size, near))
                    {
                        return cave;
                    }
                    run_length = 0;
                    continue;
                }
                if (run_length == 0)
                {
                    run_start = address + index;
                }
                ++run_length;
            }
            address += bytes->size();
        }

        return fit(run_start, run_length, size, near);
    }

    std::optional<std::uint64_t>
    CaveAllocator::fit(std::uint64_t run_start, std::size_t run_length, std::size_t size, std::uint64_t near) const
    {
        if (run_length == 0)
        {
            return std::nullopt;
        }

        std::uint64_t address = align_up(run_start);
        std::size_t   offset  = static_cast<std::size_t>(address - run_start);

        // A cave a script already holds is not free, so one run holds as many caves
        // as its length and alignment allow.
        while (run_length >= offset + size)
        {
            if ((near == 0 || within_reach(near, address)) && !taken(address, size))
            {
                return address;
            }
            address += kAlign;
            offset += kAlign;
        }
        return std::nullopt;
    }

    bool CaveAllocator::taken(std::uint64_t address, std::size_t size) const noexcept
    {
        return std::ranges::any_of(loans_,
                                   [address, size](const Loan& loan)
                                   {
                                       return address < loan.address + loan.size && loan.address < address + size;
                                   });
    }

    bool CaveAllocator::refused(const Region& region) const noexcept
    {
        return std::ranges::find(refused_, region.start) != refused_.end();
    }
}
