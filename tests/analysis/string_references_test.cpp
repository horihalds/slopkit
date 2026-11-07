#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>
#include <vector>

#include "analysis/string_references.hpp"
#include "disasm/assembler.hpp"
#include "process/types.hpp"

namespace
{
    using slopkit::analysis::find_string_references;
    using slopkit::analysis::ModuleRange;
    using slopkit::analysis::ReadIntoFn;
    using slopkit::analysis::RegionReason;
    using slopkit::analysis::StringReference;
    using slopkit::analysis::StringReferenceConfig;
    using slopkit::analysis::StringScanResult;
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
        std::vector<std::byte>                               bytes {kImageEnd - kImageBase, std::byte {0}};
        std::vector<std::pair<std::uint64_t, std::uint64_t>> unreadable;
        bool                                                 strict_reads {false};

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
                if (strict_reads && out.size() > available)
                {
                    return std::unexpected(AccessError::io_error);
                }
                for (const auto& [start, end] : unreadable)
                {
                    if (address < end && start < address + out.size())
                    {
                        return std::unexpected(AccessError::io_error);
                    }
                }
                const std::size_t count = std::min(out.size(), available);
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

    const StringScanResult scan = find_string_references(image.reader(), module(), regions(), MachineMode::long_64);

    const std::vector<StringReference>& references = scan.references;
    REQUIRE(references.size() == 2);
    CHECK(references[0].instruction == kImageBase);
    CHECK(references[0].string_address == 0x3010);
    CHECK(references[0].text == "hello");
    CHECK_FALSE(references[0].wide);
    CHECK(references[1].instruction == kImageBase + 7);
    CHECK(references[1].string_address == 0x3010);
    CHECK(references[1].text == "hello");

    // The report names the funnel: both regions were considered, only the
    // executable one was swept, and the data region was dropped as such.
    CHECK(scan.report.regions_in_module == 2);
    CHECK(scan.report.code_regions == 1);
    CHECK(scan.report.bytes_read == kCodeEnd - kImageBase);
    CHECK(scan.report.instructions > 0);
    CHECK(scan.report.address_references > 0);
    CHECK(scan.report.strings == 2);
    REQUIRE(scan.report.excluded.size() == 1);
    CHECK(scan.report.excluded[0].reason == RegionReason::not_executable);
}

TEST_CASE("the analysis reports a wide string", "[analysis]")
{
    Image image;
    image.put_wide(0x3020, "wide");
    image.put(kImageBase, encoded("LEA RCX, [0x3020]", kImageBase));

    const StringScanResult scan = find_string_references(image.reader(), module(), regions(), MachineMode::long_64);

    REQUIRE(scan.references.size() == 1);
    CHECK(scan.references[0].instruction == kImageBase);
    CHECK(scan.references[0].string_address == 0x3020);
    CHECK(scan.references[0].text == "wide");
    CHECK(scan.references[0].wide);
    CHECK(scan.report.strings == 1);
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

    const StringScanResult scan = find_string_references(image.reader(), module(), regions(), MachineMode::long_64);

    CHECK(scan.references.empty());
    CHECK(scan.report.strings == 0);
    CHECK(scan.report.instructions > 0);
}

TEST_CASE("the analysis carries an instruction across a chunk boundary", "[analysis]")
{
    Image image;
    image.put_ascii(0x3010, "hello");
    image.put(0x100E, encoded("LEA RAX, [0x3010]", 0x100E));

    const StringReferenceConfig config {.min_length = 4, .max_string_bytes = 256, .chunk_size = 16};
    const StringScanResult      scan =
        find_string_references(image.reader(), module(), regions(), MachineMode::long_64, config);

    REQUIRE(scan.references.size() == 1);
    CHECK(scan.references[0].instruction == 0x100E);
    CHECK(scan.references[0].string_address == 0x3010);
    CHECK(scan.references[0].text == "hello");
    CHECK(scan.report.bytes_read == kCodeEnd - kImageBase);
}

TEST_CASE("an executable-only module is reported without strings", "[analysis]")
{
    Image image;
    image.put_ascii(0x3010, "hello");
    image.put(kImageBase, encoded("LEA RAX, [0x3010]", kImageBase));

    const std::vector<RegionInfo> code_only {regions()[0]};
    const StringScanResult scan = find_string_references(image.reader(), module(), code_only, MachineMode::long_64);

    CHECK(scan.references.empty());
    CHECK(scan.report.regions_in_module == 1);
    CHECK(scan.report.code_regions == 1);
    CHECK(scan.report.instructions > 0);
    CHECK(scan.report.strings == 0);
    CHECK(scan.report.excluded.empty());
}

