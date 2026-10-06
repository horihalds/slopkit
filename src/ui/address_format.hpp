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

    // How an address inside a loaded module image is shown.
    enum class AddressMode
    {
        module_relative, // "libc.so.6+1A2B" (default)
        absolute,        // "7F3A1B2C"
    };

    // A module's display name is shortened once the part before its file
    // extension exceeds this budget; the extension itself is always kept. See
    // `elide_module_name` and docs/UI_DESIGN.md.
    inline constexpr std::size_t kModuleNameBudget = 16;
    inline constexpr std::size_t kModuleNameHead   = 6;
    inline constexpr std::size_t kModuleNameTail   = 5;

    // The name a module is displayed with: its name, or the file name of its
    // path when the name is empty, shortened to the module-name budget. Within
    // the budget the text is unchanged, otherwise the part before the extension
    // keeps its first 6 and last 5 characters around "...". The extension is
    // never cut, and a leading dot is part of the name, not an extension.
    [[nodiscard]] QString elide_module_name(std::string_view name);

    // One file-backed module image: the full name plus the shortened label the
    // surfaces paint and its half-open span.
    struct ModuleSpan
    {
        std::string   name;    // the full name: the lookup and clipboard spelling
        std::string   display; // the shortened label the surfaces paint
        std::uint64_t base {};
        std::uint64_t end {};
        bool          is_main {};

        // The label a surface paints: `display`, or `name` for a span built by
        // hand that never went through the shortening rule.
        [[nodiscard]] std::string_view label() const;

        bool operator==(const ModuleSpan&) const = default;
    };

    // The map a view resolves addresses against: file-backed images, sorted by
    // base. The lookup is a binary search, so it stays cheap per painted row.
    class ModuleSpans
    {
    public:
        void                                      set_modules(std::span<const process::ModuleInfo> modules);
        void                                      clear();
        [[nodiscard]] bool                        empty() const;
        // The file-backed images, sorted by base.
        [[nodiscard]] std::span<const ModuleSpan> spans() const;

        bool operator==(const ModuleSpans&) const = default;

        // The span containing `address`, or nullptr outside every image.
        [[nodiscard]] const ModuleSpan* containing(std::uint64_t address) const;
        // The span whose file name matches `name` case-insensitively.
        [[nodiscard]] const ModuleSpan* find_by_name(std::string_view name) const;
        // The target's main image span, or nullptr when the map is empty.
        [[nodiscard]] const ModuleSpan* main() const;

    private:
        std::vector<ModuleSpan> spans_;
        std::size_t             main_index_ {};
    };

    // "name+RVA" with the module's shortened label: upper-case hex, no 0x, no
    // leading zeros, no padding.
    [[nodiscard]] QString format_module_relative(const ModuleSpan& span, std::uint64_t address);

    // The untruncated "<name>+<RVA>"; what Copy keeps so a pasted address
    // still resolves.
    [[nodiscard]] QString format_module_relative_full(const ModuleSpan& span, std::uint64_t address);

    // Upper-case hex, no prefix, no leading zeros, no padding.
    [[nodiscard]] QString format_absolute(std::uint64_t address);

    // Upper-case hex, no prefix, zero-padded to `digits`, for the fixed-width
    // panes where addresses must line up (docs/UI_DESIGN.md#fonts).
    [[nodiscard]] QString format_padded_hex(std::uint64_t value, int digits = 16);

    // A fixed-width pane row's address: module-relative where the mode and spans
    // allow it, otherwise `format_padded_hex`.
    [[nodiscard]] QString format_pane_address(AddressMode mode, const ModuleSpans& spans, std::uint64_t address);

    // A value/cell address: module-relative where the mode and spans allow it,
    // otherwise `format_absolute`.
    [[nodiscard]] QString format_cell_address(AddressMode mode, const ModuleSpans& spans, std::uint64_t address);

    // The untruncated forms of the two above, for Copy and for hovers.
    [[nodiscard]] QString format_pane_address_full(AddressMode mode, const ModuleSpans& spans, std::uint64_t address);
    [[nodiscard]] QString format_cell_address_full(AddressMode mode, const ModuleSpans& spans, std::uint64_t address);

    // How a model renders an address, so it needs no controller of its own.
    using AddressText = std::function<QString(std::uint64_t)>;

    // The module-relative text, or nullopt in absolute mode / outside every span.
    [[nodiscard]] std::optional<QString>
    module_relative_text(AddressMode mode, const ModuleSpans& spans, std::uint64_t address);

    // A deref-free address: a bare hex value (`0x…` is also accepted, `#…` is
    // decimal), a bare module name (resolved case-insensitively to its base,
    // winning over the hex reading), or a `<module>+<RVA>` /
    // `<literal>+<offset>` expression. A pointer chain (two or more offsets)
    // yields nothing - it must go through the access worker.
    [[nodiscard]] std::optional<std::uint64_t> parse_address_text(std::string_view text, const ModuleSpans& spans);

    // The module map in the form the expression resolver expects.
    [[nodiscard]] std::vector<expr::ModuleRef> module_refs(const ModuleSpans& spans);

    // The base of the module named `name` (case-insensitive), or nothing.
    [[nodiscard]] std::optional<std::uint64_t> module_base(const ModuleSpans& spans, std::string_view name);

} // namespace slopkit::ui
