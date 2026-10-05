#include <catch2/catch.hpp>

#include "support/sandbox_helpers.hpp"

TEST_CASE("the sandbox exposes one field per slopkit value type", "[sandbox]")
{
    SECTION("each value type maps to a field of the right size")
    {
        using slopkit::scan::value_size;
        using slopkit::scan::ValueType;

        static_assert(std::is_same_v<decltype(Values::byte_value), std::uint8_t>);
        static_assert(std::is_same_v<decltype(Values::small_value), std::int16_t>);
        static_assert(std::is_same_v<decltype(Values::health), std::int32_t>);
        static_assert(std::is_same_v<decltype(Values::score), std::int64_t>);
        static_assert(std::is_same_v<decltype(Values::speed), float>);
        static_assert(std::is_same_v<decltype(Values::precision), double>);
        static_assert(std::is_same_v<decltype(Values::banner), std::array<char, 32>>);
        static_assert(std::is_same_v<decltype(Values::pattern), std::array<std::uint8_t, 8>>);

        CHECK(sizeof(decltype(Values::byte_value)) == value_size(ValueType::byte));
        CHECK(sizeof(decltype(Values::small_value)) == value_size(ValueType::int16));
        CHECK(sizeof(decltype(Values::health)) == value_size(ValueType::int32));
        CHECK(sizeof(decltype(Values::score)) == value_size(ValueType::int64));
        CHECK(sizeof(decltype(Values::speed)) == value_size(ValueType::float32));
        CHECK(sizeof(decltype(Values::precision)) == value_size(ValueType::float64));
        CHECK(std::tuple_size_v<decltype(Values::banner)> > std::string_view {"SLOPKIT-PRACTICE-TARGET"}.size());
        CHECK(std::tuple_size_v<decltype(Values::pattern)> == 8);
    }

    SECTION("every field lies inside the Values object")
    {
        constexpr std::size_t size = sizeof(Values);

        CHECK(offsetof(Values, byte_value) + sizeof(Values::byte_value) <= size);
        CHECK(offsetof(Values, small_value) + sizeof(Values::small_value) <= size);
        CHECK(offsetof(Values, health) + sizeof(Values::health) <= size);
        CHECK(offsetof(Values, score) + sizeof(Values::score) <= size);
        CHECK(offsetof(Values, speed) + sizeof(Values::speed) <= size);
        CHECK(offsetof(Values, precision) + sizeof(Values::precision) <= size);
        CHECK(offsetof(Values, banner) + sizeof(Values::banner) <= size);
        CHECK(offsetof(Values, pattern) + sizeof(Values::pattern) <= size);
        CHECK(offsetof(Values, drift) + sizeof(Values::drift) <= size);
    }
}

TEST_CASE("advance animates only health, score and drift", "[sandbox]")
{
    slopkit::sandbox::reset();
    const Values before = slopkit::sandbox::values();

    slopkit::sandbox::advance();
    const Values& after = slopkit::sandbox::values();

    CHECK(after.health == before.health - 1);
    CHECK(after.score == before.score + slopkit::sandbox::kScoreStep);
    CHECK(after.drift == before.drift + slopkit::sandbox::kDriftStep);

    CHECK(after.byte_value == before.byte_value);
    CHECK(after.small_value == before.small_value);
    CHECK(after.speed == before.speed);
    CHECK(after.precision == before.precision);
    CHECK(after.banner == before.banner);
    CHECK(after.pattern == before.pattern);
}

TEST_CASE("advance wraps the int32 health instead of overflowing", "[sandbox]")
{
    slopkit::sandbox::reset();
    slopkit::sandbox::values().health = slopkit::sandbox::kHealthFloor;
    slopkit::sandbox::advance();
    CHECK(slopkit::sandbox::values().health == slopkit::sandbox::kHealthStart);

    slopkit::sandbox::values().health = std::numeric_limits<std::int32_t>::min();
    slopkit::sandbox::advance();
    CHECK(slopkit::sandbox::values().health == slopkit::sandbox::kHealthStart);
}

TEST_CASE("the heap marker holds the ASCII marker at a stable address", "[sandbox]")
{
    const std::span<std::byte> marker = slopkit::sandbox::heap_marker();
    REQUIRE(marker.size() >= std::string_view {"SLOPKIT-SANDBOX-MARKER"}.size());

    const std::string_view text(reinterpret_cast<const char*>(marker.data()),
                                std::string_view {"SLOPKIT-SANDBOX-MARKER"}.size());
    CHECK(text == "SLOPKIT-SANDBOX-MARKER");
    CHECK(slopkit::sandbox::heap_marker().data() == marker.data());
}

TEST_CASE("print_layout emits one parseable line per field", "[sandbox]")
{
    std::ostringstream out;
    slopkit::sandbox::print_layout(out);
    const std::string text = out.str();

    for (const std::string_view name : {"byte_value",
                                        "small_value",
                                        "health",
                                        "score",
                                        "speed",
                                        "precision",
                                        "banner",
                                        "pattern",
                                        "drift",
                                        "heap_marker"})
    {
        const auto at = text.find(name);
        REQUIRE(at != std::string::npos);
        const std::string line = text.substr(at, text.find('\n', at) - at);
        CHECK(line.find(" address=0x") != std::string::npos);
        CHECK(line.find(" size=") != std::string::npos);
        CHECK(line.find(" value=") != std::string::npos);
    }

    CHECK(std::ranges::count(text, '\n') == 10);
}

TEST_CASE("the scan engine finds the sandbox values in the current process", "[sandbox][scan]")
{
    slopkit::sandbox::reset();

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    auto session = access.attach(static_cast<slopkit::process::ProcessId>(::getpid()), "linux-proc");
    REQUIRE(session.has_value());

    SECTION("the int32 health field")
    {
        const std::uint64_t address = address_of(slopkit::sandbox::values().health);

        ScanConfig config;
        config.type       = ScanType::exact_value;
        config.value_type = ValueType::int32;
        config.value      = static_cast<std::int64_t>(slopkit::sandbox::values().health);

        CHECK(scan_finds(*session, config, address));
    }

    SECTION("the string banner")
    {
        const std::uint64_t address = address_of(slopkit::sandbox::values().banner);

        ScanConfig config;
        config.type       = ScanType::exact_value;
        config.value_type = ValueType::string;
        config.value      = std::string("SLOPKIT-PRACTICE-TARGET");

        CHECK(scan_finds(*session, config, address));
    }

    SECTION("the heap marker's ASCII marker")
    {
        const std::span<std::byte> marker  = slopkit::sandbox::heap_marker();
        const std::uint64_t        address = reinterpret_cast<std::uintptr_t>(marker.data());

        ScanConfig config;
        config.type       = ScanType::exact_value;
        config.value_type = ValueType::string;
        config.value      = std::string("SLOPKIT-SANDBOX-MARKER");

        CHECK(scan_finds(*session, config, address));
    }
}
