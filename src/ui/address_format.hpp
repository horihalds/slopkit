#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "expr/resolver.hpp"
#include "process/types.hpp"

#include <QString>

namespace slopkit::ui
{
    enum class AddressMode
    {
        module_relative,
        absolute,
    };

    inline constexpr std::size_t kModuleNameBudget = 16;
    inline constexpr std::size_t kModuleNameHead   = 6;
    inline constexpr std::size_t kModuleNameTail   = 5;

    [[nodiscard]] QString elide_module_name(std::string_view name);

    // Shortens only for display: the stored expression, the parser and the saved
    // file all keep the untruncated base name.
    [[nodiscard]] QString elide_address_expression(std::string_view text);

    struct ModuleSpan
    {
        std::string   name;
        std::string   display;
        std::uint64_t base {};
        std::uint64_t end {};
        bool          is_main {};

        [[nodiscard]] std::string_view label() const;

        bool operator==(const ModuleSpan&) const = default;
    };

    class ModuleSpans
    {
    public:
        void                                      set_modules(std::span<const process::ModuleInfo> modules);
        void                                      clear();
        [[nodiscard]] bool                        empty() const;
        [[nodiscard]] std::span<const ModuleSpan> spans() const;

        bool operator==(const ModuleSpans&) const = default;

        [[nodiscard]] const ModuleSpan* containing(std::uint64_t address) const;
        [[nodiscard]] const ModuleSpan* find_by_name(std::string_view name) const;
        [[nodiscard]] const ModuleSpan* main() const;

    private:
        std::vector<ModuleSpan> spans_;
        std::size_t             main_index_ {};
    };

    [[nodiscard]] QString format_module_relative(const ModuleSpan& span, std::uint64_t address);

    [[nodiscard]] QString format_module_relative_full(const ModuleSpan& span, std::uint64_t address);

    [[nodiscard]] QString format_absolute(std::uint64_t address);

    [[nodiscard]] QString format_padded_hex(std::uint64_t value, int digits = 16);

    [[nodiscard]] QString format_pane_address(AddressMode mode, const ModuleSpans& spans, std::uint64_t address);

    [[nodiscard]] QString format_cell_address(AddressMode mode, const ModuleSpans& spans, std::uint64_t address);

    [[nodiscard]] QString format_pane_address_full(AddressMode mode, const ModuleSpans& spans, std::uint64_t address);
    [[nodiscard]] QString format_cell_address_full(AddressMode mode, const ModuleSpans& spans, std::uint64_t address);

    using AddressText = std::function<QString(std::uint64_t)>;

    [[nodiscard]] std::optional<QString>
    module_relative_text(AddressMode mode, const ModuleSpans& spans, std::uint64_t address);

    [[nodiscard]] std::optional<std::uint64_t>
    parse_address_text(std::string_view text, const ModuleSpans& spans, expr::Symbols symbols = {});

    [[nodiscard]] std::vector<expr::ModuleRef> module_refs(const ModuleSpans& spans);

    [[nodiscard]] std::optional<std::uint64_t> module_base(const ModuleSpans& spans, std::string_view name);

    [[nodiscard]] std::optional<std::uint64_t> symbol_value(expr::Symbols symbols, std::string_view name);

}
