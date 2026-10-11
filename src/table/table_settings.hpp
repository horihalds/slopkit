#pragma once

#include <string>

namespace slopkit::table
{
    struct TableSettings
    {
        std::string target_process;
        std::string exe_path;
        bool        auto_attach {false};
        bool        match_exe_path {false};

        [[nodiscard]] bool empty() const noexcept;

        friend bool operator==(const TableSettings&, const TableSettings&) = default;
    };

}
