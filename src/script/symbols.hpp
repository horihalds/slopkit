#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "expr/resolver.hpp"
#include "script/types.hpp"

namespace slopkit::script
{

    // Validates a name the expression parser could later resolve: non-empty,
    // non-blank, no `+` (the parser splits on one) and no leading `#` (that
    // marks a decimal literal). Returns the error text, or an empty optional for
    // a valid name. Shared by the symbol table and the script-local labels.
    [[nodiscard]] std::optional<std::string> validate_symbol_name(std::string_view name);

    // The process-wide registry scripts publish names into. A registered name
    // resolves like a module name in any address expression (see
    // `expr::evaluate`). The table survives a detach and lives until slopkit
    // exits; it is never written to a `.skt` file.
    //
    // Scripts write on the access worker thread while the UI reads on the UI
    // thread, so a single mutex guards the vector. Readers copy a snapshot and
    // never hold the lock across a resolve.
    class SymbolTable
    {
    public:
        // Validates the name (non-empty, non-blank, no `+`, no leading `#`) and
        // stores `value`. An existing case-insensitive match is overwritten and
        // keeps its first spelling; otherwise the name is appended.
        std::expected<void, std::string> set(std::string_view name, std::uint64_t value);

        // Removes `name`; an unknown name is a no-op. Returns whether it was
        // there.
        bool remove(std::string_view name);

        // The value stored for `name`, matched case-insensitively.
        [[nodiscard]] std::optional<std::uint64_t> lookup(std::string_view name) const;

        // A copy of the whole table, taken under the lock.
        [[nodiscard]] std::vector<expr::SymbolRef> snapshot() const;

        // Drops every entry; used to scope the engine's script-local labels to
        // the chunk that registered them.
        void clear();

        [[nodiscard]] std::size_t size() const;

        // The seam closures over this table, so an engine or a test can drive
        // it without seeing the class.
        [[nodiscard]] SymbolApi api();

    private:
        mutable std::mutex           mutex_;
        std::vector<expr::SymbolRef> symbols_; // insertion order, unique case-insensitive names
    };

    // The process-wide table used as the default by the worker, the controller,
    // the views and the main window, so a caller without a table of its own
    // (mostly tests) can still build them. Production owns its table in
    // `ui::App` and passes it explicitly.
    [[nodiscard]] SymbolTable& default_symbol_table();

} // namespace slopkit::script
