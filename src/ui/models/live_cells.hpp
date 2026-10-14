#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "ui/live_values.hpp"

namespace slopkit::ui::models
{
    struct LiveCell
    {
        bool                                  has_reading {false};
        bool                                  readable {false};
        std::optional<std::vector<std::byte>> bytes;
        // One flag per byte of `bytes`, empty while the cell is unreadable.
        // `std::uint8_t` rather than `bool` so the value model can hand out a span.
        std::vector<std::uint8_t>             changed_bytes;
        bool                                  changed {false};
    };

    struct LiveCandidate
    {
        std::uint64_t address {0};
        std::size_t   size {0};
        std::uint64_t identity {0};
        bool          readable {true};
    };

    class LiveCells
    {
    public:
        void reset(std::size_t rows);

        void clear();

        [[nodiscard]] std::size_t     size() const noexcept;
        [[nodiscard]] const LiveCell* at(std::size_t row) const noexcept;
        [[nodiscard]] LiveCell&       cell(std::size_t row) noexcept;

        [[nodiscard]] std::vector<LiveRequest> request(std::size_t                                      rows,
                                                       const std::function<LiveCandidate(std::size_t)>& candidate);

        void apply(std::span<const LiveReading>                                        readings,
                   const std::function<bool(std::size_t row, std::uint64_t identity)>& valid,
                   const std::function<void(std::size_t first, std::size_t last)>&     emit_run);

    private:
        struct Pending
        {
            std::size_t   row {};
            std::uint64_t identity {};
        };

        std::vector<LiveCell> cells_;
        std::vector<Pending>  pending_;
    };

}
