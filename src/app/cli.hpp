#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace slopkit::app
{

    // Parses the command line and runs the requested mode; returns the process
    // exit code.
    int run(int argc, char** argv);

    // Directory containing the running executable.
    std::filesystem::path executable_directory();

    // `${executable_dir}/plugins`, the installed plugin directory, then every
    // SLOPKIT_PLUGIN_PATH entry.
    std::vector<std::filesystem::path> plugin_search_directories();

    // Headless commands, exposed so tests can exercise them without a display.
    // Each returns a process exit code.
    int print_version(std::ostream& out, std::ostream& err);
    int list_plugins(std::ostream& out, std::ostream& err);
    int list_processes(std::ostream& out, std::ostream& err);

    // Runs one exact-value scan of `pid`'s memory for `value_text` and prints the
    // first hits as `address<TAB>type<TAB>value`. The value type is inferred from
    // the literal (integer, or real when it has a fractional part).
    int scan_process(std::ostream& out, std::ostream& err, std::uint32_t pid, std::string_view value_text);

} // namespace slopkit::app
