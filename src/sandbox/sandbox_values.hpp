#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string_view>

namespace slopkit::sandbox
{

    // The initial text of the `banner` field, NUL-padded to the array's width.
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

    // The animated fields' loop values: `health` falls to its floor and then
    // wraps back to its start, `score` climbs by a fixed step and `drift`
    // advances by a fixed step.
    inline constexpr std::int32_t kHealthStart {100};
    inline constexpr std::int32_t kHealthFloor {1};
    inline constexpr std::int64_t kScoreStart {1000};
    inline constexpr std::int64_t kScoreStep {1};
    inline constexpr float        kDriftStep {0.25F};

    // One field per slopkit value type, plus a second animated float. Every
    // field is initialised, so the whole object lands in the writable `.data`
    // region the scanner's default region filter includes.
    struct Values
    {
        std::uint8_t                byte_value {0xA5};                                        // Byte
        std::int16_t                small_value {-1234};                                      // 2 Bytes
        std::int32_t                health {kHealthStart};                                    // 4 Bytes, animated down
        std::int64_t                score {kScoreStart};                                      // 8 Bytes, animated up
        float                       speed {1.5F};                                             // Float
        double                      precision {3.14159265};                                   // Double
        std::array<char, 32>        banner {banner_default()};                                // String, NUL-padded
        std::array<std::uint8_t, 8> pattern {0xDE, 0xAD, 0xBE, 0xEF, 0x51, 0xF2, 0xC0, 0xDE}; // Array of byte
        float                       drift {0.0F}; // Float, animated by a small step
    };

    // The process-wide store: static storage duration, so every field keeps a
    // stable address in the image for the life of the process.
    [[nodiscard]] Values& values();

    // A heap buffer holding an ASCII marker and a magic word: dynamic hits and
    // a region worth opening in the Memory Viewer.
    [[nodiscard]] std::span<std::byte> heap_marker();

    // Restores every initial value.
    void reset();

    // One in-place animation step: `health` down with a wrap to its start,
    // `score` up and `drift` moved by a small fixed step; everything else is
    // left untouched.
    void advance();

    // Prints each field's name, address, size and current value, one per line,
    // so the layout is verifiable with no display.
    void print_layout(std::ostream& out);

} // namespace slopkit::sandbox
