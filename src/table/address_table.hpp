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
#include "table/entry_name.hpp"
#include "table/table_settings.hpp"

namespace slopkit::table
{

    // One row of the address list: either a value entry (address, type, value) or
    // a script entry (a Lua source kept beside the table). `EntryKind` and the
    // member extensions that persist the kind live in `entry_name.hpp`.
    struct AddressEntry
    {
        std::uint64_t          id {};     // Stable identity, assigned by AddressTable::add.
        std::uint64_t          parent {}; // The id of the entry this one nests under; 0 = top level.
        bool                   active {false};
        EntryKind              kind {EntryKind::value};
        std::string            description;
        std::uint64_t          address {}; // the last successful resolution
        std::string            expression; // empty for a plain absolute address
        scan::ValueType        type {scan::ValueType::int32};
        std::vector<std::byte> bytes;
        bool                   hex {false};
        std::string            script; // EntryKind::script only: verbatim Lua source
    };

    // How many incoming rows a merge added and how many it dropped because an
    // equal entry (same kind; same address, type and description for a value
    // entry, same description and script text for a script entry) was already
    // present.
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
        // Erases the whole subtree rooted at `index` and keeps the selection
        // adjustment, so a parent never leaves orphaned children behind.
        void remove(std::size_t index);
        void clear();

        // Appends a script entry and selects it, returning its row index. A
        // script row has neither an address nor a type; it is run on demand, so
        // it never takes part in a freeze pass.
        [[nodiscard]] std::size_t add_script(std::string description, std::string script);

        // Replaces a script entry's source. False when `index` is out of range
        // or names a value entry.
        bool set_script(std::size_t index, std::string script);

        // Replaces an entry's description, for both kinds. False when `index` is
        // out of range.
        bool set_description(std::size_t index, std::string description);

        // Replaces every entry with fresh ids. The serializer uses this when
        // loading so a file load does not log one record per row.
        void replace(std::vector<AddressEntry> entries);

        // Moves the entry at `from` (with its whole subtree) to the final index
        // `to`, keeping its id and every field and carrying the selection with
        // the row. An equal or out-of-range pair is a no-op, so a drag onto the
        // same row changes nothing and logs nothing. Implemented over `move_row`.
        void move(std::size_t from, std::size_t to);

        // The nesting depth of `index` (0 = top level); 0 for an out-of-range
        // index.
        [[nodiscard]] std::size_t depth_of(std::size_t index) const;

        // The rows the subtree rooted at `index` holds, the root included.
        [[nodiscard]] std::size_t subtree_size(std::size_t index) const;

        // True when `index` is `ancestor` or sits inside its subtree.
        [[nodiscard]] bool is_within_subtree(std::size_t index, std::size_t ancestor) const;

        // Moves the row at `from` with its subtree. With `nest`, the moved root
        // becomes the last child of the row at `target`; otherwise it is inserted
        // directly in front of the row at `target` and adopts that row's parent.
        // `target == size()` without `nest` appends after the last row at the
        // last row's level. False when the move is a no-op, `target` is inside
        // the moved subtree, or a `nest` target is out of range.
        bool move_row(std::size_t from, std::size_t target, bool nest);

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

        // The value text shown in the table. Empty for a script row, which has
        // no value.
        [[nodiscard]] std::string display_value(std::size_t index) const;

        // Parses `text` for the entry's type and encodes it. Malformed input
        // returns invalid_argument, and so does a script row, which has no
        // value to encode; no target access happens here.
        [[nodiscard]] std::expected<std::vector<std::byte>, process::AccessError>
        encode_value(std::size_t index, std::string_view text) const;

        // The items a freeze pass must write at most every `interval` seconds:
        // one per active value entry with cached bytes. Script rows are never
        // included. Advances the timer.
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
