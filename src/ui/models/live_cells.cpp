#include "ui/models/live_cells.hpp"

#include <algorithm>
#include <optional>

namespace slopkit::ui::models
{

    void LiveCells::reset(std::size_t rows)
    {
        cells_.assign(rows, LiveCell {});
        pending_.clear();
    }

    void LiveCells::clear()
    {
        cells_.clear();
        pending_.clear();
    }

    std::size_t LiveCells::size() const noexcept
    {
        return cells_.size();
    }

    const LiveCell* LiveCells::at(std::size_t row) const noexcept
    {
        return row < cells_.size() ? &cells_[row] : nullptr;
    }

    LiveCell& LiveCells::cell(std::size_t row) noexcept
    {
        return cells_[row];
    }

    std::vector<LiveRequest> LiveCells::request(std::size_t                                      rows,
                                                const std::function<LiveCandidate(std::size_t)>& candidate)
    {
        if (cells_.size() != rows)
        {
            cells_.assign(rows, LiveCell {});
        }
        pending_.clear();

        std::vector<LiveRequest> requests;
        requests.reserve(rows);
        pending_.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const LiveCandidate value = candidate(row);
            if (!value.readable)
            {
                continue; // Nothing to read: a zero-width row, or one being written.
            }
            requests.push_back(LiveRequest {.id = row, .address = value.address, .size = value.size});
            pending_.push_back(Pending {.row = row, .identity = value.identity});
        }
        return requests;
    }

    void LiveCells::apply(std::span<const LiveReading>                                        readings,
                          const std::function<bool(std::size_t row, std::uint64_t identity)>& valid,
                          const std::function<void(std::size_t first, std::size_t last)>&     emit_run)
    {
        const std::size_t count = std::min(readings.size(), pending_.size());

        std::optional<std::size_t> run_start;
        std::optional<std::size_t> run_end;
        const auto                 flush = [&]()
        {
            if (run_start.has_value())
            {
                emit_run(*run_start, *run_end);
                run_start.reset();
                run_end.reset();
            }
        };

        for (std::size_t i = 0; i < count; ++i)
        {
            const Pending&     pending = pending_[i];
            const LiveReading& reading = readings[i];
            const std::size_t  row     = pending.row;
            if (reading.id != row || row >= cells_.size())
            {
                continue; // Out of order; leave the row alone.
            }
            if (!valid(row, pending.identity))
            {
                continue; // The row changed identity since the request.
            }

            LiveCell&  cell      = cells_[row];
            const bool was_state = cell.has_reading;
            const bool was_read  = cell.readable;
            const bool was_chg   = cell.changed;
            const auto was_bytes = cell.bytes;

            const bool readable = reading.readable && !reading.bytes.empty();
            if (readable)
            {
                cell.changed = cell.bytes.has_value() && *cell.bytes != reading.bytes;
                cell.bytes   = reading.bytes;
            }
            else
            {
                cell.changed = false;
            }
            cell.has_reading = true;
            cell.readable    = readable;

            if (was_state != cell.has_reading || was_read != cell.readable || was_chg != cell.changed
                || was_bytes != cell.bytes)
            {
                if (!run_start.has_value())
                {
                    run_start = row;
                }
                run_end = row;
            }
            else
            {
                flush();
            }
        }
        flush();

        pending_.clear();
    }

} // namespace slopkit::ui::models
