#pragma once

#include <array>
#include <string>

#include "table/address_table.hpp"

namespace slopkit::ui::dialogs
{

    // The Add Address dialog: description, address, value type and, for the
    // dynamic types, the number of bytes to track.
    class AddAddress
    {
    public:
        explicit AddAddress(table::AddressTable& table);

        void draw(bool& open);

    private:
        void commit();

        table::AddressTable&  table_;
        std::array<char, 128> description_ {};
        std::array<char, 32>  address_ {};
        std::array<char, 16>  size_ {"32"};
        int                   type_index_ {2};
        bool                  hex_ {false};
        std::string           status_;
    };

} // namespace slopkit::ui::dialogs
