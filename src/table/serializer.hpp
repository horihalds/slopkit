#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "table/address_table.hpp"

namespace slopkit::table
{

    // Table files are ZIP archives (the `.skt` extension) with a readable inner
    // layout:
    //
    //   version.txt         the single line `slopkit-table 3`
    //   settings.txt        optional; `target="…" exe_path="…" auto_attach=0|1 match_exe_path=0|1`
    //   index.txt           one `entries/<name>.txt` member per line, in row order
    //   entries/<name>.txt  one entry per member:
    //                       `type=i32 hex=0 size=4 expr="game+10"`
    //
    // The member name is the entry's description turned into a file name (see
    // `entry_member_name`) and is not repeated in the body, so a description the
    // file system cannot hold comes back sanitised. `size` is written explicitly
    // because the cached value bytes are not; the frozen flag and the last read
    // values are session-only. An entry without an expression is stored as the
    // hex form of its address, which `expr::parse` accepts as a literal. Both
    // functions return a human-readable message naming the offending member.
    std::expected<void, std::string> save(const std::filesystem::path& path, const AddressTable& table);
    std::expected<void, std::string> load(const std::filesystem::path& path, AddressTable& table);

    // The stable token used for a value type in the file format.
    [[nodiscard]] std::string_view type_token(scan::ValueType type) noexcept;

} // namespace slopkit::table
