#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "process/types.hpp"

#include <QString>

namespace slopkit::ui
{

    // How an address inside a loaded module image is shown.
    enum class AddressMode
    {
        module_relative, // "libc.so.6+1A2B" (default)
        absolute,        // "0x7F3A1B2C"
    };

    // One file-backed module image: display name plus its half-open span.
    struct ModuleSpan
    {
        std::string   name;
        std::uint64_t base {};
        std::uint64_t end {};
        bool          is_main {};

        bool operator==(const ModuleSpan&) const = default;
    };

    // The map a view resolves addresses against: file-backed images, sorted by
    // base. The lookup is a binary search, so it stays cheap per painted row.
    class ModuleSpans
    {
    public:
        void               set_modules(std::span<const process::ModuleInfo> modules);
        void               clear();
        [[nodiscard]] bool empty() const;

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

    // "name+RVA": upper-case hex, no 0x, no leading zeros, no padding.
    [[nodiscard]] QString format_module_relative(const ModuleSpan& span, std::uint64_t address);

    // "0x…": upper-case hex, no leading zeros, no padding.
    [[nodiscard]] QString format_absolute(std::uint64_t address);

    // The module-relative text, or nullopt in absolute mode / outside every span.
    [[nodiscard]] std::optional<QString>
    module_relative_text(AddressMode mode, const ModuleSpans& spans, std::uint64_t address);

    // "0x…"/decimal as today, plus "<module>+<RVA>" resolved against `spans`.
    [[nodiscard]] std::optional<std::uint64_t> parse_address_text(std::string_view text, const ModuleSpans& spans);

} // namespace slopkit::ui
