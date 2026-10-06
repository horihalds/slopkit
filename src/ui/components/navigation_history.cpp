#include "ui/components/navigation_history.hpp"

namespace slopkit::ui::components
{
    bool NavigationHistory::record(std::uint64_t from, std::uint64_t to) noexcept
    {
        if (from == to)
        {
            return false;
        }
        entries_.push_back(from);
        if (entries_.size() > limit)
        {
            entries_.erase(entries_.begin());
        }
        return true;
    }

    std::optional<std::uint64_t> NavigationHistory::back() noexcept
    {
        if (entries_.empty())
        {
            return std::nullopt;
        }
        const std::uint64_t previous = entries_.back();
        entries_.pop_back();
        return previous;
    }

    bool NavigationHistory::can_go_back() const noexcept
    {
        return !entries_.empty();
    }

    void NavigationHistory::clear() noexcept
    {
        entries_.clear();
    }

} // namespace slopkit::ui::components
