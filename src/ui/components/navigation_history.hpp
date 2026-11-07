#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace slopkit::ui::components
{
    class NavigationHistory
    {
    public:
        static constexpr std::size_t limit = 64;

        [[nodiscard]] bool                         record(std::uint64_t from, std::uint64_t to) noexcept;
        [[nodiscard]] std::optional<std::uint64_t> back() noexcept;
        [[nodiscard]] bool                         can_go_back() const noexcept;
        void                                       clear() noexcept;

    private:
        std::vector<std::uint64_t> entries_;
    };

}
