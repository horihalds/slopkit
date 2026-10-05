#pragma once

#include <expected>
#include <filesystem>
#include <string>

#include "table/address_table.hpp"

namespace slopkit::table
{

    // Hand-rolled, line-oriented table file - no serialization dependency. The
    // format is one entry per line:
    //
    //   slopkit-table 2
    //   entry description="health" address=0x1234 type=i32 frozen=1 hex=0 value=64000000 expr="module+50"
    //
    // `value` is the entry's raw bytes as hex, so every value type round-trips.
    // `expr` is optional and carries the expression an entry re-resolves from; a
    // line without it (a v1 file) yields a plain absolute address. The version
    // token is advisory and is ignored by the loader, so v1 files load unchanged.
    // Both functions return a human-readable message on failure.

    std::expected<void, std::string> save(const std::filesystem::path& path, const AddressTable& table);
    std::expected<void, std::string> load(const std::filesystem::path& path, AddressTable& table);

    // The stable token used for a value type in the file format.
    [[nodiscard]] std::string_view type_token(scan::ValueType type) noexcept;

} // namespace slopkit::table
