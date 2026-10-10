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
    //   version.txt         the single line `slopkit-table 5`
    //   settings.txt        optional; `target="…" exe_path="…" auto_attach=0|1 match_exe_path=0|1`
    //   index.txt           one entry member per line, in row order, a child's line
    //                       carrying its parent: `entries/<name>.txt parent="entries/<parent>.txt"`
    //   entries/<name>.txt  a value entry:
    //                       `type=i32 hex=0 size=4 expr="game+10"`
    //   entries/<name>.lua  a script entry: the Lua source verbatim
    //
    // The member name is the entry's description turned into a file name (see
    // `entry_member_name`) and is not repeated in the body, so a description the
    // file system cannot hold comes back sanitised. The extension is the kind:
    // `.txt` is a value entry, `.lua` a script entry, and any other extension is
    // a load error. A `.lua` body is the script and nothing else — no header and
    // no added newline. `size` is written explicitly because the cached value
    // bytes are not; the frozen flag and the last read values are session-only. An
    // entry without an expression is stored as the hex form of its address, which
    // `expr::parse` accepts as a literal. Both functions return a human-readable
    // message naming the offending member.
    //
    // The writer emits the current `slopkit-table 5` token; the reader also
    // accepts `slopkit-table 4` (no nesting) and `slopkit-table 3` (no script
    // entries, so no `.lua` members). A parent named in index.txt must be listed
    // before its child; the parent id is not stored in an entry body because a
    // `.lua` body is the script verbatim.
    std::expected<void, std::string> save(const std::filesystem::path& path, const AddressTable& table);
    std::expected<void, std::string> load(const std::filesystem::path& path, AddressTable& table);

    // The stable token used for a value type in the file format.
    [[nodiscard]] std::string_view type_token(scan::ValueType type) noexcept;

} // namespace slopkit::table
