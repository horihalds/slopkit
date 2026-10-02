#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "process/access.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "scan/engine.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"

namespace slopkit::ui::panels
{

    // The right half of the middle zone: the scan controls. It builds the
    // ScanConfig, owns the engine and the worker session the engine reads
    // through, and never blocks the render loop.
    class ScannerPanel
    {
    public:
        ScannerPanel(process::AccessWorker& worker, process::AttachedTarget& target);

        void draw();

        [[nodiscard]] scan::ScanEngine& engine() noexcept
        {
            return engine_;
        }

        // Sets the fast-scan alignment field, used by the Settings dialog.
        void set_default_alignment(std::uint64_t alignment);

        // A request to open the Memory Viewer at an address.
        [[nodiscard]] std::optional<std::uint64_t> take_memory_view_request();

        // A request to open the Add Address dialog.
        [[nodiscard]] bool take_add_address_request();

    private:
        void request_scan_session();
        void start_first_scan();
        void start_next_scan();

        [[nodiscard]] scan::ScanType                               current_scan_type() const noexcept;
        [[nodiscard]] scan::ValueType                              current_value_type() const noexcept;
        [[nodiscard]] std::expected<scan::ScanConfig, std::string> build_config() const;

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;

        // Inputs.
        std::array<char, 64> value_buffer_ {};
        std::array<char, 64> value_upper_buffer_ {};
        bool                 hex_ {false};
        int                  scan_type_ {0};
        int                  value_type_ {2};

        std::array<char, 32> start_address_ {};
        std::array<char, 32> stop_address_ {};
        std::array<char, 16> alignment_ {"4"};
        bool                 writable_ {true};
        bool                 executable_ {false};
        bool                 copy_on_write_ {true};
        bool                 fast_scan_ {true};
        bool                 pause_while_scanning_ {false};

        // Placeholder toggles for the not-yet-implemented extra options.
        bool lua_formula_ {false};
        bool not_operator_ {false};
        bool unrandomizer_ {false};
        bool speedhack_ {false};

        std::optional<std::uint64_t> memory_view_request_;
        bool                         add_address_request_ {false};

        std::string status_;
        bool        status_is_error_ {false};

        // A session separate from the app-wide one, used only by the scan
        // worker so the shared session is never touched from two threads. It is
        // declared before the engine so the engine joins its worker before the
        // session is destroyed. The session is created by an access-worker
        // handoff job and applied when that completion is drained.
        process::Session              worker_session_;
        process::ProcessId            worker_pid_ {0};
        std::string                   worker_plugin_;
        std::optional<process::JobId> handoff_pending_;
        scan::ScanEngine              engine_;
    };

} // namespace slopkit::ui::panels
