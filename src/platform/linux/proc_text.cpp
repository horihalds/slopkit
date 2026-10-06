#include "platform/linux/proc_text.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace slopkit::platform
{

    std::string_view trim(std::string_view value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
        {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
        {
            value.remove_suffix(1);
        }
        return value;
    }

    std::vector<std::string_view> split_lines(std::string_view text)
    {
        std::vector<std::string_view> lines;
        std::size_t                   start = 0;
        while (start <= text.size())
        {
            const auto newline = text.find('\n', start);
            const auto length  = newline == std::string_view::npos ? std::string_view::npos : newline - start;
            lines.push_back(text.substr(start, length));
            if (newline == std::string_view::npos)
            {
                break;
            }
            start = newline + 1;
        }
        return lines;
    }

    std::optional<std::string> read_file(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return std::nullopt;
        }
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::string proc_entry(process::ProcessId pid, std::string_view name)
    {
        return "/proc/" + std::to_string(pid) + "/" + std::string(name);
    }

    bool is_all_digits(std::string_view value)
    {
        return !value.empty()
            && std::ranges::all_of(value,
                                   [](char character)
                                   {
                                       return character >= '0' && character <= '9';
                                   });
    }

    std::string basename_lower(std::string_view path)
    {
        const auto  separator = path.find_last_of("/\\");
        const auto  name      = separator == std::string_view::npos ? path : path.substr(separator + 1);
        std::string result(name);
        std::ranges::transform(result,
                               result.begin(),
                               [](char character)
                               {
                                   return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                               });
        return result;
    }

} // namespace slopkit::platform
