#include <catch2/catch.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include <QDir>
#include <QString>

#include "ui/table_file.hpp"

TEST_CASE("a typed table name gets the .skt extension when it has none", "[ui]")
{
    using slopkit::ui::default_table_path;
    using slopkit::ui::table_file_path;

    CHECK(table_file_path(QStringLiteral("foo")) == QStringLiteral("foo.skt"));
    CHECK(table_file_path(QStringLiteral("/tmp/some dir/foo")) == QStringLiteral("/tmp/some dir/foo.skt"));
    CHECK(table_file_path(QStringLiteral("foo.skt")) == QStringLiteral("foo.skt"));
    CHECK(table_file_path(QStringLiteral("foo.txt")) == QStringLiteral("foo.txt"));
    CHECK(table_file_path(QString()) == default_table_path());
    CHECK(default_table_path() == QStringLiteral("untitled.skt"));
}

TEST_CASE("a target-derived table name is sanitised and always gets the .skt suffix", "[ui]")
{
    using slopkit::ui::default_table_path;
    using slopkit::ui::table_name_for_target;

    CHECK(table_name_for_target("firefox") == QStringLiteral("firefox.skt"));
    CHECK(table_name_for_target("python3.12") == QStringLiteral("python3.12.skt"));
    CHECK(table_name_for_target("weird/name") == QStringLiteral("weird_name.skt"));
    CHECK(table_name_for_target("weird\\name") == QStringLiteral("weird_name.skt"));
    CHECK(table_name_for_target("  spaced  ") == QStringLiteral("spaced.skt"));
    CHECK(table_name_for_target("") == default_table_path());
    CHECK(table_name_for_target(".") == default_table_path());
    CHECK(table_name_for_target("..") == default_table_path());
    CHECK(table_name_for_target("   ") == default_table_path());
}

TEST_CASE("the default table directory is a slopkit folder below an absolute location", "[ui]")
{
    using slopkit::ui::default_table_directory;

    const QString directory = default_table_directory();
    CHECK_FALSE(directory.isEmpty());
    CHECK(QDir::isAbsolutePath(directory));
    CHECK(directory.endsWith(QStringLiteral("/slopkit")));
    CHECK(default_table_directory() == directory); // stable across calls
}

TEST_CASE("ensuring a table directory creates the missing parents and is idempotent", "[ui]")
{
    using slopkit::ui::ensure_table_directory;

    const std::filesystem::path base = std::filesystem::path(SLOPKIT_TMP_DIR) / "table_file_test" / "nested" / "tables";
    std::error_code             error;
    std::filesystem::remove_all(base, error);

    const QString directory = QString::fromStdString(base.string());
    CHECK(ensure_table_directory(directory));
    CHECK(std::filesystem::is_directory(base));
    CHECK(ensure_table_directory(directory)); // a second call stays true

    // A path whose parent is a regular file cannot be created.
    const auto    blocker = base / "blocker";
    std::ofstream file(blocker);
    file << "x";
    file.close();
    CHECK_FALSE(ensure_table_directory(QString::fromStdString((blocker / "child").string())));
}
