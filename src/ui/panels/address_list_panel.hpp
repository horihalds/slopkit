#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "process/attachment.hpp"
#include "table/address_table.hpp"

namespace slopkit::ui::panels
{

    // The bottom zone: the editable address list with its context menu and the
    // footer popups. It owns no session; the shared attached target is used for
    // writes.
    class AddressListPanel
    {
    public:
        AddressListPanel(table::AddressTable& table, process::AttachedTarget& target);

        void draw();

        // Entry points used by the menu bar and the toolbar.
        void request_open();
        void request_save();
        void delete_selected();
        void toggle_freeze_selected();

        // Reports a freeze failure raised outside the panel.
        void report_freeze_error(std::string_view message);

        // A request to show an address, consumed by the Memory Viewer.
        [[nodiscard]] std::optional<std::uint64_t> take_browse_request();

    private:
        enum class EditField
        {
            none,
            description,
            value,
        };

        void draw_table();
        void draw_context_menu(std::size_t row);
        void draw_footer();
        void draw_file_popups();
        void start_edit(std::size_t row, EditField field);
        void commit_edit();
        void write_value_at(std::size_t row, std::string_view text);
        void set_status(std::string message, bool is_error);

        table::AddressTable&     table_;
        process::AttachedTarget& target_;

        std::size_t           editing_row_ {0};
        EditField             editing_field_ {EditField::none};
        bool                  edit_focus_pending_ {false};
        std::array<char, 256> edit_buffer_ {};

        int pending_delete_ {-1};

        std::optional<std::uint64_t> browse_request_;

        std::array<char, 512> table_path_ {"slopkit-table.txt"};
        bool                  open_requested_ {false};
        bool                  save_requested_ {false};

        std::string status_;
        bool        status_is_error_ {false};
    };

} // namespace slopkit::ui::panels
