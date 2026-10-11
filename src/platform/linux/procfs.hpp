#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "process/types.hpp"

namespace slopkit::platform
{
    struct ProcessStatus
    {
        std::string   name;
        std::uint32_t ppid {0};
        std::uint32_t uid {0};
        std::uint32_t gid {0};
        int           tracer_pid {0};
    };

    struct MappedRegion
    {
        std::uint64_t start {0};
        std::uint64_t end {0};
        bool          readable {false};
        bool          writable {false};
        bool          executable {false};
        bool          shared {false};
        std::uint64_t offset {0};
        std::string   path;
        bool          deleted {false};
    };

    [[nodiscard]] std::vector<process::ProcessId>  list_pids();
    [[nodiscard]] std::optional<std::string>       read_exe(process::ProcessId pid);
    [[nodiscard]] std::optional<std::string>       read_cmdline(process::ProcessId pid);
    [[nodiscard]] std::vector<std::string>         read_cmdline_parts(process::ProcessId pid);
    [[nodiscard]] std::optional<ProcessStatus>     read_status(process::ProcessId pid);
    [[nodiscard]] std::vector<MappedRegion>        read_maps(process::ProcessId pid);
    [[nodiscard]] std::vector<process::ThreadInfo> read_threads(process::ProcessId pid);
    [[nodiscard]] std::optional<char>              read_thread_state(process::ProcessId pid, process::ProcessId tid);
    [[nodiscard]] std::optional<std::int64_t>      read_thread_syscall(process::ProcessId pid, process::ProcessId tid);
    [[nodiscard]] std::string                      read_thread_wchan(process::ProcessId pid, process::ProcessId tid);

    [[nodiscard]] std::optional<ProcessStatus> parse_status(std::string_view text);
    [[nodiscard]] std::vector<MappedRegion>    parse_maps(std::string_view text);
    [[nodiscard]] std::vector<std::uint32_t>   parse_task_ids(std::string_view text);

    [[nodiscard]] process::ModuleKind classify_region(const MappedRegion& region);
    [[nodiscard]] std::string         region_name(const MappedRegion& region);

    [[nodiscard]] std::vector<process::ModuleInfo> modules_from_maps(const std::vector<MappedRegion>& regions);

    void fill_module_entry_points(process::ProcessId pid, std::vector<process::ModuleInfo>& modules);

    [[nodiscard]] bool flag_main_module_by_path(std::vector<process::ModuleInfo>& modules, std::string_view exe_path);

    [[nodiscard]] bool flag_main_pe_image(process::ProcessId pid, std::vector<process::ModuleInfo>& modules);

}
