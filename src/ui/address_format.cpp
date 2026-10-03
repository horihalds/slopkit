#include "ui/address_format.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <iterator>
#include <system_error>

#include "scan/value.hpp"

namespace slopkit::ui
{

    namespace
    {
        // The label a module is displayed with: its name, or the file name of
        // its path when the name is empty.
        std::string module_label(const process::ModuleInfo& module)
        {
            if (!module.name.empty())
            {
                return module.name;
            }
            const std::size_t slash = module.path.find_last_of('/');
            return slash == std::string::npos ? module.path : module.path.substr(slash + 1);
        }

        bool ascii_iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
            {
                return false;
            }
            const auto lower = [](char value)
            {
                return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
            };
            for (std::size_t index = 0; index < lhs.size(); ++index)
            {
                if (lower(lhs[index]) != lower(rhs[index]))
                {
                    return false;
                }
            }
            return true;
        }

        std::optional<std::uint64_t> parse_hex(std::string_view text)
        {
            if (text.empty())
            {
                return std::nullopt;
            }
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            {
                text.remove_prefix(2);
            }
            std::uint64_t value     = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
            if (error != std::errc {} || end != text.data() + text.size())
            {
                return std::nullopt;
            }
            return value;
        }
    } // namespace

    void ModuleSpans::set_modules(std::span<const process::ModuleInfo> modules)
    {
        spans_.clear();
        main_index_ = 0;
        spans_.reserve(modules.size());
        const process::ModuleInfo* main = process::main_module(modules);
        for (const process::ModuleInfo& module : modules)
        {
            if (!process::is_file_backed(module))
            {
                continue;
            }
            spans_.push_back(
                ModuleSpan {module_label(module), module.base, module.base + module.size, &module == main});
        }
        std::ranges::sort(spans_, {}, &ModuleSpan::base);
        for (std::size_t index = 0; index < spans_.size(); ++index)
        {
            if (spans_[index].is_main)
            {
                main_index_ = index;
                break;
            }
        }
    }

    void ModuleSpans::clear()
    {
        spans_.clear();
        main_index_ = 0;
    }

    bool ModuleSpans::empty() const
    {
        return spans_.empty();
    }

    const ModuleSpan* ModuleSpans::containing(std::uint64_t address) const
    {
        const auto after = std::upper_bound(spans_.begin(),
                                            spans_.end(),
                                            address,
                                            [](std::uint64_t value, const ModuleSpan& span)
                                            {
                                                return value < span.base;
                                            });
        if (after == spans_.begin())
        {
            return nullptr;
        }
        const ModuleSpan& candidate = *std::prev(after);
        return address < candidate.end ? &candidate : nullptr;
    }

    const ModuleSpan* ModuleSpans::find_by_name(std::string_view name) const
    {
        for (const ModuleSpan& span : spans_)
        {
            if (ascii_iequals(span.name, name))
            {
                return &span;
            }
        }
        return nullptr;
    }

    const ModuleSpan* ModuleSpans::main() const
    {
        if (spans_.empty())
        {
            return nullptr;
        }
        return &spans_[main_index_];
    }

    QString format_module_relative(const ModuleSpan& span, std::uint64_t address)
    {
        const std::uint64_t rva = address >= span.base ? address - span.base : 0;
        return QString::fromUtf8(span.name.data(), static_cast<qsizetype>(span.name.size())) + QLatin1Char('+')
             + QString::number(rva, 16).toUpper();
    }

    std::optional<QString> module_relative_text(AddressMode mode, const ModuleSpans& spans, std::uint64_t address)
    {
        if (mode != AddressMode::module_relative)
        {
            return std::nullopt;
        }
        const ModuleSpan* span = spans.containing(address);
        if (span == nullptr)
        {
            return std::nullopt;
        }
        return format_module_relative(*span, address);
    }

    std::optional<std::uint64_t> parse_address_text(std::string_view text, const ModuleSpans& spans)
    {
        const std::size_t plus = text.find('+');
        if (plus != std::string_view::npos)
        {
            const std::string_view name = text.substr(0, plus);
            const std::string_view rva  = text.substr(plus + 1);
            if (!name.empty())
            {
                if (const ModuleSpan* span = spans.find_by_name(name); span != nullptr)
                {
                    if (const auto offset = parse_hex(rva); offset.has_value())
                    {
                        return span->base + *offset;
                    }
                }
            }
            return std::nullopt;
        }

        const auto parsed = scan::parse_address(text);
        return parsed.has_value() ? std::optional<std::uint64_t> {*parsed} : std::nullopt;
    }

} // namespace slopkit::ui
