#include <catch2/catch.hpp>

#include <string_view>
#include <vector>

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
