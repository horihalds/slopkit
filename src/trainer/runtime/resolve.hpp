#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "expr/resolver.hpp"
#include "trainer/payload.hpp"
#include "trainer/runtime/target.hpp"

namespace slopkit::trainer
{
    // An entry's live state. `address` starts at the address recorded at generation
    // time, which is what an unresolved expression keeps showing.
    struct LiveEntry
    {
        std::uint64_t          address {};
        bool                   resolved {false};
        bool                   frozen {false};
        std::string            note;
        std::vector<std::byte> bytes;
    };

    // The number of bytes an entry occupies: fixed types use their width, strings
    // and byte arrays the length recorded beside the entry.
    [[nodiscard]] std::size_t entry_size(const PayloadEntry& entry);

    // Resolves expressions the way slopkit does - `expr::evaluate` against a module
    // list of basenames - and reads and writes values through the target.
    class Resolver
    {
    public:
        Resolver(const Target& target, std::size_t pointer_size);

        void refresh_modules();

        void refresh(const PayloadEntry& entry, LiveEntry& live) const;

        [[nodiscard]] std::expected<void, std::string>
        write(const PayloadEntry& entry, const LiveEntry& live, std::span<const std::byte> bytes) const;

    private:
        const Target&                target_;
        std::size_t                  pointer_size_ {8};
        std::vector<expr::ModuleRef> modules_;
    };

}
