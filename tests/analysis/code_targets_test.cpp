#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "analysis/code_targets.hpp"
#include "disasm/assembler.hpp"
#include "process/types.hpp"

namespace
{
    using slopkit::analysis::CodeTarget;
    using slopkit::analysis::CodeTargetKind;
    using slopkit::analysis::CodeTargetKinds;
    using slopkit::analysis::CodeTargetScanConfig;
    using slopkit::analysis::CodeTargetScanResult;
    using slopkit::analysis::find_code_targets;
    using slopkit::analysis::ModuleRange;
    using slopkit::analysis::ReadIntoFn;
    using slopkit::analysis::RegionReason;
    using slopkit::disasm::AssembleBlockContext;
    using slopkit::disasm::MachineMode;
    using slopkit::process::AccessError;
    using slopkit::process::RegionInfo;

    constexpr std::uint64_t kImageBase = 0x1000;
    constexpr std::uint64_t kCodeEnd   = 0x2000;
    constexpr std::uint64_t kDataBase  = 0x3000;
    constexpr std::uint64_t kImageEnd  = 0x4000;

    [[nodiscard]] std::vector<std::byte> raw(std::initializer_list<unsigned> values)
    {
        std::vector<std::byte> result;
        result.reserve(values.size());
        for (const unsigned value : values)
        {
            result.push_back(static_cast<std::byte>(value));
        }
        return result;
    }

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

    [[nodiscard]] bool has_kind(const CodeTargetScanResult& scan, CodeTargetKind kind)
    {
        return std::ranges::any_of(scan.targets,
                                   [kind](const CodeTarget& target)
                                   {
                                       return target.kind == kind;
                                   });
    }
}

TEST_CASE("the sweep collects only call targets by default", "[analysis]")
{
    Image image;
    image.put(kImageBase, encoded("CALL 0x1100\nJMP 0x1200\nJZ 0x1300\nCALL 0x1100\nCALL 0x9000", kImageBase));

    const CodeTargetScanResult scan =
        find_code_targets(image.reader(), module(), regions(), MachineMode::long_64, CodeTargetKinds {});

    REQUIRE(scan.targets.size() == 1);
    CHECK(scan.targets[0].instruction == kImageBase);
    CHECK(scan.targets[0].target == 0x1100);
    CHECK(scan.targets[0].kind == CodeTargetKind::call);

    // The jump kinds are off, the repeated call is folded into one candidate,
    // and the call out of the module is only counted.
    CHECK(scan.report.branches == 3);
    CHECK(scan.report.outside_module == 1);
    CHECK(scan.report.targets == 1);
    CHECK(scan.report.regions_in_module == 2);
    CHECK(scan.report.code_regions == 1);
    CHECK(scan.report.bytes_read == kCodeEnd - kImageBase);
    CHECK(scan.report.instructions > 0);
    REQUIRE(scan.report.excluded.size() == 1);
    CHECK(scan.report.excluded[0].reason == RegionReason::not_executable);
}

TEST_CASE("the sweep collects jumps and conditional jumps when they are enabled", "[analysis]")
{
    Image image;
    image.put(kImageBase, encoded("CALL 0x1100\nJMP 0x1200\nJZ 0x1300", kImageBase));

    const CodeTargetScanResult scan =
        find_code_targets(image.reader(),
                          module(),
                          regions(),
                          MachineMode::long_64,
                          CodeTargetKinds {.call = true, .jump = true, .conditional_jump = true});

    REQUIRE(scan.targets.size() == 3);
    CHECK(scan.targets[0].target == 0x1100);
    CHECK(scan.targets[0].kind == CodeTargetKind::call);
    CHECK(scan.targets[1].target == 0x1200);
    CHECK(scan.targets[1].kind == CodeTargetKind::jump);
    CHECK(scan.targets[2].target == 0x1300);
    CHECK(scan.targets[2].kind == CodeTargetKind::conditional_jump);
    CHECK(scan.targets[0].instruction < scan.targets[1].instruction);
    CHECK(scan.targets[1].instruction < scan.targets[2].instruction);
    CHECK(scan.report.targets == 3);

    CHECK(has_kind(scan, CodeTargetKind::jump));
    CHECK(has_kind(scan, CodeTargetKind::conditional_jump));
}

TEST_CASE("the sweep never collects an indirect branch", "[analysis]")
{
    Image image;
    // CALL [RIP+0xA] and JMP RAX: no directly encoded target.
    image.put(kImageBase, raw({0xFF, 0x15, 0x0A, 0x00, 0x00, 0x00, 0xFF, 0xE0}));

    const CodeTargetScanResult scan =
        find_code_targets(image.reader(),
                          module(),
                          regions(),
                          MachineMode::long_64,
                          CodeTargetKinds {.call = true, .jump = true, .conditional_jump = true});

    CHECK(scan.targets.empty());
    CHECK(scan.report.branches == 2);
    CHECK(scan.report.indirect_branches == 2);
    CHECK(scan.report.targets == 0);
}

TEST_CASE("the sweep carries an instruction across a chunk boundary", "[analysis]")
{
    Image image;
    image.put(0x100E, encoded("CALL 0x1100", 0x100E));

    const CodeTargetScanConfig config {.chunk_size = 16};
    const CodeTargetScanResult scan =
        find_code_targets(image.reader(), module(), regions(), MachineMode::long_64, CodeTargetKinds {}, config);

    REQUIRE(scan.targets.size() == 1);
    CHECK(scan.targets[0].instruction == 0x100E);
    CHECK(scan.targets[0].target == 0x1100);
}

TEST_CASE("an empty module reports why nothing was swept", "[analysis]")
{
    Image image;

    const ModuleRange          empty {.name = "mod", .base = kImageBase, .end = kImageBase};
    const CodeTargetScanResult scan =
        find_code_targets(image.reader(), empty, regions(), MachineMode::long_64, CodeTargetKinds {});

    CHECK(scan.targets.empty());
    REQUIRE(scan.report.excluded.size() == 1);
    CHECK(scan.report.excluded[0].reason == RegionReason::empty_module);
}

TEST_CASE("describe names the funnel and the skipped reasons", "[analysis]")
{
    Image image;
    image.put(kImageBase, encoded("CALL 0x1100\nJMP 0x1200", kImageBase));

    const CodeTargetScanResult scan =
        find_code_targets(image.reader(), module(), regions(), MachineMode::long_64, CodeTargetKinds {});
    const std::string text = slopkit::analysis::describe(scan.report);

    CHECK(text.find("1 target(s)") != std::string::npos);
    CHECK(text.find("not executable") != std::string::npos);
    CHECK(text.find(std::to_string(scan.report.bytes_read)) != std::string::npos);
    CHECK(text.find(std::to_string(scan.report.instructions)) != std::string::npos);
}
