#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string_view>
#include <vector>

#include "scan/engine.hpp"

namespace slopkit::app
{
    int run(int argc, char** argv);

    std::filesystem::path executable_directory();

    std::vector<std::filesystem::path> plugin_search_directories();

    int print_version(std::ostream& out, std::ostream& err);
    int list_plugins(std::ostream& out, std::ostream& err);
    int list_processes(std::ostream& out, std::ostream& err);

    int scan_process(std::ostream& out, std::ostream& err, std::uint32_t pid, std::string_view value_text);

    // `slopkit generate-trainer`: turns a saved table into standalone trainers, one
    // per requested target. `args` are the arguments after the command name.
    int generate_trainer(std::ostream& out, std::ostream& err, std::span<const std::string_view> args);

    // Prints one stored hit per line and the result summary, naming the stored and
    // match counts apart when the scan had to cap its stored set.
    void report_scan_hits(std::ostream& out, const scan::ScanSnapshot& snapshot, scan::ValueType value_type);

}
