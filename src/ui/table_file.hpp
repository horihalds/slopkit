#pragma once

#include <string_view>

#include <QString>

namespace slopkit::ui
{

    // Completes a user-typed table path with the table extension. A path that
    // already ends in a suffix is returned unchanged, so `foo` becomes
    // `foo.skt` while `foo.txt` stays `foo.txt`; an empty input falls back to
    // default_table_path().
    [[nodiscard]] QString table_file_path(const QString& typed);

    // The suggested file name for a table that has never been saved.
    [[nodiscard]] QString default_table_path();

    // The suggested file name for a table that has never been saved, derived
    // from the attached target's process name: the name is sanitised for use as
    // a file name and always gets the `.skt` suffix; an empty, `.` or `..` name
    // falls back to default_table_path().
    [[nodiscard]] QString table_name_for_target(std::string_view target_name);

    // The directory a new table is saved into: a `slopkit` folder inside the
    // per-user documents location, or inside $HOME when the platform reports
    // none.
    [[nodiscard]] QString default_table_directory();

    // Creates `directory` and its parents when it is missing, so the first save
    // into the default location cannot fail on a missing folder; false when it
    // could not be created.
    [[nodiscard]] bool ensure_table_directory(const QString& directory);

} // namespace slopkit::ui
