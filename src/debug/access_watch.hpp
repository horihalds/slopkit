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

    struct WatchHit
    {
        std::uint64_t instruction {};
        std::uint32_t tid {};
        std::uint64_t count {1};
    };

    class AccessWatch
    {
    public:
        static constexpr std::size_t kMaxHits = 1024;

        [[nodiscard]] WatchState                state() const noexcept;
        [[nodiscard]] std::uint64_t             address() const noexcept;
        [[nodiscard]] Kind                      kind() const noexcept;
        [[nodiscard]] std::size_t               size() const noexcept;
        [[nodiscard]] std::uint64_t             breakpoint_id() const noexcept;
        [[nodiscard]] std::span<const WatchHit> hits() const noexcept;
        [[nodiscard]] std::uint64_t             hit_count() const noexcept;
        [[nodiscard]] bool                      truncated() const noexcept;

        void start(std::uint64_t address, Kind kind, std::size_t size, std::uint64_t breakpoint_id);

        void mark_armed(bool armed);

        void stop();
        void clear_hits();

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

}
