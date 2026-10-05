#include "debug/access_watch.hpp"

#include <algorithm>

namespace slopkit::debug
{

    WatchState AccessWatch::state() const noexcept
    {
        return state_;
    }

    std::uint64_t AccessWatch::address() const noexcept
    {
        return address_;
    }

    Kind AccessWatch::kind() const noexcept
    {
        return kind_;
    }

    std::size_t AccessWatch::size() const noexcept
    {
        return size_;
    }

    std::uint64_t AccessWatch::breakpoint_id() const noexcept
    {
        return breakpoint_id_;
    }

    std::span<const WatchHit> AccessWatch::hits() const noexcept
    {
        return hits_;
    }

    std::uint64_t AccessWatch::hit_count() const noexcept
    {
        return hit_count_;
    }

    bool AccessWatch::truncated() const noexcept
    {
        return truncated_;
    }

    void AccessWatch::start(std::uint64_t address, Kind kind, std::size_t size, std::uint64_t breakpoint_id)
    {
        // Starting a new watch replaces the running one, rows included.
        state_         = WatchState::watching;
        address_       = address;
        kind_          = kind;
        size_          = size;
        breakpoint_id_ = breakpoint_id;
        armed_         = false;
        hit_count_     = 0;
        truncated_     = false;
        hits_.clear();
    }

    void AccessWatch::mark_armed(bool armed)
    {
        armed_ = armed;
        if (!armed && state_ == WatchState::watching)
        {
            state_ = WatchState::stopped;
        }
    }

    void AccessWatch::stop()
    {
        if (state_ != WatchState::idle)
        {
            state_ = WatchState::stopped;
        }
    }

    void AccessWatch::clear_hits()
    {
        hits_.clear();
        hit_count_ = 0;
        truncated_ = false;
    }

    void AccessWatch::record(std::uint32_t tid, std::uint64_t rip)
    {
        ++hit_count_;

        const auto existing = std::find_if(hits_.begin(),
                                           hits_.end(),
                                           [rip](const WatchHit& hit)
                                           {
                                               return hit.instruction == rip;
                                           });
        if (existing != hits_.end())
        {
            ++existing->count;
            return;
        }

        if (hits_.size() >= kMaxHits)
        {
            truncated_ = true;
            return;
        }
        hits_.push_back(WatchHit {rip, tid, 1});
    }

} // namespace slopkit::debug
