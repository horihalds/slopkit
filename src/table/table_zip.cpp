#include "table/table_zip.hpp"

#include <zip.h>

#include <array>
#include <format>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace slopkit::table
{
    namespace
    {
        // zip_open hands back a bare error code, before there is an archive whose
        // error can be read.
        std::string open_error_text(int code)
        {
            zip_error_t error;
            zip_error_init_with_code(&error, code);
            const std::string message = zip_error_strerror(&error);
            zip_error_fini(&error);
            return message;
        }

        // The last error recorded on `archive`, prefixed with `what` so a caller
        // sees which member or step failed.
        std::string archive_error_text(zip_t* archive, std::string_view what)
        {
            return std::format("{}: {}", what, zip_error_strerror(zip_get_error(archive)));
        }
    } // namespace

    std::expected<void, std::string> write_archive(const std::filesystem::path&   path,
                                                   std::span<const ArchiveMember> members)
    {
        std::filesystem::path temporary = path;
        temporary += ".tmp";

        int    code    = 0;
        zip_t* archive = zip_open(temporary.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &code);
        if (archive == nullptr)
        {
            return std::unexpected(open_error_text(code));
        }

        // An empty member list must still produce a file, so the rename below
        // always has a source; libzip removes an empty archive otherwise.
        zip_set_archive_flag(archive, ZIP_AFL_CREATE_OR_KEEP_FILE_FOR_EMPTY_ARCHIVE, 1);

        for (const ArchiveMember& member : members)
        {
            zip_source_t* source = zip_source_buffer(archive, member.text.data(), member.text.size(), 0);
            if (source == nullptr)
            {
                const std::string message = archive_error_text(archive, member.name);
                zip_discard(archive);
                std::filesystem::remove(temporary);
                return std::unexpected(message);
            }

            const zip_int64_t index = zip_file_add(archive, member.name.c_str(), source, ZIP_FL_ENC_UTF_8);
            if (index < 0)
            {
                zip_source_free(source);
                const std::string message = archive_error_text(archive, member.name);
                zip_discard(archive);
                std::filesystem::remove(temporary);
                return std::unexpected(message);
            }

            // The members are tiny text files; storing them keeps the archive
            // inspectable and the save cheap.
            zip_set_file_compression(archive, static_cast<zip_uint64_t>(index), ZIP_CM_STORE, 0);
        }

        if (zip_close(archive) < 0)
        {
            const std::string message = archive_error_text(archive, path.string());
            zip_discard(archive);
            std::filesystem::remove(temporary);
            return std::unexpected(message);
        }

        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            std::filesystem::remove(temporary);
            return std::unexpected(error.message());
        }
        return {};
    }

    std::expected<std::vector<ArchiveMember>, std::string> read_archive(const std::filesystem::path& path)
    {
        int    code    = 0;
        zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &code);
        if (archive == nullptr)
        {
            return std::unexpected(open_error_text(code));
        }

        const zip_int64_t count = zip_get_num_entries(archive, 0);
        if (count < 0)
        {
            const std::string message = archive_error_text(archive, path.string());
            zip_discard(archive);
            return std::unexpected(message);
        }

        std::vector<ArchiveMember> members;
        members.reserve(static_cast<std::size_t>(count));

        for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(count); ++index)
        {
            zip_stat_t stat;
            zip_stat_init(&stat);
            if (zip_stat_index(archive, index, 0, &stat) != 0)
            {
                const std::string message = archive_error_text(archive, path.string());
                zip_discard(archive);
                return std::unexpected(message);
            }

            zip_file_t* file = zip_fopen_index(archive, index, 0);
            if (file == nullptr)
            {
                const std::string message = archive_error_text(archive, stat.name);
                zip_discard(archive);
                return std::unexpected(message);
            }

            std::string text(static_cast<std::size_t>(stat.size), '\0');
            if (!text.empty())
            {
                const zip_int64_t read = zip_fread(file, text.data(), text.size());
                if (read < 0 || static_cast<zip_uint64_t>(read) != stat.size)
                {
                    const std::string message = archive_error_text(archive, stat.name);
                    zip_fclose(file);
                    zip_discard(archive);
                    return std::unexpected(message);
                }
            }
            zip_fclose(file);

            members.push_back(ArchiveMember {.name = stat.name, .text = std::move(text)});
        }

        zip_discard(archive);
        return members;
    }

    bool is_zip_archive(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return false;
        }

        std::array<char, 4> signature {};
        file.read(signature.data(), static_cast<std::streamsize>(signature.size()));
        return file.gcount() == static_cast<std::streamsize>(signature.size()) && signature[0] == 'P'
            && signature[1] == 'K' && signature[2] == '\x03' && signature[3] == '\x04';
    }

} // namespace slopkit::table
