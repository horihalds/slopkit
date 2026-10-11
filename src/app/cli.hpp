#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace slopkit::app
{
    int run(int argc, char** argv);

    std::filesystem::path executable_directory();

    std::vector<std::filesystem::path> plugin_search_directories();

    int print_version(std::ostream& out, std::ostream& err);
    int list_plugins(std::ostream& out, std::ostream& err);
    int list_processes(std::ostream& out, std::ostream& err);

    int scan_process(std::ostream& out, std::ostream& err, std::uint32_t pid, std::string_view value_text);

}
