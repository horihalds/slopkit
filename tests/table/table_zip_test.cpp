#include <catch2/catch.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "table/table_zip.hpp"

namespace
{
    namespace fs = std::filesystem;

    using slopkit::table::ArchiveMember;
    using slopkit::table::is_zip_archive;
    using slopkit::table::read_archive;
    using slopkit::table::write_archive;

    fs::path scratch_file(std::string_view name)
    {
        const fs::path  directory = fs::path(SLOPKIT_TMP_DIR) / "table_zip_test";
        std::error_code error;
        fs::create_directories(directory, error);
        return directory / name;
    }
} // namespace

TEST_CASE("a table archive round-trips every member's text", "[table]")
{
    const fs::path path = scratch_file("roundtrip.skt");
    fs::remove(path);

    const std::vector<ArchiveMember> members {
        {       .name = "version.txt",                                          .text = "slopkit-table 3\n"},
        {.name = "entries/health.txt",                   .text = "type=i32 hex=0 size=4 expr=\"game+10\"\n"},
        { .name = "entries/empty.txt",                                                           .text = ""},
        { .name = "entries/multi.txt",                                       .text = "line one\nline two\n"},
        {         .name = "index.txt", .text = "entries/health.txt\nentries/empty.txt\nentries/multi.txt\n"},
    };

    REQUIRE(write_archive(path, members).has_value());
    CHECK(is_zip_archive(path));

    const auto read = read_archive(path);
    REQUIRE(read.has_value());
    REQUIRE(read->size() == members.size());
    for (std::size_t index = 0; index < members.size(); ++index)
    {
        CHECK((*read)[index].name == members[index].name);
        CHECK((*read)[index].text == members[index].text);
    }

    fs::remove(path);
}

TEST_CASE("writing an archive replaces an existing file without leftovers", "[table]")
{
    const fs::path path = scratch_file("replace.skt");
    fs::remove(path);

    const std::vector<ArchiveMember> first {
        {.name = "a.txt", .text = "one"}
    };
    REQUIRE(write_archive(path, first).has_value());

    const std::vector<ArchiveMember> second {
        {.name = "b.txt", .text = "two"}
    };
    REQUIRE(write_archive(path, second).has_value());

    const auto read = read_archive(path);
    REQUIRE(read.has_value());
    REQUIRE(read->size() == 1);
    CHECK((*read)[0].name == "b.txt");
    CHECK((*read)[0].text == "two");
    CHECK_FALSE(fs::exists(fs::path(path.string() + ".tmp")));

    fs::remove(path);
}

TEST_CASE("reading a missing path or a plain text file fails", "[table]")
{
    const fs::path missing = scratch_file("missing.skt");
    fs::remove(missing);
    CHECK_FALSE(is_zip_archive(missing));
    CHECK_FALSE(read_archive(missing).has_value());

    const fs::path plain = scratch_file("plain.skt");
    fs::remove(plain);
    {
        std::ofstream file(plain);
        file << "slopkit-table 3\n";
    }
    CHECK_FALSE(is_zip_archive(plain));
    CHECK_FALSE(read_archive(plain).has_value());

    fs::remove(plain);
}

TEST_CASE("a truncated archive keeps the signature but fails to read", "[table]")
{
    const fs::path path = scratch_file("truncated.skt");
    fs::remove(path);

    const std::vector<ArchiveMember> members {
        {.name = "version.txt", .text = "slopkit-table 3\n"}
    };
    REQUIRE(write_archive(path, members).has_value());

    // Cut the archive after the local file header: the signature still matches,
    // but the central directory is gone.
    fs::resize_file(path, 10);
    CHECK(is_zip_archive(path));
    CHECK_FALSE(read_archive(path).has_value());

    fs::remove(path);
}
