#pragma once

#include <string>

namespace slopkit::table
{

    // Per-table settings, persisted next to the entries. They say which process
    // the table belongs to and what should happen when the table is loaded.
    struct TableSettings
    {
        std::string target_process;         // Process the table belongs to.
        bool        auto_attach {false};    // Attach to it when the table is loaded.
        bool        match_exe_path {false}; // Match the executable path too.

        // True when nothing has been configured, so the writer can omit the line.
        [[nodiscard]] bool empty() const noexcept;

        friend bool operator==(const TableSettings&, const TableSettings&) = default;
    };

} // namespace slopkit::table
