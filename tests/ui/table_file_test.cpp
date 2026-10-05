#include <catch2/catch.hpp>

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
