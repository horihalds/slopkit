#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"

namespace slopkit::ui::dialogs
{

    // A hex dump of the attached target's memory. Reads run on the access worker
    // and the viewer renders one cached page, never touching a session. The
    // disassembler pane is out of scope, so this ships the byte and ASCII view.
    class MemoryViewer
    {
    public:
        // Rows of 16 bytes shown per page.
        static constexpr std::size_t kRowBytes = 16;
        static constexpr std::size_t kRows     = 24;

        MemoryViewer(process::AccessWorker& worker, process::AttachedTarget& target);

        void draw(bool& open);

        // Opens the viewer at `address`, aligned down to a row boundary.
        void set_address(std::uint64_t address);

    private:
        // Submits one page read unless a request is already in flight.
        void request_page(double now);

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;
        std::uint64_t            base_ {};
        std::array<char, 32>     goto_buffer_ {};

        // The last page the worker returned, plus which rows were unreadable.
        std::vector<std::byte>        page_bytes_;
        std::array<bool, kRows>       unreadable_ {};
        std::optional<process::JobId> pending_;
        std::uint64_t                 requested_base_ {0};
        process::ProcessId            requested_pid_ {0};
        double                        next_refresh_time_ {0.0};
        bool                          refresh_requested_ {false};
    };

} // namespace slopkit::ui::dialogs
