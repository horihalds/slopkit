#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "trainer/payload.hpp"

namespace slopkit::table
{
    class AddressTable;
}

namespace slopkit::trainer
{
    struct Request
    {
        const table::AddressTable* table {};
        // The process the generating session was attached to; the trainer attaches
        // to it when neither `--pid` nor `--name` is given.
        std::string                process_name;
        std::size_t                pointer_size {8};
        bool                       linux_target {true};
        bool                       windows_target {false};
        std::filesystem::path      output_directory;
        std::string                base_name;
    };

    struct Written
    {
        TargetOs              target {TargetOs::linux};
        std::filesystem::path path;
    };

    struct Result
    {
        std::vector<Written>     written;
        std::size_t              value_entries {0};
        std::size_t              script_entries {0};
        std::vector<std::string> errors;
    };

    [[nodiscard]] std::string output_name(std::string_view base_name, TargetOs target);

    // The directories a template is looked up in, in order: the build tree the
    // binary was linked against, then the installed layout next to the executable.
    [[nodiscard]] std::vector<std::filesystem::path> template_directories();

    // Per-target failures land in `Result::errors` and leave the other target's
    // file in place; only a request that cannot be honoured at all is an error.
    [[nodiscard]] std::expected<Result, std::string> generate(const Request& request);

    [[nodiscard]] std::expected<Result, std::string> generate(const Request&                         request,
                                                              std::span<const std::filesystem::path> directories);

}
