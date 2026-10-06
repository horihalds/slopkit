#include <catch2/catch.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "platform/linux/proc_text.hpp"

namespace
{
    std::filesystem::path scratch_file(std::string_view name)
    {
        const auto      directory = std::filesystem::path(SLOPKIT_TMP_DIR) / "proc_text_test";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return directory / name;
    }
} // namespace

TEST_CASE("trim removes leading and trailing whitespace", "[platform]")
{
    using slopkit::platform::trim;

    CHECK(trim("  value \t\r\n") == "value");
    CHECK(trim("value") == "value");
    CHECK(trim("") == "");
    CHECK(trim("   \t\n") == "");
}

TEST_CASE("split_lines keeps the trailing empty line", "[platform]")
{
    using slopkit::platform::split_lines;

    const std::vector<std::string_view> lines = split_lines("one\ntwo\n");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "one");
    CHECK(lines[1] == "two");
    CHECK(lines[2] == "");

    const std::vector<std::string_view> single = split_lines("one");
    REQUIRE(single.size() == 1);
    CHECK(single[0] == "one");

    const std::vector<std::string_view> empty = split_lines("");
    REQUIRE(empty.size() == 1);
    CHECK(empty[0] == "");
}

TEST_CASE("read_file returns the whole file or nothing", "[platform]")
{
    using slopkit::platform::read_file;

    const auto path = scratch_file("payload.txt");
    {
        std::ofstream file(path, std::ios::binary);
        file << "hello\nworld";
    }
    const auto content = read_file(path);
    REQUIRE(content.has_value());
    CHECK(*content == "hello\nworld");

    CHECK_FALSE(read_file(path.parent_path() / "missing.txt").has_value());
}

TEST_CASE("proc_entry points into the process's proc directory", "[platform]")
{
    CHECK(slopkit::platform::proc_entry(1234, "cmdline") == "/proc/1234/cmdline");
}

TEST_CASE("is_all_digits accepts only non-empty digit runs", "[platform]")
{
    using slopkit::platform::is_all_digits;

    CHECK(is_all_digits("0"));
    CHECK(is_all_digits("12345"));
    CHECK_FALSE(is_all_digits(""));
    CHECK_FALSE(is_all_digits("12a"));
    CHECK_FALSE(is_all_digits("a12"));
    CHECK_FALSE(is_all_digits("1 2"));
}

TEST_CASE("basename_lower keeps the final component in lower case", "[platform]")
{
    using slopkit::platform::basename_lower;

    CHECK(basename_lower("/usr/bin/Wine") == "wine");
    CHECK(basename_lower("C:\\windows\\winedevice.EXE") == "winedevice.exe");
    CHECK(basename_lower("winedevice.exe") == "winedevice.exe");
    CHECK(basename_lower("") == "");
}
