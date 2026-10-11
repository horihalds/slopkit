#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string_view>

namespace slopkit::sandbox
{
    [[nodiscard]] constexpr std::array<char, 32> banner_default() noexcept
    {
        std::array<char, 32>       text {};
        constexpr std::string_view source = "SLOPKIT-PRACTICE-TARGET";
        for (std::size_t index = 0; index < source.size(); ++index)
        {
            text[index] = source[index];
        }
        return text;
    }

    inline constexpr std::int32_t kHealthStart {100};
    inline constexpr std::int32_t kHealthFloor {1};
    inline constexpr std::int64_t kScoreStart {1000};
    inline constexpr std::int64_t kScoreStep {1};
    inline constexpr float        kDriftStep {0.25F};

    struct Values
    {
        std::uint8_t                byte_value {0xA5};
        std::int16_t                small_value {-1234};
        std::int32_t                health {kHealthStart};
        std::int64_t                score {kScoreStart};
        float                       speed {1.5F};
        double                      precision {3.14159265};
        std::array<char, 32>        banner {banner_default()};
        std::array<std::uint8_t, 8> pattern {0xDE, 0xAD, 0xBE, 0xEF, 0x51, 0xF2, 0xC0, 0xDE};
        float                       drift {0.0F};
    };

    [[nodiscard]] Values& values();

    [[nodiscard]] std::span<std::byte> heap_marker();

    void reset();

    void advance();

    void print_layout(std::ostream& out);

}
