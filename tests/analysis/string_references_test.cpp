#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

#include "analysis/string_references.hpp"
#include "disasm/assembler.hpp"
#include "process/types.hpp"

namespace
{
    using slopkit::analysis::find_string_references;
    using slopkit::analysis::ModuleRange;
    using slopkit::analysis::ReadIntoFn;
    using slopkit::analysis::StringReference;
    using slopkit::analysis::StringReferenceConfig;
    using slopkit::disasm::AssembleBlockContext;
    using slopkit::disasm::MachineMode;
    using slopkit::process::AccessError;
    using slopkit::process::RegionInfo;

    constexpr std::uint64_t kImageBase = 0x1000;
    constexpr std::uint64_t kCodeEnd   = 0x2000;
    constexpr std::uint64_t kDataBase  = 0x3000;
    constexpr std::uint64_t kImageEnd  = 0x4000;

    [[nodiscard]] std::vector<std::byte> encoded(std::string_view text, std::uint64_t address)
    {
        const auto assembled =
            slopkit::disasm::assemble_block(text, AssembleBlockContext {.base = address, .mode = MachineMode::long_64});
        REQUIRE(assembled.has_value());
        return *assembled;
    }

    [[nodiscard]] ModuleRange module()
    {
        return ModuleRange {.name = "mod", .base = kImageBase, .end = kImageEnd};
    }

    [[nodiscard]] std::vector<RegionInfo> regions()
    {
        return {
            RegionInfo {.start      = kImageBase,
                        .end        = kCodeEnd,
                        .offset     = 0,
                        .readable   = true,
                        .writable   = false,
                        .executable = true,
                        .shared     = false,
                        .path       = "mod"},
            RegionInfo { .start      = kDataBase,
                        .end        = kImageEnd,
                        .offset     = 0,
                        .readable   = true,
                        .writable   = false,
                        .executable = false,
                        .shared     = false,
                        .path       = "mod"},
        };
    }

    struct Image
    {
        std::vector<std::byte> bytes {kImageEnd - kImageBase, std::byte {0}};

        Image()
        {
            std::fill(
                bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(kCodeEnd - kImageBase), std::byte {0x90});
        }

        void put(std::uint64_t address, std::span<const std::byte> code)
        {
            std::copy(code.begin(), code.end(), bytes.begin() + static_cast<std::ptrdiff_t>(address - kImageBase));
        }

        void put_ascii(std::uint64_t address, std::string_view text)
        {
            std::size_t offset = address - kImageBase;
            for (const char character : text)
            {
                bytes[offset++] = static_cast<std::byte>(character);
            }
            bytes[offset] = std::byte {0};
        }

        void put_wide(std::uint64_t address, std::string_view text)
        {
            std::size_t offset = address - kImageBase;
            for (const char character : text)
            {
                bytes[offset++] = static_cast<std::byte>(character);
                bytes[offset++] = std::byte {0};
            }
            bytes[offset]     = std::byte {0};
            bytes[offset + 1] = std::byte {0};
        }

        [[nodiscard]] ReadIntoFn reader() const
        {
            return [this](std::uint64_t address, std::span<std::byte> out) -> std::expected<std::size_t, AccessError>
            {
                if (address < kImageBase || address >= kImageEnd)
                {
                    return 0;
                }
                const std::size_t available = static_cast<std::size_t>(kImageEnd - address);
                const std::size_t count     = std::min(out.size(), available);
                std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(address - kImageBase), count, out.begin());
                return count;
            };
        }
    };
}

TEST_CASE("the analysis finds a direct ASCII reference", "[analysis]")
{
    Image image;
    image.put_ascii(0x3010, "hello");
    image.put(kImageBase, encoded("LEA RAX, [0x3010]\nLEA RBX, [0x3010]\nMOV RAX, [RBX]", kImageBase));

    const std::vector<StringReference> references =
        find_string_references(image.reader(), module(), regions(), MachineMode::long_64);

    REQUIRE(references.size() == 2);
    CHECK(references[0].instruction == kImageBase);
    CHECK(references[0].string_address == 0x3010);
    CHECK(references[0].text == "hello");
    CHECK_FALSE(references[0].wide);
    CHECK(references[1].instruction == kImageBase + 7);
    CHECK(references[1].string_address == 0x3010);
    CHECK(references[1].text == "hello");
}

TEST_CASE("the analysis reports a wide string", "[analysis]")
{
    Image image;
    image.put_wide(0x3020, "wide");
    image.put(kImageBase, encoded("LEA RCX, [0x3020]", kImageBase));

    const std::vector<StringReference> references =
        find_string_references(image.reader(), module(), regions(), MachineMode::long_64);

    REQUIRE(references.size() == 1);
    CHECK(references[0].instruction == kImageBase);
    CHECK(references[0].string_address == 0x3020);
    CHECK(references[0].text == "wide");
    CHECK(references[0].wide);
}

TEST_CASE("the analysis rejects short, unterminated and non-string targets", "[analysis]")
{
    Image image;
    image.put_ascii(0x3030, "abc");

    // A run of printable bytes with no terminator inside the read cap.
    for (std::uint64_t address = 0x3060; address < 0x3160; ++address)
    {
        image.bytes[address - kImageBase] = std::byte {0x61};
    }

    image.put(kImageBase,
              encoded("LEA RAX, [0x3030]\nLEA RBX, [0x3050]\nLEA RCX, [0x3060]\nLEA RDX, [0x1000]\nLEA RSI, "
                      "[0x9000]\nMOV EAX, 0x3010",
                      kImageBase));

    const std::vector<StringReference> references =
        find_string_references(image.reader(), module(), regions(), MachineMode::long_64);

    CHECK(references.empty());
}

TEST_CASE("the analysis carries an instruction across a chunk boundary", "[analysis]")
{
    Image image;
    image.put_ascii(0x3010, "hello");
    image.put(0x100E, encoded("LEA RAX, [0x3010]", 0x100E));

    const StringReferenceConfig        config {.min_length = 4, .max_string_bytes = 256, .chunk_size = 16};
    const std::vector<StringReference> references =
        find_string_references(image.reader(), module(), regions(), MachineMode::long_64, config);

    REQUIRE(references.size() == 1);
    CHECK(references[0].instruction == 0x100E);
    CHECK(references[0].string_address == 0x3010);
    CHECK(references[0].text == "hello");
}
