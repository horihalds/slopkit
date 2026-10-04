#pragma once

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

} // namespace slopkit::ui
