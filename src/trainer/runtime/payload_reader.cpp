#include "trainer/runtime/payload_reader.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "trainer/runtime/target.hpp"

namespace slopkit::trainer
{
    std::expected<Payload, std::string> load_own_payload()
    {
        const auto path = own_executable_path();
        if (path.empty())
        {
            return std::unexpected(std::string {"could not locate this trainer's own executable"});
        }

        std::ifstream stream {path, std::ios::binary};
        if (!stream)
        {
            return std::unexpected(std::format("could not read {}", path.string()));
        }

        const std::vector<char> text {std::istreambuf_iterator<char> {stream}, std::istreambuf_iterator<char> {}};
        const auto*             bytes = reinterpret_cast<const std::byte*>(text.data());
        return decode_payload(std::span {bytes, text.size()});
    }

}
