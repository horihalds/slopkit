#include "debug/code_filter.hpp"

#include <algorithm>
#include <utility>

namespace slopkit::debug
{
    CodeFilterState CodeFilter::state() const noexcept
    {
        return state_;
    }

    void CodeFilter::start(std::vector<CodeFilterCandidate> candidates)
    {
        candidates_ = std::move(candidates);
        state_      = CodeFilterState::filtering;
        reset_reason_.clear();
    }

    void CodeFilter::reset(QString reason)
    {
        state_ = CodeFilterState::idle;
        candidates_.clear();
        reset_reason_ = std::move(reason);
    }

    const QString& CodeFilter::reset_reason() const noexcept
    {
        return reset_reason_;
    }

    std::span<const CodeFilterCandidate> CodeFilter::candidates() const noexcept
    {
        return candidates_;
    }

    bool CodeFilter::covers(std::uint64_t address) const noexcept
    {
        return std::ranges::any_of(candidates_,
                                   [address](const CodeFilterCandidate& candidate)
                                   {
                                       return candidate.visible && candidate.target == address
                                           && (candidate.breakpoint_id != 0 || candidate.restored);
                                   });
    }

    bool CodeFilter::record(std::uint64_t address)
    {
        for (CodeFilterCandidate& candidate : candidates_)
        {
            if (candidate.visible && candidate.target == address
                && (candidate.breakpoint_id != 0 || candidate.restored))
            {
                ++candidate.executions;
                const bool live         = candidate.breakpoint_id != 0;
                candidate.breakpoint_id = 0;
                return live;
            }
        }
        return false;
    }

    void CodeFilter::mark_restored(std::uint64_t address)
    {
        for (CodeFilterCandidate& candidate : candidates_)
        {
            if (candidate.visible && candidate.target == address)
            {
                candidate.restored = true;
                return;
            }
        }
    }

    void CodeFilter::mark_armed(std::uint64_t address, std::uint64_t breakpoint_id)
    {
        for (CodeFilterCandidate& candidate : candidates_)
        {
            if (candidate.visible && candidate.target == address && candidate.executions == 0)
            {
                candidate.breakpoint_id = breakpoint_id;
                return;
            }
        }
    }

    std::optional<std::uint64_t> CodeFilter::next_waiting() const
    {
        for (const CodeFilterCandidate& candidate : candidates_)
        {
            if (candidate.visible && candidate.breakpoint_id == 0 && candidate.executions == 0)
            {
                return candidate.target;
            }
        }
        return std::nullopt;
    }

    std::size_t CodeFilter::drop_unexecuted()
    {
        std::size_t dropped = 0;
        for (CodeFilterCandidate& candidate : candidates_)
        {
            if (candidate.visible && candidate.executions == 0)
            {
                candidate.visible = false;
                ++dropped;
            }
        }
        return dropped;
    }

    std::size_t CodeFilter::drop_executed()
    {
        std::size_t dropped = 0;
        for (CodeFilterCandidate& candidate : candidates_)
        {
            if (candidate.visible && candidate.executions > 0)
            {
                candidate.visible = false;
                ++dropped;
            }
        }
        return dropped;
    }

    std::size_t CodeFilter::armed_count() const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(candidates_,
                                                              [](const CodeFilterCandidate& candidate)
                                                              {
                                                                  return candidate.visible
                                                                      && candidate.breakpoint_id != 0;
                                                              }));
    }

    std::size_t CodeFilter::waiting_count() const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(candidates_,
                                                              [](const CodeFilterCandidate& candidate)
                                                              {
                                                                  return candidate.visible
                                                                      && candidate.breakpoint_id == 0
                                                                      && candidate.executions == 0;
                                                              }));
    }

    std::size_t CodeFilter::executed_count() const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(candidates_,
                                                              [](const CodeFilterCandidate& candidate)
                                                              {
                                                                  return candidate.visible && candidate.executions > 0;
                                                              }));
    }

    std::size_t CodeFilter::visible_count() const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(candidates_,
                                                              [](const CodeFilterCandidate& candidate)
                                                              {
                                                                  return candidate.visible;
                                                              }));
    }

    std::size_t CodeFilter::dropped_count() const noexcept
    {
        return candidates_.size() - visible_count();
    }
}