TEST_CASE("a zero-size module is reported as empty", "[analysis]")
{
    Image image;
    image.put_ascii(0x3010, "hello");

    const ModuleRange      empty {.name = "mod", .base = kImageBase, .end = kImageBase};
    const StringScanResult scan = find_string_references(image.reader(), empty, regions(), MachineMode::long_64);

    CHECK(scan.references.empty());
    CHECK(scan.report.regions_in_module == 0);
    CHECK(scan.report.code_regions == 0);
    CHECK(scan.report.bytes_read == 0);
    REQUIRE(scan.report.excluded.size() == 1);
    CHECK(scan.report.excluded[0].reason == RegionReason::empty_module);
}

TEST_CASE("a module whose regions all fall outside it is reported without work", "[analysis]")
{
    Image image;
    image.put_ascii(0x3010, "hello");

    const ModuleRange      nowhere {.name = "mod", .base = 0x8000, .end = 0x9000};
    const StringScanResult scan = find_string_references(image.reader(), nowhere, regions(), MachineMode::long_64);

    CHECK(scan.references.empty());
    CHECK(scan.report.regions_in_module == 0);
    CHECK(scan.report.code_regions == 0);
    CHECK(scan.report.instructions == 0);
    CHECK(scan.report.excluded.empty());
}

TEST_CASE("a code region past the module end is swept only up to the end", "[analysis]")
{
    Image image;
    image.put_ascii(0x1010, "hello");
    image.put(0x1800, encoded("LEA RAX, [0x1010]", 0x1800));
    // Past the module end, where it must not be decoded.
    image.put(0x2100, encoded("LEA RBX, [0x1010]", 0x2100));

    const ModuleRange             narrow {.name = "mod", .base = kImageBase, .end = kCodeEnd};
    const std::vector<RegionInfo> narrow_regions {
        RegionInfo {    .start      = 0x1800,
                    .end        = 0x3000,
                    .offset     = 0,
                    .readable   = true,
                    .writable   = false,
                    .executable = true,
                    .shared     = false,
                    .path       = "mod"},
        RegionInfo {.start      = kImageBase,
                    .end        = 0x1800,
                    .offset     = 0,
                    .readable   = true,
                    .writable   = false,
                    .executable = false,
                    .shared     = false,
                    .path       = "mod"},
    };
    const StringScanResult scan = find_string_references(image.reader(), narrow, narrow_regions, MachineMode::long_64);

    REQUIRE(scan.references.size() == 1);
    CHECK(scan.references[0].instruction == 0x1800);
    CHECK(scan.references[0].string_address == 0x1010);
    CHECK(scan.references[0].text == "hello");
    CHECK(scan.report.code_regions == 1);
    CHECK(scan.report.bytes_read == kCodeEnd - 0x1800);
}

TEST_CASE("a module whose code region carries no file path is still swept", "[analysis]")
{
    Image image;
    image.put_ascii(0x3010, "hello");
    image.put(kImageBase, encoded("LEA RAX, [0x3010]", kImageBase));

    std::vector<RegionInfo> unbacked = regions();
    unbacked[0].path.clear();
    unbacked[1].path.clear();

    const StringScanResult scan = find_string_references(image.reader(), module(), unbacked, MachineMode::long_64);

    REQUIRE(scan.references.size() == 1);
    CHECK(scan.references[0].instruction == kImageBase);
    CHECK(scan.references[0].string_address == 0x3010);
    CHECK(scan.references[0].text == "hello");
    CHECK(scan.report.code_regions == 1);
}

TEST_CASE("a string inside an executable region is found", "[analysis]")
{
    Image image;
    image.put_ascii(0x1080, "hello");
    image.put(kImageBase, encoded("LEA RAX, [0x1080]", kImageBase));

    const ModuleRange             executable_module {.name = "mod", .base = kImageBase, .end = 0x1100};
    const std::vector<RegionInfo> executable_regions {
        RegionInfo {.start      = kImageBase,
                    .end        = 0x1100,
                    .offset     = 0,
                    .readable   = true,
                    .writable   = false,
                    .executable = true,
                    .shared     = false,
                    .path       = "mod"},
    };
    const StringScanResult scan =
        find_string_references(image.reader(), executable_module, executable_regions, MachineMode::long_64);

    REQUIRE(scan.references.size() == 1);
    CHECK(scan.references[0].instruction == kImageBase);
    CHECK(scan.references[0].string_address == 0x1080);
    CHECK(scan.references[0].text == "hello");
    CHECK(scan.report.strings == 1);
}

