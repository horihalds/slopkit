#pragma once

#include <string_view>

#include <QString>

namespace slopkit::ui
{
    [[nodiscard]] QString table_file_path(const QString& typed);

    [[nodiscard]] QString default_table_path();

    [[nodiscard]] QString table_name_for_target(std::string_view target_name);

    [[nodiscard]] QString default_table_directory();

    [[nodiscard]] bool ensure_table_directory(const QString& directory);

}
