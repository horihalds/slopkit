#pragma once

#include <filesystem>
#include <iosfwd>
#include <vector>

namespace slopkit::app
{

    // Parses the command line and runs the requested mode; returns the process
    // exit code.
    int run(int argc, char** argv);

    // Directory containing the running executable.
    std::filesystem::path executable_directory();

    // `${executable_dir}/plugins` plus every SLOPKIT_PLUGIN_PATH entry.
    std::vector<std::filesystem::path> plugin_search_directories();

    // Headless commands, exposed so tests can exercise them without a display.
    // Each returns a process exit code.
    int print_version(std::ostream& out, std::ostream& err);
    int list_plugins(std::ostream& out, std::ostream& err);
    int list_processes(std::ostream& out, std::ostream& err);

} // namespace slopkit::app
