#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "process/access_worker.hpp"
#include "process/types.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"
#include "table/table_settings.hpp"

namespace slopkit::table
{

    // One tracked address. The Active checkbox doubles as the freeze toggle.
    struct AddressEntry
    {
        std::uint64_t          id {}; // Stable identity, assigned by AddressTable::add.
        bool                   active {false};
        std::string            description;
        std::uint64_t          address {};
        scan::ValueType        type {scan::ValueType::int32};
        std::vector<std::byte> bytes;
        bool                   hex {false};
    };

    // How many incoming rows a merge added and how many it dropped because an
    // equal entry (address, type and description) was already present.
    struct MergeSummary
    {
        std::size_t added {};
        std::size_t skipped {};
    };

    // The address list shown in the bottom zone. It owns no session: writes are
    // encoded here and executed by the AccessWorker.
    class AddressTable
    {
    public:
        void add(AddressEntry entry);
        void remove(std::size_t index);
        void clear();

        // Replaces every entry with fresh ids. The serializer uses this when
        // loading so a file load does not log one record per row.
        void replace(std::vector<AddressEntry> entries);

        // Appends every incoming entry that is not already present (same address,
        // type and description) with a fresh id. Existing entries, the selection
        // and the settings are left untouched.
        MergeSummary merge(std::span<const AddressEntry> incoming);

        [[nodiscard]] std::span<AddressEntry>       entries() noexcept;
        [[nodiscard]] std::span<const AddressEntry> entries() const noexcept;
        [[nodiscard]] std::size_t                   size() const noexcept;
        [[nodiscard]] bool                          empty() const noexcept;
        [[nodiscard]] bool                          valid_index(std::size_t index) const noexcept;

        [[nodiscard]] int selected() const noexcept;
        void              set_selected(int index) noexcept;

        // The value text shown in the table.
        [[nodiscard]] std::string display_value(std::size_t index) const;

        // Parses `text` for the entry's type and encodes it. Malformed input
        // returns invalid_argument; no target access happens here.
        [[nodiscard]] std::expected<std::vector<std::byte>, process::AccessError>
        encode_value(std::size_t index, std::string_view text) const;

        // The items a freeze pass must write at most every `interval` seconds:
        // one per active entry with cached bytes. Advances the timer.
        [[nodiscard]] std::vector<process::WriteItem> freeze_items(double now, double interval = 0.1);

        // Caches the bytes of an entry after a successful write. Unknown ids are
        // ignored, so a completion for a removed entry is harmless.
        void apply_write(std::uint64_t entry_id, std::vector<std::byte> bytes);

        // The table's persisted settings; owned by the table so save/load
        // round-trips them.
        [[nodiscard]] TableSettings&       settings() noexcept;
        [[nodiscard]] const TableSettings& settings() const noexcept;

    private:
        std::vector<AddressEntry> entries_;
        TableSettings             settings_;
        int                       selected_ {-1};
        double                    next_freeze_time_ {0.0};
        std::uint64_t             next_id_ {1};
    };

} // namespace slopkit::table
