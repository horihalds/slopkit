#include "plugins/wine_proton/agent_modules.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace slopkit::plugins::wine_proton
{
    namespace
    {
        void append_utf8(char32_t code, std::string& out)
        {
            if (code <= 0x7F)
            {
                out.push_back(static_cast<char>(code));
            }
            else if (code <= 0x7FF)
            {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else if (code <= 0xFFFF)
            {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
        }
    } // namespace

    void decode_utf16(std::span<const char16_t> text, std::string& out)
    {
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            const char16_t unit = text[index];
            char32_t       code = unit;
            if (unit >= 0xD800 && unit <= 0xDBFF)
            {
                // A high surrogate is only valid when a low surrogate follows.
                if (index + 1 < text.size() && text[index + 1] >= 0xDC00 && text[index + 1] <= 0xDFFF)
                {
                    code = 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10)
                         + (static_cast<char32_t>(text[index + 1]) - 0xDC00);
                    ++index;
                }
                else
                {
                    code = 0xFFFD;
                }
            }
            else if (unit >= 0xDC00 && unit <= 0xDFFF)
            {
                // A low surrogate without a preceding high one.
                code = 0xFFFD;
            }
            append_utf8(code, out);
        }
    }

    std::vector<process::ModuleInfo> merge_modules(std::span<const AgentModuleEntry>          image_modules,
                                                   const std::vector<platform::MappedRegion>& maps)
    {
        std::vector<process::ModuleInfo> merged;
        merged.reserve(image_modules.size());
        for (const auto& image : image_modules)
        {
            process::ModuleInfo module;
            module.base    = image.base;
            module.size    = image.size;
            module.entry   = image.entry;
            module.kind    = process::ModuleKind::pe;
            module.is_main = image.main;
            module.name    = image.name;

            // The procfs entry containing the image base supplies the on-disk
            // identity; a loader entry with no counterpart keeps an empty path
            // and offset 0.
            const auto overlay = std::ranges::find_if(maps,
                                                      [&](const platform::MappedRegion& map)
                                                      {
                                                          return map.start <= module.base && module.base < map.end;
                                                      });
            if (overlay != maps.end())
            {
                module.path   = overlay->path;
                module.offset = overlay->offset;
            }
            merged.push_back(std::move(module));
        }

        std::ranges::sort(merged, {}, &process::ModuleInfo::base);
        return merged;
    }
} // namespace slopkit::plugins::wine_proton
