#include <catch2/catch.hpp>

#include "support/scan_helpers.hpp"

TEST_CASE("the compiled matcher agrees with the generic comparison", "[scan]")
{
    using slopkit::scan::Matcher;
    using slopkit::scan::ScanValue;

    std::mt19937                       rng(0xC0FFEEu);
    std::uniform_int_distribution<int> pick(0, 5);
    const std::array<int, 6>           alphabet {0x00, 0x01, 0x0F, 0x7F, 0xFF, 0x15};

    // The pre-matcher reference: the offsets the engine kept before, using the
    // fully generic comparison for every candidate.
    const auto reference =
        [](std::span<const std::byte> bytes, const ScanConfig& config, std::size_t window, std::size_t alignment)
    {
        std::vector<std::size_t> offsets;
        const bool               initial_unknown = config.type == ScanType::unknown_initial_value;
        for (std::size_t offset = 0; offset + window <= bytes.size(); offset += alignment)
        {
            if (initial_unknown
                || slopkit::scan::matches(config.type,
                                          config.value_type,
                                          bytes.subspan(offset, window),
                                          config.value,
                                          config.value_upper,
                                          config.hex))
            {
                offsets.push_back(offset);
            }
        }
        return offsets;
    };

    // Exactly what ScanEngine::run_first does with the matcher.
    const auto collected =
        [](std::span<const std::byte> bytes, const Matcher& matcher, std::size_t window, std::size_t alignment)
    {
        std::vector<std::size_t> offsets;
        if (matcher.matches_all())
        {
            for (std::size_t offset = 0; offset + window <= bytes.size(); offset += alignment)
            {
                offsets.push_back(offset);
            }
            return offsets;
        }
        for (std::size_t offset = 0; offset < bytes.size();)
        {
            const std::size_t hit = matcher.find(bytes, offset, bytes.size(), alignment);
            if (hit == bytes.size())
            {
                break;
            }
            offsets.push_back(hit);
            offset = hit + alignment;
        }
        return offsets;
    };

    struct Needle
    {
        ValueType type;
        ScanValue value;
    };

    const double              nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<Needle> needles {
        {      ValueType::byte,                                            ScanValue {std::int64_t {0}}},
        {      ValueType::byte,                                         ScanValue {std::int64_t {0x15}}},
        {      ValueType::byte,                                           ScanValue {std::int64_t {-1}}},
        {     ValueType::int16,                                            ScanValue {std::int64_t {0}}},
        {     ValueType::int16,                                       ScanValue {std::int64_t {0x1515}}},
        {     ValueType::int16,                                           ScanValue {std::int64_t {-1}}},
        {     ValueType::int32,                                            ScanValue {std::int64_t {0}}},
        {     ValueType::int32,                                           ScanValue {std::int64_t {15}}},
        {     ValueType::int32,                                           ScanValue {std::int64_t {-1}}},
        {     ValueType::int32,                                   ScanValue {std::int64_t {0x01020304}}},
        {     ValueType::int64,                                            ScanValue {std::int64_t {0}}},
        {     ValueType::int64,                                           ScanValue {std::int64_t {15}}},
        {     ValueType::int64,                                           ScanValue {std::int64_t {-1}}},
        {       ValueType::all,                                            ScanValue {std::int64_t {0}}},
        {       ValueType::all,                                           ScanValue {std::int64_t {15}}},
        {   ValueType::float32,                                                         ScanValue {0.0}},
        {   ValueType::float32,                                                        ScanValue {-0.0}},
        {   ValueType::float32,                                                         ScanValue {1.5}},
        {   ValueType::float32,                                                         ScanValue {nan}},
        {   ValueType::float64,                                                         ScanValue {0.0}},
        {   ValueType::float64,                                                        ScanValue {-0.0}},
        {   ValueType::float64,                                                         ScanValue {1.5}},
        {   ValueType::float64,                                                         ScanValue {nan}},
        {    ValueType::string,                                         ScanValue {std::string {"abc"}}},
        {    ValueType::string,                                           ScanValue {std::string {"a"}}},
        {ValueType::byte_array, ScanValue {std::vector<std::byte> {std::byte {0x15}, std::byte {0x00}}}},
        {ValueType::byte_array, ScanValue {std::vector<std::byte> {std::byte {0xFF}, std::byte {0xFF}}}},
        {ValueType::byte_array,                   ScanValue {std::vector<std::byte> {std::byte {0x00}}}},
    };

    const std::array<ScanType, 5>    scan_types {ScanType::exact_value,
                                                 ScanType::bigger_than,
                                                 ScanType::smaller_than,
                                                 ScanType::value_between,
                                                 ScanType::unknown_initial_value};
    const std::array<std::size_t, 4> alignments {1, 2, 4, 8};

    for (const auto& needle : needles)
    {
        for (const ScanType scan_type : scan_types)
        {
            ScanConfig config;
            config.type        = scan_type;
            config.value_type  = needle.type;
            config.value       = needle.value;
            config.value_upper = needle.value;
            if (std::holds_alternative<std::int64_t>(needle.value))
            {
                config.value_upper = ScanValue {std::get<std::int64_t>(needle.value) + 1};
            }
            else if (std::holds_alternative<double>(needle.value))
            {
                config.value_upper = ScanValue {std::get<double>(needle.value) + 1.0};
            }

            const std::size_t window = slopkit::scan::effective_size(config.value_type, config.value);
            if (window == 0)
            {
                continue;
            }

            const Matcher matcher = Matcher::build(config);
            REQUIRE(matcher.window() == window);
            REQUIRE(matcher.matches_all() == (scan_type == ScanType::unknown_initial_value));

            const std::vector<std::byte> needle_bytes = slopkit::scan::encode_value(config.value_type, config.value);

            for (const std::size_t alignment : alignments)
            {
                std::vector<std::byte> buffer(2048);
                for (auto& byte : buffer)
                {
                    byte = static_cast<std::byte>(alphabet[static_cast<std::size_t>(pick(rng))]);
                }
                if (!needle_bytes.empty() && needle_bytes.size() <= buffer.size())
                {
                    const std::size_t last = buffer.size() - needle_bytes.size();
                    for (const std::size_t at : {std::size_t {0}, std::size_t {64}, last})
                    {
                        std::copy(
                            needle_bytes.begin(), needle_bytes.end(), buffer.begin() + static_cast<std::ptrdiff_t>(at));
                    }
                }
                const std::span<const std::byte> bytes(buffer);

                INFO("value_type=" << static_cast<int>(needle.type) << " scan_type=" << static_cast<int>(scan_type)
                                   << " alignment=" << alignment);

                const bool initial_unknown = scan_type == ScanType::unknown_initial_value;
                bool       match_agrees    = true;
                for (std::size_t offset = 0; offset + window <= bytes.size(); ++offset)
                {
                    const bool expected = initial_unknown
                                       || slopkit::scan::matches(config.type,
                                                                 config.value_type,
                                                                 bytes.subspan(offset, window),
                                                                 config.value,
                                                                 config.value_upper,
                                                                 config.hex);
                    if (matcher.match(bytes.subspan(offset, window)) != expected)
                    {
                        match_agrees = false;
                        break;
                    }
                }
                CHECK(match_agrees);
                CHECK(collected(bytes, matcher, window, alignment) == reference(bytes, config, window, alignment));
            }
        }
    }
}

