#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "debug/breakpoints.hpp"

namespace slopkit::debug
{

    enum class WatchState
    {
        idle,
        watching,
        stopped,
    };

    // One instruction that touched the watched address. `instruction` is the RIP
    // as the stop reported it, which on x86-64 is one past the accessing
    // instruction; the UI recovers the real instruction with a backward decode.
    struct WatchHit
    {
        std::uint64_t instruction {};
        std::uint32_t tid {};
        std::uint64_t count {1};
    };

    // The state and the coalesced hits of one address watch. It knows nothing
    // about the worker or the session; the Controller arms the hardware slot and
    // feeds every stop in, and the window renders what comes out.
    class AccessWatch
    {
    public:
        // A tight loop can touch the watched address thousands of times a second,
        // so distinct instructions are capped; the total keeps counting.
        static constexpr std::size_t kMaxHits = 1024;

        [[nodiscard]] WatchState                state() const noexcept;
        [[nodiscard]] std::uint64_t             address() const noexcept;
        [[nodiscard]] Kind                      kind() const noexcept;
        [[nodiscard]] std::size_t               size() const noexcept;
        [[nodiscard]] std::uint64_t             breakpoint_id() const noexcept;
        [[nodiscard]] std::span<const WatchHit> hits() const noexcept;
        [[nodiscard]] std::uint64_t             hit_count() const noexcept;
        [[nodiscard]] bool                      truncated() const noexcept;

        // Begins (or, when one is running, replaces) the watch on `address`.
        void start(std::uint64_t address, Kind kind, std::size_t size, std::uint64_t breakpoint_id);

        // Records whether the underlying slot is armed, so a failed arming can end
        // the watch.
        void mark_armed(bool armed);

        // Ends the watch without dropping the recorded rows; `clear_hits` empties
        // the rows on their own.
        void stop();
        void clear_hits();

        // Records one stop of the watched slot. Always consumes the stop, so the
        // caller can resume instead of reporting it; the row count stops growing
        // at `kMaxHits` while the total keeps rising.
        void record(std::uint32_t tid, std::uint64_t rip);

    private:
        WatchState            state_ {WatchState::idle};
        std::uint64_t         address_ {};
        Kind                  kind_ {Kind::hardware_write};
        std::size_t           size_ {4};
        std::uint64_t         breakpoint_id_ {};
        bool                  armed_ {false};
        std::uint64_t         hit_count_ {};
        bool                  truncated_ {false};
        std::vector<WatchHit> hits_;
    };

} // namespace slopkit::debug
