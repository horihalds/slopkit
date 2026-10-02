#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "process/access.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"

namespace slopkit::table
{

    // One tracked address. The Active checkbox doubles as the freeze toggle.
    struct AddressEntry
    {
        bool                   active {false};
        std::string            description;
        std::uint64_t          address {};
        scan::ValueType        type {scan::ValueType::int32};
        std::vector<std::byte> bytes;
        bool                   hex {false};
    };

    // The address list shown in the bottom zone. It owns no session: callers
    // pass the shared one in for reads and writes.
    class AddressTable
    {
    public:
        void add(AddressEntry entry);
        void remove(std::size_t index);
        void clear();

        [[nodiscard]] std::span<AddressEntry>       entries() noexcept;
        [[nodiscard]] std::span<const AddressEntry> entries() const noexcept;
        [[nodiscard]] std::size_t                   size() const noexcept;
        [[nodiscard]] bool                          empty() const noexcept;
        [[nodiscard]] bool                          valid_index(std::size_t index) const noexcept;

        [[nodiscard]] int selected() const noexcept;
        void              set_selected(int index) noexcept;

        // The value text shown in the table.
        [[nodiscard]] std::string display_value(std::size_t index) const;

        // Parses `text` for the entry's type, writes it into the target and
        // updates the cached bytes. Malformed input returns invalid_argument.
        std::expected<void, process::AccessError>
        write_value(std::size_t index, std::string_view text, process::Session& session);

        // Rewrites the frozen entries at most every `interval` seconds. Returns
        // how many entries were written, or the first write failure.
        [[nodiscard]] std::expected<std::size_t, process::AccessError>
        tick_freeze(process::Session& session, double now, double interval = 0.1);

    private:
        std::vector<AddressEntry> entries_;
        int                       selected_ {-1};
        double                    next_freeze_time_ {0.0};
    };

} // namespace slopkit::table
