#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "platform/linux/module_entry.hpp"
#include "platform/linux/procfs.hpp"

namespace
{
    constexpr std::string_view kMapsFixture = R"(7f0000000000-7f0000001000 r-xp 00000000 08:01 123 /usr/lib/libc.so.6
7f0000001000-7f0000002000 r--p 00001000 08:01 123 /usr/lib/libc.so.6
7f1000000000-7f1000001000 r-xp 00000000 08:01 456 /home/user/.wine/drive_c/windows/system32/ntdll.dll
7f2000000000-7f2000001000 rw-p 00000000 00:00 0
55a000000000-55a000001000 rw-p 00000000 00:00 0 [heap]
7f3000000000-7f3000001000 r-xp 00000000 08:01 789 /tmp/libdeleted.so (deleted)
)";

    // A minimal little-endian 64-bit ELF header carrying `type` and `entry`.
    std::vector<std::byte> make_elf(std::uint16_t type, std::uint64_t entry)
    {
        std::vector<std::byte> bytes(32, std::byte {0});
        bytes[0]  = std::byte {0x7f};
        bytes[1]  = std::byte {'E'};
        bytes[2]  = std::byte {'L'};
        bytes[3]  = std::byte {'F'};
        bytes[4]  = std::byte {2}; // ELFCLASS64
        bytes[5]  = std::byte {1}; // ELFDATA2LSB
        bytes[16] = static_cast<std::byte>(type & 0xff);
        bytes[17] = static_cast<std::byte>((type >> 8) & 0xff);
        for (std::size_t i = 0; i < 8; ++i)
        {
            bytes[24 + i] = static_cast<std::byte>((entry >> (8 * i)) & 0xff);
        }
        return bytes;
    }

    // A minimal PE header whose optional header carries `entry_rva`.
    std::vector<std::byte> make_pe(std::uint32_t entry_rva)
    {
        constexpr std::size_t  lfanew = 0x40;
        std::vector<std::byte> bytes(lfanew + 44, std::byte {0});
        bytes[0]          = std::byte {'M'};
        bytes[1]          = std::byte {'Z'};
        bytes[0x3c]       = static_cast<std::byte>(lfanew & 0xff);
        bytes[0x3d]       = static_cast<std::byte>((lfanew >> 8) & 0xff);
        bytes[lfanew]     = std::byte {'P'};
        bytes[lfanew + 1] = std::byte {'E'};

        const auto rva_offset = lfanew + 24 + 16;
        for (std::size_t i = 0; i < 4; ++i)
        {
            bytes[rva_offset + i] = static_cast<std::byte>((entry_rva >> (8 * i)) & 0xff);
        }
        return bytes;
    }
} // namespace

TEST_CASE("procfs parses region maps", "[procfs]")
{
    const auto regions = slopkit::platform::parse_maps(kMapsFixture);
    REQUIRE(regions.size() == 6);

    CHECK(regions[0].start == 0x7f0000000000ULL);
    CHECK(regions[0].end == 0x7f0000001000ULL);
    CHECK(regions[0].readable);
    CHECK_FALSE(regions[0].writable);
    CHECK(regions[0].executable);
    CHECK(regions[0].offset == 0);
    CHECK(regions[0].path == "/usr/lib/libc.so.6");

    CHECK(regions[1].offset == 0x1000);
    CHECK(regions[1].readable);
    CHECK_FALSE(regions[1].executable);

    // Anonymous mapping: no backing path.
    CHECK(regions[3].path.empty());
    CHECK(regions[3].readable);
    CHECK(regions[3].writable);

    // Kernel pseudo-path is treated as anonymous.
    CHECK(regions[4].path == "[heap]");

    // A deleted file keeps its path and is flagged.
    CHECK(regions[5].deleted);
    CHECK(regions[5].path == "/tmp/libdeleted.so");
}

TEST_CASE("procfs classifies regions as ELF, PE or anonymous", "[procfs]")
{
    const auto regions = slopkit::platform::parse_maps(kMapsFixture);

    using slopkit::process::ModuleKind;
    CHECK(slopkit::platform::classify_region(regions[0]) == ModuleKind::elf);
    CHECK(slopkit::platform::classify_region(regions[2]) == ModuleKind::pe);
    CHECK(slopkit::platform::classify_region(regions[3]) == ModuleKind::anonymous);
    CHECK(slopkit::platform::classify_region(regions[4]) == ModuleKind::anonymous);
    CHECK(slopkit::platform::classify_region(regions[5]) == ModuleKind::elf);
}

