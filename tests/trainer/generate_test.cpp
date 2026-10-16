#include <catch2/catch.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "table/address_table.hpp"
#include "trainer/generate.hpp"
#include "trainer/payload.hpp"

namespace
{
    using slopkit::scan::ValueType;
    using slopkit::table::AddressEntry;
    using slopkit::table::AddressTable;
    using slopkit::table::EntryKind;
    using slopkit::trainer::Request;
    using slopkit::trainer::TargetOs;

    constexpr std::string_view kTemplateBytes {"FAKE-TRAINER-TEMPLATE"};

    std::vector<std::filesystem::path> only(const std::filesystem::path& directory)
    {
        return {directory};
    }

    std::filesystem::path scratch_directory(std::string_view name)
    {
        const std::filesystem::path directory = std::filesystem::path {SLOPKIT_TMP_DIR} / name;
        std::error_code             error;
        std::filesystem::remove_all(directory, error);
        std::filesystem::create_directories(directory, error);
        REQUIRE_FALSE(error);
        return directory;
    }

    void write_file(const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream stream {path, std::ios::binary | std::ios::trunc};
        REQUIRE(stream.good());
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    std::vector<std::byte> read_file(const std::filesystem::path& path)
    {
        std::ifstream stream {path, std::ios::binary};
        REQUIRE(stream.good());
        const std::vector<char> text {std::istreambuf_iterator<char> {stream}, std::istreambuf_iterator<char> {}};
        return std::vector<std::byte> {reinterpret_cast<const std::byte*>(text.data()),
                                       reinterpret_cast<const std::byte*>(text.data()) + text.size()};
    }

    AddressEntry value_entry(
        std::string description, std::string expression, ValueType type, std::string bytes, bool frozen, bool hex)
    {
        AddressEntry entry;
        entry.kind        = EntryKind::value;
        entry.description = std::move(description);
        entry.expression  = std::move(expression);
        entry.type        = type;
        entry.address     = 0x1234'5678;
        entry.active      = frozen;
        entry.hex         = hex;
        for (const char value : bytes)
        {
            entry.bytes.push_back(static_cast<std::byte>(value));
        }
        return entry;
    }

    AddressTable practice_table()
    {
        AddressTable table;
        table.add(value_entry("Health", "practice+1f2a", ValueType::int32, std::string(4, '\x2a'), true, false));
        table.add(value_entry("Score", "practice+1f2e", ValueType::float32, std::string(4, '\x01'), false, true));
        static_cast<void>(table.add_script("No reload", "return true"));
        static_cast<void>(table.structures().add_class("Player", "practice"));
        return table;
    }

    Request request_for(const AddressTable& table, const std::filesystem::path& output)
    {
        Request request;
        request.table            = &table;
        request.process_name     = "practice";
        request.pointer_size     = 8;
        request.linux_target     = true;
        request.windows_target   = false;
        request.output_directory = output;
        request.base_name        = "practice";
        return request;
    }
}

TEST_CASE("generation appends the table's value entries to a copy of the template", "[trainer][generate]")
{
    const std::filesystem::path templates = scratch_directory("trainer_generate_templates");
    write_file(templates / "slopkit-trainer", kTemplateBytes);

    const std::filesystem::path output = scratch_directory("trainer_generate_output");
    const AddressTable          table  = practice_table();

    const auto result = slopkit::trainer::generate(request_for(table, output), only(templates));
    REQUIRE(result.has_value());
    CHECK(result->value_entries == 2);
    CHECK(result->script_entries == 1);
    CHECK(result->errors.empty());
    REQUIRE(result->written.size() == 1);
    CHECK(result->written.front().target == TargetOs::linux);
    CHECK(result->written.front().path.filename() == "practice.trainer");
    CHECK_FALSE(std::filesystem::exists(output / "practice.trainer.tmp"));

    const auto image = read_file(output / "practice.trainer");
    REQUIRE(image.size() > kTemplateBytes.size());
    CHECK(std::string_view {reinterpret_cast<const char*>(image.data()), kTemplateBytes.size()} == kTemplateBytes);

    const auto payload = slopkit::trainer::decode_payload(image);
    REQUIRE(payload.has_value());
    CHECK(payload->target_os == slopkit::trainer::kTargetLinux);
    CHECK(payload->pointer_size == 8);
    CHECK(payload->process_name == "practice");
    REQUIRE(payload->entries.size() == 2);

    const auto& health = payload->entries[0];
    CHECK(health.description == "Health");
    CHECK(health.expression == "practice+1f2a");
    CHECK(health.type == ValueType::int32);
    CHECK(health.last_address == 0x1234'5678);
    CHECK(health.frozen);
    CHECK_FALSE(health.hex);
    CHECK(health.bytes == std::vector<std::byte>(4, std::byte {0x2a}));

    const auto& score = payload->entries[1];
    CHECK(score.description == "Score");
    CHECK(score.type == ValueType::float32);
    CHECK_FALSE(score.frozen);
    CHECK(score.hex);

    REQUIRE(payload->scripts.size() == 1);
    CHECK(payload->scripts.front().description == "No reload");
    CHECK(payload->scripts.front().source == "return true");

    REQUIRE(payload->classes.size() == 2);
    CHECK(payload->classes.front().name == "reclass/index.txt");
    CHECK(payload->classes.front().text.find("reclass/classes/Player.txt") != std::string::npos);
    CHECK(payload->classes.back().name == "reclass/classes/Player.txt");
}

TEST_CASE("generating both targets writes one file per target", "[trainer][generate]")
{
    const std::filesystem::path templates = scratch_directory("trainer_generate_both_templates");
    write_file(templates / "slopkit-trainer", kTemplateBytes);
    write_file(templates / "slopkit-trainer.exe", "MZ-FAKE-TRAINER-TEMPLATE");

    const std::filesystem::path output = scratch_directory("trainer_generate_both_output");
    const AddressTable          table  = practice_table();

    auto request           = request_for(table, output);
    request.windows_target = true;

    const auto result = slopkit::trainer::generate(request, only(templates));
    REQUIRE(result.has_value());
    REQUIRE(result->written.size() == 2);
    CHECK(result->written[0].path.filename() == "practice.trainer");
    CHECK(result->written[1].path.filename() == "practice.trainer.exe");
    CHECK(std::filesystem::is_regular_file(output / "practice.trainer"));
    CHECK(std::filesystem::is_regular_file(output / "practice.trainer.exe"));

    const auto linux_payload = slopkit::trainer::decode_payload(read_file(output / "practice.trainer"));
    REQUIRE(linux_payload.has_value());
    CHECK(linux_payload->target_os == slopkit::trainer::kTargetLinux);

    const auto windows_payload = slopkit::trainer::decode_payload(read_file(output / "practice.trainer.exe"));
    REQUIRE(windows_payload.has_value());
    CHECK(windows_payload->target_os == slopkit::trainer::kTargetWindows);
}

TEST_CASE("a missing template is reported for that target only", "[trainer][generate]")
{
    const std::filesystem::path templates = scratch_directory("trainer_generate_partial_templates");
    write_file(templates / "slopkit-trainer", kTemplateBytes);

    const std::filesystem::path output = scratch_directory("trainer_generate_partial_output");
    const AddressTable          table  = practice_table();

    auto request           = request_for(table, output);
    request.windows_target = true;

    const auto result = slopkit::trainer::generate(request, only(templates));
    REQUIRE(result.has_value());
    REQUIRE(result->written.size() == 1);
    CHECK(result->written.front().path.filename() == "practice.trainer");
    REQUIRE(result->errors.size() == 1);
    CHECK(result->errors.front().find("windows") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(output / "practice.trainer.exe"));
}

TEST_CASE("a target that cannot be written reports an error and leaves no partial file", "[trainer][generate]")
{
    const std::filesystem::path templates = scratch_directory("trainer_generate_blocked_templates");
    write_file(templates / "slopkit-trainer", kTemplateBytes);

    const std::filesystem::path output = scratch_directory("trainer_generate_blocked_output");
    std::error_code             error;
    std::filesystem::create_directory(output / "practice.trainer.tmp", error);
    REQUIRE_FALSE(error);

    const AddressTable table = practice_table();

    const auto result = slopkit::trainer::generate(request_for(table, output), only(templates));
    REQUIRE(result.has_value());
    CHECK(result->written.empty());
    REQUIRE(result->errors.size() == 1);
    CHECK_FALSE(std::filesystem::is_regular_file(output / "practice.trainer"));
}

TEST_CASE("a table that carries only scripts still generates", "[trainer][generate]")
{
    const std::filesystem::path templates = scratch_directory("trainer_generate_scripts_only_templates");
    write_file(templates / "slopkit-trainer", kTemplateBytes);
    const std::filesystem::path output = scratch_directory("trainer_generate_scripts_only_output");

    AddressTable scripts;
    static_cast<void>(scripts.add_script("No reload", "return true"));

    const auto result = slopkit::trainer::generate(request_for(scripts, output), only(templates));
    REQUIRE(result.has_value());
    CHECK(result->value_entries == 0);
    CHECK(result->script_entries == 1);
    CHECK(result->errors.empty());
    REQUIRE(result->written.size() == 1);

    const auto payload = slopkit::trainer::decode_payload(read_file(output / "practice.trainer"));
    REQUIRE(payload.has_value());
    CHECK(payload->entries.empty());
    REQUIRE(payload->scripts.size() == 1);
    CHECK(payload->scripts.front().description == "No reload");
    CHECK(payload->scripts.front().source == "return true");
}

TEST_CASE("a table with neither value entries nor scripts is refused", "[trainer][generate]")
{
    const std::filesystem::path templates = scratch_directory("trainer_generate_empty_templates");
    write_file(templates / "slopkit-trainer", kTemplateBytes);
    const std::filesystem::path output = scratch_directory("trainer_generate_empty_output");

    const AddressTable empty;

    const auto refusal = slopkit::trainer::generate(request_for(empty, output), only(templates));
    REQUIRE_FALSE(refusal.has_value());
    CHECK(refusal.error().find("value entries") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(output / "practice.trainer"));
}

TEST_CASE("a file that carries no payload is not mistaken for one", "[trainer][generate]")
{
    const std::filesystem::path output = scratch_directory("trainer_generate_bogus_output");
    const std::filesystem::path file   = output / "bogus.trainer";
    write_file(file, "MZ-not-a-trainer");

    CHECK_FALSE(slopkit::trainer::decode_payload(read_file(file)).has_value());

    const std::vector<std::byte> truncated(64, std::byte {0x7F});
    CHECK_FALSE(slopkit::trainer::decode_payload(truncated).has_value());
}

TEST_CASE("a payload round-trips its scripts and classes", "[trainer][payload]")
{
    slopkit::trainer::Payload source;
    source.target_os    = slopkit::trainer::kTargetWindows;
    source.pointer_size = 8;
    source.process_name = "practice";

    slopkit::trainer::PayloadEntry entry;
    entry.description  = "Health";
    entry.type         = ValueType::int32;
    entry.frozen       = true;
    entry.expression   = "practice+1f2a";
    entry.last_address = 0x1234'5678;
    entry.bytes.assign(4, std::byte {0x2a});
    source.entries.push_back(std::move(entry));

    source.scripts.push_back({"No reload", "return true"});
    source.classes.push_back({"Player", "int32 health @0x10"});

    const auto image   = slopkit::trainer::encode_payload(source);
    const auto decoded = slopkit::trainer::decode_payload(image);
    REQUIRE(decoded.has_value());
    CHECK(decoded->target_os == slopkit::trainer::kTargetWindows);
    CHECK(decoded->process_name == "practice");
    REQUIRE(decoded->entries.size() == 1);
    CHECK(decoded->entries.front().expression == "practice+1f2a");
    REQUIRE(decoded->scripts.size() == 1);
    CHECK(decoded->scripts.front().description == "No reload");
    CHECK(decoded->scripts.front().source == "return true");
    REQUIRE(decoded->classes.size() == 1);
    CHECK(decoded->classes.front().name == "Player");
    CHECK(decoded->classes.front().text == "int32 health @0x10");
}

TEST_CASE("a payload from another slopkit build is rejected as an unsupported version", "[trainer][payload]")
{
    slopkit::trainer::Payload source;
    source.process_name = "practice";
    source.scripts.push_back({"No reload", "return true"});

    auto image = slopkit::trainer::encode_payload(source);
    image[8]   = std::byte {1};
    image[9]   = std::byte {0};
    image[10]  = std::byte {0};
    image[11]  = std::byte {0};

    const auto decoded = slopkit::trainer::decode_payload(image);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().find("version") != std::string::npos);
}

TEST_CASE("a payload with a script record cut short is reported as malformed", "[trainer][payload]")
{
    slopkit::trainer::Payload source;
    source.process_name = "practice";
    source.scripts.push_back({"No reload", "return true"});

    auto image = slopkit::trainer::encode_payload(source);
    // Claim two script records where only one was written.
    image[28]  = std::byte {2};
    image[29]  = std::byte {0};
    image[30]  = std::byte {0};
    image[31]  = std::byte {0};

    const auto decoded = slopkit::trainer::decode_payload(image);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().find("malformed") != std::string::npos);
}
