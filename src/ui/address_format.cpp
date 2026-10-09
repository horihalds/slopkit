#include "ui/address_format.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>

#include "scan/value.hpp"
#include "ui/text.hpp"

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
    } // namespace

    QString elide_module_name(std::string_view name)
    {
        // Split at the last '.' that is not the first character; a leading dot
        // is part of the name, so `.hidden` has no extension.
        const std::size_t      dot           = name.rfind('.');
        const bool             has_extension = dot != std::string_view::npos && dot != 0;
        const std::string_view stem          = has_extension ? name.substr(0, dot) : name;
        if (stem.size() <= kModuleNameBudget)
        {
            return to_qstring(name);
        }

        QString label = to_qstring(stem.substr(0, kModuleNameHead));
        label += QStringLiteral("...");
        label += to_qstring(stem.substr(stem.size() - kModuleNameTail));
        if (has_extension)
        {
            label += to_qstring(name.substr(dot));
        }
        return label;
    }

    std::string_view ModuleSpan::label() const
    {
        return display.empty() ? std::string_view(name) : std::string_view(display);
    }

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
            const std::string label = module_label(module);
            spans_.push_back(ModuleSpan {label,
                                         elide_module_name(label).toStdString(),
                                         module.base,
                                         module.base + module.size,
                                         &module == main});
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

    std::span<const ModuleSpan> ModuleSpans::spans() const
    {
        return spans_;
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
        return to_qstring(span.label()) + QLatin1Char('+') + QString::number(rva, 16).toUpper();
    }

    QString format_module_relative_full(const ModuleSpan& span, std::uint64_t address)
    {
        const std::uint64_t rva = address >= span.base ? address - span.base : 0;
        return to_qstring(span.name) + QLatin1Char('+') + QString::number(rva, 16).toUpper();
    }

    QString format_absolute(std::uint64_t address)
    {
        return QString::number(address, 16).toUpper();
    }

    QString format_padded_hex(std::uint64_t value, int digits)
    {
        return QString::number(value, 16).toUpper().rightJustified(digits, QLatin1Char('0'));
    }

    QString format_pane_address(AddressMode mode, const ModuleSpans& spans, std::uint64_t address)
    {
        if (const auto relative = module_relative_text(mode, spans, address); relative.has_value())
        {
            return *relative;
        }
        return format_padded_hex(address);
    }

    QString format_cell_address(AddressMode mode, const ModuleSpans& spans, std::uint64_t address)
    {
        if (const auto relative = module_relative_text(mode, spans, address); relative.has_value())
        {
            return *relative;
        }
        return format_absolute(address);
    }

    QString format_pane_address_full(AddressMode mode, const ModuleSpans& spans, std::uint64_t address)
    {
        if (mode == AddressMode::module_relative)
        {
            if (const ModuleSpan* span = spans.containing(address); span != nullptr)
            {
                return format_module_relative_full(*span, address);
            }
        }
        return format_padded_hex(address);
    }

    QString format_cell_address_full(AddressMode mode, const ModuleSpans& spans, std::uint64_t address)
    {
        if (mode == AddressMode::module_relative)
        {
            if (const ModuleSpan* span = spans.containing(address); span != nullptr)
            {
                return format_module_relative_full(*span, address);
            }
        }
        return format_absolute(address);
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

    std::optional<std::uint64_t>
    parse_address_text(std::string_view text, const ModuleSpans& spans, expr::Symbols symbols)
    {
        if (text.find('+') == std::string_view::npos)
        {
            // A bare module name jumps to the module base; the lookup is
            // case-insensitive and wins over the hex reading of the text. A
            // symbol name resolves the same way, after the module lookup.
            if (const ModuleSpan* span = spans.find_by_name(text); span != nullptr)
            {
                return span->base;
            }
            if (const std::optional<std::uint64_t> value = symbol_value(symbols, text); value.has_value())
            {
                return value;
            }
            // A plain absolute address is bare hex; `0x…` is hex and `#…`
            // is decimal.
            const auto parsed = scan::parse_address(text);
            return parsed.has_value() ? std::optional<std::uint64_t> {*parsed} : std::nullopt;
        }

        const auto expression = expr::parse(text);
        if (!expression || expression->offsets.size() != 1)
        {
            return std::nullopt; // a pointer chain needs the worker
        }

        std::uint64_t base = 0;
        if (const ModuleSpan* span = spans.find_by_name(expression->base); span != nullptr)
        {
            base = span->base;
        }
        else if (const std::optional<std::uint64_t> value = symbol_value(symbols, expression->base); value.has_value())
        {
            base = *value;
        }
        else if (const auto literal = expr::parse_literal(expression->base); literal.has_value())
        {
            base = *literal;
        }
        else
        {
            return std::nullopt;
        }
        return base + expression->offsets.front().value;
    }

    std::vector<expr::ModuleRef> module_refs(const ModuleSpans& spans)
    {
        std::vector<expr::ModuleRef> refs;
        refs.reserve(spans.spans().size());
        for (const ModuleSpan& span : spans.spans())
        {
            refs.push_back(expr::ModuleRef {span.name, span.base});
        }
        return refs;
    }

    std::optional<std::uint64_t> module_base(const ModuleSpans& spans, std::string_view name)
    {
        if (const ModuleSpan* span = spans.find_by_name(name); span != nullptr)
        {
            return span->base;
        }
        return std::nullopt;
    }

    std::optional<std::uint64_t> symbol_value(expr::Symbols symbols, std::string_view name)
    {
        for (const expr::SymbolRef& symbol : symbols)
        {
            if (ascii_iequals(symbol.name, name))
            {
                return symbol.value;
            }
        }
        return std::nullopt;
    }

} // namespace slopkit::ui
