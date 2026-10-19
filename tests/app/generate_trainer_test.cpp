#include <catch2/catch.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "app/cli.hpp"
#include "table/address_table.hpp"
#include "table/serializer.hpp"
#include "trainer/payload.hpp"

namespace
{
    using slopkit::scan::ValueType;
    using slopkit::table::AddressEntry;
    using slopkit::table::AddressTable;
    using slopkit::table::EntryKind;

    struct Fixture
    {
        std::filesystem::path directory;
        std::filesystem::path table;
    };

    Fixture prepare(std::string_view name, const AddressTable& table)
    {
        const std::filesystem::path directory = std::filesystem::path {SLOPKIT_TMP_DIR} / "cli_trainer" / name;
        std::error_code             error;
        std::filesystem::remove_all(directory, error);
        std::filesystem::create_directories(directory, error);
        REQUIRE_FALSE(error);

        const std::filesystem::path path  = directory / "practice.skt";
        const auto                  saved = slopkit::table::save(path, table);
        REQUIRE(saved.has_value());
        return Fixture {directory, path};
    }

    AddressTable practice_table()
    {
        AddressEntry health;
        health.kind        = EntryKind::value;
        health.description = "Health";
        health.expression  = "practice+1f2a";
        health.type        = ValueType::int32;
        health.bytes       = std::vector<std::byte>(4, std::byte {42});

        AddressTable table;
        table.add(health);
        static_cast<void>(table.add_script("No reload", "return true"));
        table.settings().target_process = "practice";
        return table;
    }

    AddressTable script_only_table()
    {
        AddressTable table;
        static_cast<void>(table.add_script("No reload", "return true"));
        return table;
    }

    std::vector<std::byte> read_file(const std::filesystem::path& path)
    {
        std::ifstream stream {path, std::ios::binary};
        REQUIRE(stream.good());
        const std::vector<char> text {std::istreambuf_iterator<char> {stream}, std::istreambuf_iterator<char> {}};
        return std::vector<std::byte> {reinterpret_cast<const std::byte*>(text.data()),
                                       reinterpret_cast<const std::byte*>(text.data()) + text.size()};
    }

    bool windows_template_exists()
    {
        return std::filesystem::is_regular_file(std::filesystem::path {SLOPKIT_TRAINER_DIR} / "slopkit-trainer.exe");
    }
}

TEST_CASE("the generate-trainer command writes a trainer and prints its path", "[app][trainer]")
{
    const auto fixture = prepare("linux", practice_table());

    std::ostringstream out;
    std::ostringstream err;
    const auto         code = slopkit::app::generate_trainer(out,
                                                             err,
                                                             std::vector<std::string_view> {"--table",
                                                                                            fixture.table.string(),
                                                                                            "--target",
                                                                                            "linux",
                                                                                            "--out",
                                                                                            fixture.directory.string(),
                                                                                            "--name",
                                                                                            "practice"});

    CHECK(code == 0);
    CHECK(err.str().empty());

    const auto produced = fixture.directory / "practice.trainer";
    CHECK(out.str() == produced.string() + "\n");
    REQUIRE(std::filesystem::is_regular_file(produced));

    // The payload carries the recorded process name, the table's value entries
    // and its script entry.
    const auto payload = slopkit::trainer::decode_payload(read_file(produced));
    REQUIRE(payload.has_value());
    CHECK(payload->process_name == "practice");
    REQUIRE(payload->value_count() == 1);
    CHECK(payload->values().front()->description == "Health");
    REQUIRE(payload->script_count() == 1);
    CHECK(payload->script_records().front().description == "No reload");
    CHECK(payload->script_records().front().source == "return true");
}

TEST_CASE("the command defaults to both targets and to the table's own name", "[app][trainer]")
{
    const auto fixture = prepare("both", practice_table());

    std::ostringstream out;
    std::ostringstream err;
    const auto         code = slopkit::app::generate_trainer(
        out,
        err,
        std::vector<std::string_view> {"--table", fixture.table.string(), "--out", fixture.directory.string()});

    const bool windows      = windows_template_exists();
    const auto linux_file   = fixture.directory / "practice.trainer";
    const auto windows_file = fixture.directory / "practice.trainer.exe";

    CHECK(code == (windows ? 0 : 1));
    CHECK(std::filesystem::is_regular_file(linux_file));
    CHECK(std::filesystem::is_regular_file(windows_file) == windows);
    if (!windows)
    {
        CHECK(err.str().find("windows") != std::string::npos);
    }
}

TEST_CASE("the command generates a trainer for a table that carries only scripts", "[app][trainer]")
{
    const auto fixture = prepare("scripts", script_only_table());

    std::ostringstream out;
    std::ostringstream err;
    const auto         code = slopkit::app::generate_trainer(
        out,
        err,
        std::vector<std::string_view> {
            "--table", fixture.table.string(), "--target", "linux", "--out", fixture.directory.string()});

    const auto produced = fixture.directory / "practice.trainer";
    CHECK(code == 0);
    CHECK(err.str().empty());
    CHECK(out.str() == produced.string() + "\n");
    REQUIRE(std::filesystem::is_regular_file(produced));

    const auto payload = slopkit::trainer::decode_payload(read_file(produced));
    REQUIRE(payload.has_value());
    CHECK(payload->value_count() == 0);
    REQUIRE(payload->script_count() == 1);
    CHECK(payload->script_records().front().description == "No reload");
    CHECK(payload->script_records().front().source == "return true");
}

TEST_CASE("the command rejects bad usage and an unreadable table", "[app][trainer]")
{
    const auto fixture = prepare("usage", practice_table());

    std::ostringstream out;
    std::ostringstream err;

    CHECK(slopkit::app::generate_trainer(out, err, std::vector<std::string_view> {}) == 2);
    CHECK(err.str().find("--table") != std::string::npos);

    err.str({});
    CHECK(slopkit::app::generate_trainer(
              out, err, std::vector<std::string_view> {"--table", fixture.table.string(), "--target", "mips"})
          == 2);
    CHECK(err.str().find("unknown target") != std::string::npos);

    err.str({});
    CHECK(slopkit::app::generate_trainer(
              out, err, std::vector<std::string_view> {"--table", fixture.table.string(), "--oops"})
          == 2);
    CHECK(err.str().find("unknown generate-trainer option") != std::string::npos);

    err.str({});
    CHECK(slopkit::app::generate_trainer(
              out,
              err,
              std::vector<std::string_view> {
                  "--table", (fixture.directory / "missing.skt").string(), "--target", "linux"})
          == 2);
    CHECK(err.str().find("cannot read the table") != std::string::npos);
}