TEST_CASE("a readable target outside the module is rejected", "[analysis]")
{
    Image image;
    image.put_ascii(0x3010, "hello");
    image.put(kImageBase, encoded("LEA RAX, [0x3010]", kImageBase));

    const ModuleRange      narrow {.name = "mod", .base = kImageBase, .end = kCodeEnd};
    const StringScanResult scan = find_string_references(image.reader(), narrow, regions(), MachineMode::long_64);

    CHECK(scan.references.empty());
    CHECK(scan.report.code_regions == 1);
    CHECK(scan.report.strings == 0);
}

TEST_CASE("a string ending at the region boundary is classified", "[analysis]")
{
    Image image;
    image.strict_reads = true;
    // The terminator is the region's last byte, so only a read bounded by the
    // region end succeeds; the global cap would run off the mapping.
    image.put_ascii(kImageEnd - 6, "hello");
    image.put(kImageBase, encoded("LEA RAX, [0x3FFA]", kImageBase));

    const StringScanResult scan = find_string_references(image.reader(), module(), regions(), MachineMode::long_64);

    REQUIRE(scan.references.size() == 1);
    CHECK(scan.references[0].instruction == kImageBase);
    CHECK(scan.references[0].string_address == kImageEnd - 6);
    CHECK(scan.references[0].text == "hello");
    CHECK(scan.report.read_failures == 0);
}

TEST_CASE("an unreadable chunk does not erase a region's other references", "[analysis]")
{
    Image image;
    image.put_ascii(0x3010, "hello");
    image.put_ascii(0x3020, "world");
    image.put(kImageBase, encoded("LEA RAX, [0x3010]", kImageBase));
    image.put(0x1200, encoded("LEA RBX, [0x3020]", 0x1200));
    image.unreadable = {
        {0x1100, 0x1120}
    };

    const StringReferenceConfig config {.min_length = 4, .max_string_bytes = 256, .chunk_size = 32};
    const StringScanResult      scan =
        find_string_references(image.reader(), module(), regions(), MachineMode::long_64, config);

    REQUIRE(scan.references.size() == 2);
    CHECK(scan.references[0].instruction == kImageBase);
    CHECK(scan.references[0].text == "hello");
    CHECK(scan.references[1].instruction == 0x1200);
    CHECK(scan.references[1].text == "world");
    CHECK(scan.report.read_failures == 1);
}

TEST_CASE("a zero-length chunk does not stall the sweep", "[analysis]")
{
    Image image;
    image.put_ascii(0x3010, "hello");
    image.put(kImageBase, encoded("LEA RAX, [0x3010]", kImageBase));

    const StringReferenceConfig config {.min_length = 4, .max_string_bytes = 256, .chunk_size = 0};
    const StringScanResult      scan =
        find_string_references(image.reader(), module(), regions(), MachineMode::long_64, config);

    CHECK(scan.references.empty());
    CHECK(scan.report.code_regions == 1);
    CHECK(scan.report.bytes_read == 0);
    CHECK(scan.report.read_failures == 0);
}

TEST_CASE("the sweep decodes a 32-bit module's absolute operand", "[analysis]")
{
    Image image;
    image.put_ascii(0x3010, "hello");

    // An absolute disp32 operand: only the 32-bit decode resolves it to 0x3010
    // instead of a RIP-relative target.
    const auto assembled = slopkit::disasm::assemble_block(
        "LEA EAX, [0x3010]", AssembleBlockContext {.base = kImageBase, .mode = MachineMode::legacy_32});
    REQUIRE(assembled.has_value());
    image.put(kImageBase, *assembled);

    const StringScanResult scan = find_string_references(image.reader(), module(), regions(), MachineMode::legacy_32);

    REQUIRE(scan.references.size() == 1);
    CHECK(scan.references[0].instruction == kImageBase);
    CHECK(scan.references[0].string_address == 0x3010);
    CHECK(scan.references[0].text == "hello");
}
