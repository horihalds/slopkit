#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "table/address_table.hpp"

namespace slopkit::table
{
    std::expected<void, std::string> save(const std::filesystem::path& path, const AddressTable& table);
    std::expected<void, std::string> load(const std::filesystem::path& path, AddressTable& table);

    [[nodiscard]] std::string_view type_token(scan::ValueType type) noexcept;

}
