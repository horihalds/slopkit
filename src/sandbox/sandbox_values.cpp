#include "sandbox/sandbox_values.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::sandbox
{
    namespace
    {
        // The process-wide store. Namespace scope gives it static storage
        // duration in the main image, so every field keeps a stable `.data`
        // address a first scan can hit.
        Values g_values;

        // The ASCII marker and the trailing magic word the heap buffer carries.
        constexpr std::string_view kHeapMarker = "SLOPKIT-SANDBOX-MARKER";
        constexpr std::uint64_t    kHeapMagic  = 0x51F2C0DE1BADBEEFULL;

        std::vector<std::byte> make_heap_marker()
        {
            std::vector<std::byte> buffer;
            buffer.reserve(kHeapMarker.size() + sizeof(kHeapMagic));
            for (const char character : kHeapMarker)
            {
                buffer.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
            }
            for (std::size_t index = 0; index < sizeof(kHeapMagic); ++index)
            {
                buffer.push_back(static_cast<std::byte>((kHeapMagic >> (8 * index)) & 0xFF));
            }
            return buffer;
        }

        std::string format_bytes(std::span<const std::byte> bytes)
        {
            std::string result;
            for (std::size_t index = 0; index < bytes.size(); ++index)
            {
                if (index != 0)
                {
                    result += ' ';
                }
                result += std::format("{:02X}", std::to_integer<std::uint8_t>(bytes[index]));
            }
            return result;
        }

        std::string banner_string(const std::array<char, 32>& banner)
        {
            const auto end = std::ranges::find(banner, '\0');
            return std::string(banner.begin(), end);
        }

        // One `name address=... size=N value=...` line per field. The
        // labelled shape keeps the headless output easy to parse.
        template<typename T>
        void print_field(std::ostream& out, std::string_view name, const T& field, std::string value)
        {
            out << std::format("{} address={:016X} size={} value={}\n",
                               name,
                               reinterpret_cast<std::uintptr_t>(&field),
                               sizeof(T),
                               value);
        }
    } // namespace

    Values& values()
    {
        return g_values;
    }

    std::span<std::byte> heap_marker()
    {
        // Function-local static: the vector owns heap storage, and the buffer
        // exists for the life of the process.
        static std::vector<std::byte> buffer = make_heap_marker();
        return buffer;
    }

    void reset()
    {
        g_values = Values {};
    }

    void advance()
    {
        g_values.health = g_values.health > kHealthFloor ? g_values.health - 1 : kHealthStart;
        g_values.score += kScoreStep;
        g_values.drift += kDriftStep;
    }

    void print_layout(std::ostream& out)
    {
        const Values& state = g_values;

        print_field(out, "byte_value", state.byte_value, std::format("{:02X}", state.byte_value));
        print_field(out, "small_value", state.small_value, std::format("{}", state.small_value));
        print_field(out, "health", state.health, std::format("{}", state.health));
        print_field(out, "score", state.score, std::format("{}", state.score));
        print_field(out, "speed", state.speed, std::format("{:.3f}", state.speed));
        print_field(out, "precision", state.precision, std::format("{:.6f}", state.precision));
        print_field(out, "banner", state.banner, banner_string(state.banner));
        print_field(
            out,
            "pattern",
            state.pattern,
            format_bytes(std::span(reinterpret_cast<const std::byte*>(state.pattern.data()), state.pattern.size())));
        print_field(out, "drift", state.drift, std::format("{:.3f}", state.drift));

        const std::span<std::byte> marker = heap_marker();
        out << std::format("heap_marker address={:016X} size={} value={}\n",
                           reinterpret_cast<std::uintptr_t>(marker.data()),
                           marker.size(),
                           format_bytes(marker));
    }

} // namespace slopkit::sandbox
