#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "process/access_worker.hpp"
#include "process/types.hpp"
#include "reclass/project.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"
#include "table/entry_name.hpp"
#include "table/table_settings.hpp"

namespace slopkit::table
{
    struct AddressEntry
    {
        std::uint64_t          id {};
        std::uint64_t          parent {};
        bool                   active {false};
        EntryKind              kind {EntryKind::value};
        std::string            description;
        std::uint64_t          address {};
        std::string            expression;
        scan::ValueType        type {scan::ValueType::int32};
        std::vector<std::byte> bytes;
        bool                   hex {false};
        std::string            script;
    };

    struct MergeSummary
    {
        std::size_t added {};
        std::size_t skipped {};
    };

    class AddressTable
    {
    public:
        void add(AddressEntry entry);
        void remove(std::size_t index);

        void remove_rows(std::span<const std::size_t> rows);
        void clear();

        [[nodiscard]] std::size_t add_script(std::string description, std::string script);

        bool set_script(std::size_t index, std::string script);

        bool set_description(std::size_t index, std::string description);

        void replace(std::vector<AddressEntry> entries);

        void move(std::size_t from, std::size_t to);

        [[nodiscard]] std::size_t depth_of(std::size_t index) const;

        [[nodiscard]] std::size_t subtree_size(std::size_t index) const;

        [[nodiscard]] bool is_within_subtree(std::size_t index, std::size_t ancestor) const;

        bool move_row(std::size_t from, std::size_t target, bool nest);

        MergeSummary merge(std::span<const AddressEntry> incoming);

        [[nodiscard]] std::span<AddressEntry>       entries() noexcept;
        [[nodiscard]] std::span<const AddressEntry> entries() const noexcept;
        [[nodiscard]] std::size_t                   size() const noexcept;
        [[nodiscard]] bool                          empty() const noexcept;
        [[nodiscard]] bool                          valid_index(std::size_t index) const noexcept;

        [[nodiscard]] int selected() const noexcept;
        void              set_selected(int index) noexcept;

        [[nodiscard]] std::span<const std::size_t> selected_rows() const noexcept;
        void                                       set_selected_rows(std::span<const std::size_t> rows) noexcept;
        [[nodiscard]] bool                         is_row_selected(std::size_t index) const noexcept;

        [[nodiscard]] std::string display_value(std::size_t index) const;

        [[nodiscard]] std::expected<std::vector<std::byte>, process::AccessError>
        encode_value(std::size_t index, std::string_view text) const;

        [[nodiscard]] std::vector<process::WriteItem> freeze_items(double now, double interval = 0.1);

        void apply_write(std::uint64_t entry_id, std::vector<std::byte> bytes);

        [[nodiscard]] TableSettings&       settings() noexcept;
        [[nodiscard]] const TableSettings& settings() const noexcept;

        [[nodiscard]] reclass::Project&       structures() noexcept;
        [[nodiscard]] const reclass::Project& structures() const noexcept;

    private:
        void erase_blocks(std::span<const std::pair<std::size_t, std::size_t>> blocks);

        [[nodiscard]] static std::size_t
        moved_row(std::size_t index, std::size_t from, std::size_t count, std::size_t insertion) noexcept;

        std::vector<AddressEntry> entries_;
        TableSettings             settings_;
        reclass::Project          structures_;
        int                       selected_ {-1};
        std::vector<std::size_t>  selected_rows_ {};
        double                    next_freeze_time_ {0.0};
        std::uint64_t             next_id_ {1};
    };

}
