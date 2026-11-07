#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <QString>

namespace slopkit::debug
{
    struct CodeFilterCandidate
    {
        std::uint64_t target {};
        std::uint64_t instruction {};
        std::uint64_t breakpoint_id {};
        std::size_t   executions {};
        bool          visible {true};
        bool          restored {};
    };

    enum class CodeFilterState
    {
        idle,
        filtering,
    };

    // The policy of the code filter: which candidates are still interesting,
    // which one is armed and which have been seen. It owns no I/O; the
    // controller turns its answers into breakpoint jobs.
    class CodeFilter
    {
    public:
        [[nodiscard]] CodeFilterState state() const noexcept;
        void                          start(std::vector<CodeFilterCandidate> candidates);
        void                          reset(QString reason = {});
        [[nodiscard]] const QString&  reset_reason() const noexcept;

        [[nodiscard]] std::span<const CodeFilterCandidate> candidates() const noexcept;

        [[nodiscard]] bool covers(std::uint64_t address) const noexcept;
        bool               record(std::uint64_t address);
        void               mark_armed(std::uint64_t address, std::uint64_t breakpoint_id);
        void               mark_restored(std::uint64_t address);

        [[nodiscard]] std::optional<std::uint64_t> next_waiting() const;

        std::size_t drop_unexecuted();
        std::size_t drop_executed();

        [[nodiscard]] std::size_t armed_count() const noexcept;
        [[nodiscard]] std::size_t waiting_count() const noexcept;
        [[nodiscard]] std::size_t executed_count() const noexcept;
        [[nodiscard]] std::size_t visible_count() const noexcept;
        [[nodiscard]] std::size_t dropped_count() const noexcept;

    private:
        CodeFilterState                  state_ {CodeFilterState::idle};
        std::vector<CodeFilterCandidate> candidates_;
        QString                          reset_reason_;
    };
}