TEST_CASE("the matcher handles uniform and floating-point needles", "[scan]")
{
    using slopkit::scan::Matcher;
    using slopkit::scan::ScanValue;

    SECTION("a uniformly zero needle keeps every aligned hit")
    {
        const std::vector<std::byte>     bytes(64);
        const ScanConfig                 config  = exact_config(ValueType::int32, 0);
        const Matcher                    matcher = Matcher::build(config);
        const std::span<const std::byte> span(bytes);
        for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4)
        {
            CHECK(matcher.find(span, offset, span.size(), 4) == offset);
        }
        CHECK(matcher.find(span, 0, span.size(), 4) == 0);
    }

    SECTION("a uniformly 0xFF needle keeps every aligned hit")
    {
        const std::vector<std::byte>     bytes(64, std::byte {0xFF});
        const ScanConfig                 config  = exact_config(ValueType::int32, -1);
        const Matcher                    matcher = Matcher::build(config);
        const std::span<const std::byte> span(bytes);
        for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4)
        {
            CHECK(matcher.find(span, offset, span.size(), 4) == offset);
        }
    }

    SECTION("-0.0 equals 0.0 and NaN never matches")
    {
        std::vector<std::byte> bytes(16);
        bytes[0] = std::byte {0x00}; // 0.0 as a zero-filled double.

        const std::span<const std::byte> span(bytes);

        ScanConfig positive;
        positive.type       = ScanType::exact_value;
        positive.value_type = ValueType::float64;
        positive.value      = ScanValue {0.0};
        CHECK(Matcher::build(positive).match(span.subspan(0, 8)));

        ScanConfig negative;
        negative.type       = ScanType::exact_value;
        negative.value_type = ValueType::float64;
        negative.value      = ScanValue {-0.0};
        CHECK(Matcher::build(negative).match(span.subspan(0, 8)));

        ScanConfig not_a_number;
        not_a_number.type         = ScanType::exact_value;
        not_a_number.value_type   = ValueType::float64;
        not_a_number.value        = ScanValue {std::numeric_limits<double>::quiet_NaN()};
        const Matcher nan_matcher = Matcher::build(not_a_number);
        CHECK_FALSE(nan_matcher.match(span.subspan(0, 8)));
        CHECK_FALSE(nan_matcher.match(span.subspan(8, 8)));
    }
}
