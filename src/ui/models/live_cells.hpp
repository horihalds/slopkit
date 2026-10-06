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

    // The live reading of one row.
    struct LiveCell
    {
        bool                                  has_reading {false};
        bool                                  readable {false};
        std::optional<std::vector<std::byte>> bytes;
        bool                                  changed {false};
    };

    // What the owner knows about one row when a pass starts. `identity` is an
    // opaque, model-chosen key that detects a row that recycled itself between
    // the request and the reading.
    struct LiveCandidate
    {
        std::uint64_t address {0};
        std::size_t   size {0};
        std::uint64_t identity {0};
        bool          readable {true};
    };

    // The per-row live state two tables share: the cells, the identity of each
    // in-flight request and the coalescing of changed rows into runs. It emits
    // nothing: each model reports a run through `emit_run` with its own columns
    // and roles.
    class LiveCells
    {
    public:
        // Replaces every cell; the pending list is dropped.
        void reset(std::size_t rows);

        // Drops every cell and the pending list.
        void clear();

        [[nodiscard]] std::size_t     size() const noexcept;
        [[nodiscard]] const LiveCell* at(std::size_t row) const noexcept; // nullptr when out of range
        [[nodiscard]] LiveCell&       cell(std::size_t row) noexcept;     // the caller keeps `row` in range

        // Keeps the cells sized to `rows`, drops the pending list and asks the
        // owner about every row: an unreadable row (zero width, or the entry
        // being written) is skipped, and the rest become one request each, with
        // the row index as the echoed id.
        [[nodiscard]] std::vector<LiveRequest> request(std::size_t                                      rows,
                                                       const std::function<LiveCandidate(std::size_t)>& candidate);

        // Applies one pass. A reading whose id is out of order, whose row is out
        // of range, or whose identity `valid` rejects is dropped. `emit_run` is
        // called once per run of adjacent changed rows, with the first and last
        // row of the run.
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

} // namespace slopkit::ui::models
