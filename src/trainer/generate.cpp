#include "trainer/generate.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "app/cli.hpp"
#include "reclass/serialize.hpp"
#include "table/address_table.hpp"
#include "table/entry_name.hpp"
#include "trainer/payload.hpp"

namespace slopkit::trainer
{
    namespace
    {
        std::string template_file_name(TargetOs target)
        {
            return target == TargetOs::windows ? "slopkit-trainer.exe" : "slopkit-trainer";
        }

        std::vector<std::filesystem::path> default_directories()
        {
            std::vector<std::filesystem::path> directories;
#ifdef SLOPKIT_TRAINER_DIR
            directories.emplace_back(SLOPKIT_TRAINER_DIR);
#endif
#ifdef SLOPKIT_TRAINER_RELATIVE_DIR
            const auto executable = app::executable_directory();
            if (!executable.empty())
            {
                directories.push_back(executable / SLOPKIT_TRAINER_RELATIVE_DIR);
            }
#endif
            return directories;
        }

        std::string describe_directories(std::span<const std::filesystem::path> directories)
        {
            std::string text;
            for (const auto& directory : directories)
            {
                if (!text.empty())
                {
                    text += ", ";
                }
                text += directory.string();
            }
            return text.empty() ? std::string {"no template directory"} : text;
        }

        std::optional<std::filesystem::path> find_template(std::span<const std::filesystem::path> directories,
                                                           TargetOs                               target)
        {
            for (const auto& directory : directories)
            {
                const auto      candidate = directory / template_file_name(target);
                std::error_code error;
                if (std::filesystem::is_regular_file(candidate, error))
                {
                    return candidate;
                }
            }
            return std::nullopt;
        }

        bool append_payload(const std::filesystem::path& file, std::span<const std::byte> payload)
        {
            std::ofstream stream {file, std::ios::binary | std::ios::app};
            if (!stream)
            {
                return false;
            }
            stream.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
            return stream.good();
        }

        void write_target(const Request&                         request,
                          TargetOs                               target,
                          std::span<const std::filesystem::path> directories,
                          Payload                                payload,
                          Result&                                result)
        {
            const auto template_file = find_template(directories, target);
            if (!template_file)
            {
                result.errors.push_back(std::format(
                    "no {} trainer template in {}", target_name(target), describe_directories(directories)));
                return;
            }

            payload.target_os      = target == TargetOs::windows ? kTargetWindows : kTargetLinux;
            const auto destination = request.output_directory / output_name(request.base_name, target);
            auto       temporary   = destination;
            temporary += ".tmp";

            std::error_code error;
            std::filesystem::copy_file(
                *template_file, temporary, std::filesystem::copy_options::overwrite_existing, error);
            if (error)
            {
                result.errors.push_back(
                    std::format("could not copy the {} trainer template: {}", target_name(target), error.message()));
                return;
            }

            const auto encoded = encode_payload(payload);
            if (!append_payload(temporary, encoded))
            {
                std::error_code cleanup;
                std::filesystem::remove(temporary, cleanup);
                result.errors.push_back(std::format("could not write {}", destination.string()));
                return;
            }

            std::filesystem::rename(temporary, destination, error);
            if (error)
            {
                std::error_code cleanup;
                std::filesystem::remove(temporary, cleanup);
                result.errors.push_back(std::format("could not replace {}: {}", destination.string(), error.message()));
                return;
            }

            // A copy carries the template's mode, but the Linux trainer has to be
            // runnable even when the template it was copied from was not.
            if (target == TargetOs::linux)
            {
                std::error_code ignored;
                std::filesystem::permissions(destination,
                                             std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec
                                                 | std::filesystem::perms::others_exec,
                                             std::filesystem::perm_options::add,
                                             ignored);
            }

            result.written.push_back(Written {target, destination});
        }
    }

    std::string output_name(std::string_view base_name, TargetOs target)
    {
        return std::format("{}.trainer{}", base_name, target == TargetOs::windows ? ".exe" : "");
    }

    std::vector<std::filesystem::path> template_directories()
    {
        return default_directories();
    }

    std::expected<Result, std::string> generate(const Request&                         request,
                                                std::span<const std::filesystem::path> directories)
    {
        if (request.table == nullptr)
        {
            return std::unexpected(std::string {"no table was given"});
        }
        if (request.base_name.empty())
        {
            return std::unexpected(std::string {"no base file name was given"});
        }
        if (request.output_directory.empty())
        {
            return std::unexpected(std::string {"no output directory was given"});
        }
        if (!request.linux_target && !request.windows_target)
        {
            return std::unexpected(std::string {"no target was selected"});
        }

        Result  result;
        Payload payload;
        payload.pointer_size = static_cast<std::uint32_t>(request.pointer_size);
        payload.process_name = request.process_name;

        // The saved table's row order is the trainer's; each row keeps its nesting
        // depth and the number its own kind's CLI flags use.
        std::uint32_t value_number  = 0;
        std::uint32_t script_number = 0;
        const auto    entries       = request.table->entries();
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const auto& entry = entries[index];
            const auto  depth = request.table->depth_of(index);

            PayloadItem item;
            item.depth = static_cast<std::uint8_t>(depth > 255 ? 255 : depth);
            if (entry.kind == table::EntryKind::script)
            {
                PayloadScript script;
                script.description = entry.description;
                script.source      = entry.script;
                item.kind          = ItemKind::script;
                item.number        = ++script_number;
                item.data          = std::move(script);
                ++result.script_entries;
            }
            else
            {
                PayloadEntry record;
                record.description  = entry.description;
                record.type         = entry.type;
                record.frozen       = entry.active;
                record.hex          = entry.hex;
                record.bytes        = entry.bytes;
                record.expression   = entry.expression;
                record.last_address = entry.address;
                item.kind           = ItemKind::value;
                item.number         = ++value_number;
                item.data           = std::move(record);
                ++result.value_entries;
            }
            payload.items.push_back(std::move(item));
        }

        for (const auto& member : reclass::to_members(request.table->structures()))
        {
            payload.classes.push_back(PayloadClass {member.name, member.text});
        }

        if (result.value_entries == 0 && result.script_entries == 0)
        {
            return std::unexpected(std::string {"the table has no value entries to embed"});
        }

        std::error_code error;
        std::filesystem::create_directories(request.output_directory, error);
        if (!std::filesystem::is_directory(request.output_directory))
        {
            return std::unexpected(std::format("could not use {} as the output directory: {}",
                                               request.output_directory.string(),
                                               error ? error.message() : std::string {"not a directory"}));
        }

        if (request.linux_target)
        {
            write_target(request, TargetOs::linux, directories, payload, result);
        }
        if (request.windows_target)
        {
            write_target(request, TargetOs::windows, directories, payload, result);
        }
        return result;
    }

    std::expected<Result, std::string> generate(const Request& request)
    {
        const auto directories = default_directories();
        return generate(request, directories);
    }

}
