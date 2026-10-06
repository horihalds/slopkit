#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace slopkit::ui::components
{
    // The Back list one custom-painted pane keeps: an explicit jump records the
    // address it left, scrolling and re-layout do not, and the list is capped so
    // a long session cannot grow it without bound.
    class NavigationHistory
    {
    public:
        // How many previous top addresses a pane remembers for Back.
        static constexpr std::size_t limit = 64;

        // Records `from` for a jump to `to`; returns false and records nothing
        // when `to` is already the current top.
        [[nodiscard]] bool                         record(std::uint64_t from, std::uint64_t to) noexcept;
        // Pops and returns the address remembered last, or nullopt when there is
        // none.
        [[nodiscard]] std::optional<std::uint64_t> back() noexcept;
        [[nodiscard]] bool                         can_go_back() const noexcept;
        void                                       clear() noexcept;

    private:
        // The top addresses visited through navigate_to(), oldest first.
        std::vector<std::uint64_t> entries_;
    };

} // namespace slopkit::ui::components