TEST_CASE("procfs merges file regions into modules", "[procfs]")
{
    const auto regions = slopkit::platform::parse_maps(kMapsFixture);
    const auto modules = slopkit::platform::modules_from_maps(regions);

    REQUIRE(modules.size() == 5);

    const auto find = [&modules](std::string_view name) -> const slopkit::process::ModuleInfo*
    {
        for (const auto& module : modules)
        {
            if (module.name == name)
            {
                return &module;
            }
        }
        return nullptr;
    };

    const auto* libc = find("libc.so.6");
    REQUIRE(libc != nullptr);
    CHECK(libc->kind == slopkit::process::ModuleKind::elf);
    CHECK(libc->base == 0x7f0000000000ULL);
    CHECK(libc->size == 0x2000ULL);
    CHECK(libc->path == "/usr/lib/libc.so.6");

    const auto* ntdll = find("ntdll.dll");
    REQUIRE(ntdll != nullptr);
    CHECK(ntdll->kind == slopkit::process::ModuleKind::pe);

    const auto* deleted = find("libdeleted.so");
    REQUIRE(deleted != nullptr);
    CHECK(deleted->kind == slopkit::process::ModuleKind::elf);

    const auto* anonymous = find("[anon]");
    REQUIRE(anonymous != nullptr);
    CHECK(anonymous->kind == slopkit::process::ModuleKind::anonymous);
}

TEST_CASE("procfs parses status", "[procfs]")
{
    constexpr std::string_view status = "Name:\tbash\n"
                                        "Umask:\t0022\n"
                                        "State:\tS (sleeping)\n"
                                        "Tgid:\t1000\n"
                                        "Pid:\t1000\n"
                                        "PPid:\t900\n"
                                        "TracerPid:\t0\n"
                                        "Uid:\t1000\t1000\t1000\t1000\n"
                                        "Gid:\t1000\t1000\t1000\t1000\n";

    const auto parsed = slopkit::platform::parse_status(status);
    REQUIRE(parsed.has_value());
    CHECK(parsed->name == "bash");
    CHECK(parsed->ppid == 900);
    CHECK(parsed->uid == 1000);
    CHECK(parsed->gid == 1000);
    CHECK(parsed->tracer_pid == 0);

    CHECK_FALSE(slopkit::platform::parse_status("no fields here").has_value());
}

TEST_CASE("procfs parses a task listing", "[procfs]")
{
    const auto tids = slopkit::platform::parse_task_ids("1002\n1000\nnot-a-tid\n1001\n");

    REQUIRE(tids.size() == 3);
    CHECK(tids[0] == 1000);
    CHECK(tids[1] == 1001);
    CHECK(tids[2] == 1002);
}

TEST_CASE("module entry parses ELF and PE headers", "[procfs]")
{
    const auto dynamic = slopkit::platform::parse_image_entry(make_elf(3, 0x1234));
    REQUIRE(dynamic.has_value());
    CHECK(dynamic->address == 0x1234);
    CHECK(dynamic->relative);

    const auto executable = slopkit::platform::parse_image_entry(make_elf(2, 0x401000));
    REQUIRE(executable.has_value());
    CHECK(executable->address == 0x401000);
    CHECK_FALSE(executable->relative);

    const auto portable = slopkit::platform::parse_image_entry(make_pe(0x1000));
    REQUIRE(portable.has_value());
    CHECK(portable->address == 0x1000);
    CHECK(portable->relative);

    const std::vector<std::byte> garbage {std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}};
    CHECK_FALSE(slopkit::platform::parse_image_entry(garbage).has_value());

    const std::vector<std::byte> truncated {std::byte {0x7f}, std::byte {'E'}, std::byte {'L'}, std::byte {'F'}};
    CHECK_FALSE(slopkit::platform::parse_image_entry(truncated).has_value());
}

TEST_CASE("module entry filling skips modules it cannot read", "[procfs]")
{
    using slopkit::process::ModuleInfo;
    using slopkit::process::ModuleKind;

    std::vector<ModuleInfo> modules;

    ModuleInfo anonymous;
    anonymous.kind = ModuleKind::anonymous;
    modules.push_back(anonymous);

    ModuleInfo no_path;
    no_path.kind = ModuleKind::elf;
    modules.push_back(no_path);

    ModuleInfo relative;
    relative.kind = ModuleKind::elf;
    relative.path = "relative/libfoo.so";
    modules.push_back(relative);

    ModuleInfo not_at_start;
    not_at_start.kind   = ModuleKind::elf;
    not_at_start.path   = "/nonexistent/libbar.so";
    not_at_start.offset = 0x1000;
    modules.push_back(not_at_start);

    ModuleInfo missing;
    missing.kind = ModuleKind::elf;
    missing.path = "/definitely/not/here/libbaz.so";
    modules.push_back(missing);

    slopkit::platform::fill_module_entry_points(0x7fffffffu, modules);

    for (const auto& module : modules)
    {
        CHECK(module.entry == 0);
    }
}
