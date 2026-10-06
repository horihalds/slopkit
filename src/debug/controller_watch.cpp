#include "debug/controller.hpp"

#include <format>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::debug
{

    const AccessWatch& Controller::watch() const noexcept
    {
        return watch_;
    }

    std::expected<void, std::string> Controller::watch_address(std::uint64_t address, Kind kind, std::size_t size)
    {
        if (state_ != State::stopped && state_ != State::running)
        {
            return std::unexpected(std::string {"Attach to a target first."});
        }

        // One watch at a time: a new one replaces the running one, rows included.
        if (watch_.state() == WatchState::watching)
        {
            slopkit::log::info(slopkit::log::category::debug,
                               std::format("replacing the access watch with {:X}", address));
            stop_watch();
        }

        // A data breakpoint can only watch 1, 2, 4 or 8 bytes.
        const std::size_t bytes = size == 1 || size == 2 || size == 4 || size == 8 ? size : 4;
        auto              id    = breakpoints_.add(std::format("{:X}", address), address, kind, bytes);
        if (!id)
        {
            return std::unexpected(id.error());
        }
        if (Breakpoint* entry = breakpoints_.find(*id); entry != nullptr)
        {
            entry->hidden = true;
        }
        watch_.start(address, kind, bytes, *id);
        watch_dirty_ = true;
        emit watchChanged();
        emit breakpointsChanged();
        slopkit::log::info(slopkit::log::category::debug,
                           std::format("watching {:X} for {} bytes of {} accesses",
                                       address,
                                       bytes,
                                       kind == Kind::hardware_read_write ? "read/write" : "write"));

        if (state_ == State::stopped)
        {
            arm(*id);
        }
        else
        {
            request_slot_op(*id, true);
        }
        return {};
    }

    void Controller::stop_watch()
    {
        if (watch_.state() == WatchState::idle)
        {
            return;
        }
        const std::uint64_t id = watch_.breakpoint_id();
        watch_.stop();
        watch_dirty_ = true;

        if (state_ == State::running)
        {
            if (const Breakpoint* entry = breakpoints_.find(id); entry != nullptr && entry->armed)
            {
                pending_removals_.push_back(id);
                request_slot_op(id, false);
            }
            else
            {
                breakpoints_.remove(id);
            }
        }
        else
        {
            if (const Breakpoint* entry = breakpoints_.find(id); entry != nullptr && entry->armed)
            {
                disarm(id);
            }
            breakpoints_.remove(id);
        }
        emit breakpointsChanged();
        emit watchChanged();
    }

    void Controller::clear_watch_hits()
    {
        watch_.clear_hits();
        emit watchChanged();
    }

} // namespace slopkit::debug
