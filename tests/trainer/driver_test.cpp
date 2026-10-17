#include <catch2/catch.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "support/wine_helpers.hpp"
#include "table/address_table.hpp"
#include "trainer/generate.hpp"
#include "trainer/payload.hpp"

namespace
{
    using slopkit::scan::ValueType;
    using slopkit::table::AddressEntry;
    using slopkit::table::AddressTable;
    using slopkit::table::EntryKind;
    using slopkit::trainer::Request;

    constexpr std::int32_t kInitial {100};
    constexpr std::int32_t kFrozen {250};

    std::filesystem::path trainer_binary()
    {
        return std::filesystem::path {SLOPKIT_TRAINER_DIR} / "slopkit-trainer";
    }

    std::filesystem::path windows_trainer_binary()
    {
        return std::filesystem::path {SLOPKIT_TRAINER_DIR} / "slopkit-trainer.exe";
    }

    // A real target process that owns one writable int32 and rewrites it the way a
    // game does: `rewrite_ms` of furious overwriting, then it holds still, so a
    // frozen trainer's writes can be verified without racing the child.
    struct ChildTarget
    {
        pid_t         pid {-1};
        std::uint64_t address {0};

        ChildTarget() = default;

        ChildTarget(const ChildTarget&)            = delete;
        ChildTarget& operator=(const ChildTarget&) = delete;

        ChildTarget(ChildTarget&& other) noexcept : pid {other.pid}, address {other.address}
        {
            other.pid = -1;
        }

        ~ChildTarget()
        {
            if (pid > 0)
            {
                static_cast<void>(::kill(pid, SIGKILL));
                static_cast<void>(::waitpid(pid, nullptr, 0));
            }
        }

        [[nodiscard]] std::int32_t read() const
        {
            const int descriptor = ::open(("/proc/" + std::to_string(pid) + "/mem").c_str(), O_RDONLY);
            REQUIRE(descriptor >= 0);
            std::int32_t value = 0;
            const auto   count = ::pread(descriptor, &value, sizeof(value), static_cast<off_t>(address));
            ::close(descriptor);
            REQUIRE(count == static_cast<ssize_t>(sizeof(value)));
            return value;
        }
    };

    ChildTarget spawn_target(std::uint32_t rewrite_ms, std::string_view process_name = {})
    {
        const std::string comm {process_name};

        int fds[2] {0, 0};
        REQUIRE(::pipe(fds) == 0);

        const pid_t child = ::fork();
        REQUIRE(child >= 0);

        if (child == 0)
        {
            ::close(fds[0]);
            if (!comm.empty())
            {
                static_cast<void>(::prctl(PR_SET_NAME, comm.c_str()));
            }
            // The trainer is a sibling of this child, not its ancestor, so Yama's
            // ptrace scope would otherwise refuse the access it needs.
            static_cast<void>(::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY));
            std::int32_t value   = kInitial;
            const auto   address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&value));
            static_cast<void>(::write(fds[1], &address, sizeof(address)));
            ::close(fds[1]);

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds {rewrite_ms};
            while (std::chrono::steady_clock::now() < deadline)
            {
                value = kInitial;
                std::this_thread::sleep_for(std::chrono::milliseconds {5});
            }

            // Hold the value so the parent can inspect it after the trainer exited.
            while (true)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds {50});
            }
        }

        ::close(fds[1]);
        std::uint64_t address = 0;
        const auto    count   = ::read(fds[0], &address, sizeof(address));
        ::close(fds[0]);
        REQUIRE(count == static_cast<ssize_t>(sizeof(address)));

        ChildTarget target;
        target.pid     = child;
        target.address = address;
        return target;
    }

    // Reaps a guest a case started; killing the wine process it was launched
    // through takes the guest down with it.
    struct GuestGuard
    {
        pid_t pid {-1};

        explicit GuestGuard(pid_t child) : pid {child} {}

        GuestGuard(const GuestGuard&)            = delete;
        GuestGuard& operator=(const GuestGuard&) = delete;

        ~GuestGuard()
        {
            if (pid > 0)
            {
                static_cast<void>(::kill(pid, SIGKILL));
                static_cast<void>(::waitpid(pid, nullptr, 0));
            }
        }
    };

    struct RunResult
    {
        int         exit_code {0};
        std::string output;
    };

    RunResult run_command(const std::vector<std::string>& words)
    {
        std::string command;
        for (const auto& word : words)
        {
            command += command.empty() ? word : " " + word;
        }
        command += " 2>&1";

        FILE* pipe = ::popen(command.c_str(), "r");
        REQUIRE(pipe != nullptr);

        std::string           output;
        std::array<char, 256> buffer {};
        while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        {
            output += buffer.data();
        }

        const int status = ::pclose(pipe);
        REQUIRE(status >= 0);
        return RunResult {WEXITSTATUS(status), output};
    }

    RunResult run_trainer(const std::filesystem::path& trainer, const std::vector<std::string>& arguments)
    {
        std::vector<std::string> words {trainer.string()};
        words.insert(words.end(), arguments.begin(), arguments.end());
        return run_command(words);
    }

    RunResult run_windows_trainer(const std::filesystem::path&    wine,
                                  const std::filesystem::path&    trainer,
                                  const std::vector<std::string>& arguments)
    {
        std::vector<std::string> words {wine.string(), trainer.string()};
        words.insert(words.end(), arguments.begin(), arguments.end());
        return run_command(words);
    }

    AddressEntry value_entry(std::string description, std::string expression)
    {
        AddressEntry entry;
        entry.kind        = EntryKind::value;
        entry.description = std::move(description);
        entry.expression  = std::move(expression);
        entry.type        = ValueType::int32;
        entry.bytes       = std::vector<std::byte>(4, std::byte {0});
        return entry;
    }

    // Addresses are written the way the UI writes them: `parse_literal` reads a
    // bare token as hex, so the `0x` prefix keeps the two in step.
    std::string literal_address(std::uint64_t address)
    {
        return std::format("0x{:x}", address);
    }

    void generate_trainer(const std::vector<AddressEntry>& entries,
                          std::string_view                 name,
                          std::string_view                 script         = {},
                          std::string_view                 process_name   = {},
                          bool                             windows_target = false)
    {
        const std::filesystem::path directory = std::filesystem::path {SLOPKIT_TMP_DIR} / "trainer_driver" / name;
        std::error_code             error;
        std::filesystem::remove_all(directory, error);
        std::filesystem::create_directories(directory, error);
        REQUIRE_FALSE(error);

        AddressTable table;
        for (const auto& entry : entries)
        {
            table.add(entry);
        }
        if (!script.empty())
        {
            static_cast<void>(table.add_script("Sneaky", std::string {script}));
        }

        Request request;
        request.table            = &table;
        request.pointer_size     = sizeof(void*);
        request.linux_target     = !windows_target;
        request.windows_target   = windows_target;
        request.output_directory = directory;
        request.base_name        = "practice";
        request.process_name     = std::string {process_name};

        const auto result = slopkit::trainer::generate(request);
        REQUIRE(result.has_value());
        REQUIRE(result->errors.empty());
        REQUIRE(result->written.size() == 1);
        REQUIRE(std::filesystem::is_regular_file(windows_target ? windows_trainer_binary() : trainer_binary()));
    }

    std::filesystem::path generated_trainer(std::string_view name)
    {
        return std::filesystem::path {SLOPKIT_TMP_DIR} / "trainer_driver" / name / "practice.trainer";
    }

    std::filesystem::path generated_windows_trainer(std::string_view name)
    {
        return std::filesystem::path {SLOPKIT_TMP_DIR} / "trainer_driver" / name / "practice.trainer.exe";
    }

    std::string read_file(const std::filesystem::path& path)
    {
        std::ifstream stream {path, std::ios::binary};
        REQUIRE(stream.good());
        return std::string {std::istreambuf_iterator<char> {stream}, std::istreambuf_iterator<char> {}};
    }
}

TEST_CASE("the trainer lists a value of a running process", "[trainer][driver]")
{
    if (!std::filesystem::is_regular_file(trainer_binary()))
    {
        WARN("the trainer template was not built; the driver case is skipped");
        return;
    }

    const auto target = spawn_target(0);
    generate_trainer({value_entry("Health", literal_address(target.address))}, "list");

    const auto result = run_trainer(generated_trainer("list"), {"--pid", std::to_string(target.pid), "--list"});
    CHECK(result.exit_code == 0);
    CHECK(result.output.find("Health") != std::string::npos);
    CHECK(result.output.find("4 Bytes    100") != std::string::npos);
    CHECK(result.output.find("unresolved") == std::string::npos);
}

TEST_CASE("a trainer waits for its target and attaches when it appears", "[trainer][driver]")
{
    if (!std::filesystem::is_regular_file(trainer_binary()))
    {
        WARN("the trainer template was not built; the driver case is skipped");
        return;
    }

    constexpr std::string_view kLateName {"slopkit-late"};
    generate_trainer({value_entry("Health", literal_address(1))}, "late", {}, kLateName);

    RunResult   result;
    std::thread trainer {[&]
                         {
                             result = run_trainer(generated_trainer("late"), {"--wait", "3000", "--list"});
                         }};

    // The trainer has to be up and waiting before the target starts; the name is
    // unique to this case, so it cannot attach to anything else first.
    std::this_thread::sleep_for(std::chrono::milliseconds {500});
    const auto target = spawn_target(0, kLateName);
    trainer.join();

    CHECK(result.exit_code == 0);
    CHECK(result.output.find("waiting for it to appear") != std::string::npos);
    CHECK(result.output.find("pid " + std::to_string(target.pid)) != std::string::npos);
}

TEST_CASE("verbose logging writes a log file beside the trainer", "[trainer][driver]")
{
    if (!std::filesystem::is_regular_file(trainer_binary()))
    {
        WARN("the trainer template was not built; the driver case is skipped");
        return;
    }

    const auto target = spawn_target(0);
    generate_trainer({value_entry("Health", literal_address(target.address))}, "verbose");

    const auto trainer = generated_trainer("verbose");
    const auto log     = std::filesystem::path {trainer.string() + ".log"};

    // Nothing is written unless it was asked for, so a trainer behaves exactly as
    // it did before this flag existed.
    const auto quiet = run_trainer(trainer, {"--pid", std::to_string(target.pid), "--list"});
    CHECK(quiet.exit_code == 0);
    CHECK_FALSE(std::filesystem::exists(log));

    const auto result = run_trainer(trainer, {"--pid", std::to_string(target.pid), "--verbose", "--list"});
    CHECK(result.exit_code == 0);
    CHECK(result.output.find("verbose logging to") != std::string::npos);

    REQUIRE(std::filesystem::is_regular_file(log));
    const std::string text = read_file(log);
    CHECK(text.find("verbose logging to") != std::string::npos);
    CHECK(text.find("payload: 1 value entr(ies)") != std::string::npos);
    CHECK(text.find("attached to pid") != std::string::npos);
    CHECK(text.find("script host") != std::string::npos);
}

TEST_CASE("the trainer writes a value into a running process", "[trainer][driver]")
{
    if (!std::filesystem::is_regular_file(trainer_binary()))
    {
        WARN("the trainer template was not built; the driver case is skipped");
        return;
    }

    const auto target = spawn_target(0);
    generate_trainer({value_entry("Health", literal_address(target.address))}, "set");

    const auto result =
        run_trainer(generated_trainer("set"), {"--pid", std::to_string(target.pid), "--set", "1=250", "--list"});
    CHECK(result.exit_code == 0);
    CHECK(result.output.find("4 Bytes    250") != std::string::npos);
    CHECK(target.read() == kFrozen);
}

TEST_CASE("a frozen entry is restored while the target keeps rewriting it", "[trainer][driver]")
{
    if (!std::filesystem::is_regular_file(trainer_binary()))
    {
        WARN("the trainer template was not built; the driver case is skipped");
        return;
    }

    // The child overwrites for 500 ms while the trainer freezes: the trainer must
    // be seen winning, which only happens if it re-writes the value every tick.
    const auto target = spawn_target(500);
    generate_trainer({value_entry("Health", literal_address(target.address))}, "freeze");

    const auto result = run_trainer(generated_trainer("freeze"),
                                    {"--pid",
                                     std::to_string(target.pid),
                                     "--set",
                                     "1=250",
                                     "--freeze",
                                     "1",
                                     "--wait",
                                     "900",
                                     "--interval",
                                     "10",
                                     "--list"});
    CHECK(result.exit_code == 0);
    CHECK(result.output.find("4 Bytes    250") != std::string::npos);
    CHECK(target.read() == kFrozen);
}

TEST_CASE("an entry that cannot resolve is never written to", "[trainer][driver]")
{
    if (!std::filesystem::is_regular_file(trainer_binary()))
    {
        WARN("the trainer template was not built; the driver case is skipped");
        return;
    }

    const auto target = spawn_target(0);
    generate_trainer({value_entry("Health", "no-such-module+10")}, "unresolved");

    const auto result = run_trainer(generated_trainer("unresolved"),
                                    {"--pid", std::to_string(target.pid), "--freeze", "1", "--wait", "200", "--list"});
    CHECK(result.exit_code == 0);
    CHECK(result.output.find("unresolved") != std::string::npos);
    CHECK(target.read() == kInitial);
}

TEST_CASE("the trainer reports a target it cannot find", "[trainer][driver]")
{
    if (!std::filesystem::is_regular_file(trainer_binary()))
    {
        WARN("the trainer template was not built; the driver case is skipped");
        return;
    }

    const auto target = spawn_target(0);
    generate_trainer({value_entry("Health", literal_address(target.address))}, "missing");

    const auto result = run_trainer(generated_trainer("missing"), {"--pid", "999999999", "--list"});
    CHECK(result.exit_code == 3);
    CHECK(result.output.find("still running") != std::string::npos);

    const auto usage = run_trainer(generated_trainer("missing"), {"--nonsense"});
    CHECK(usage.exit_code == 1);
}

TEST_CASE("an activated script writes into the running process and publishes a symbol", "[trainer][driver]")
{
    if (!std::filesystem::is_regular_file(trainer_binary()))
    {
        WARN("the trainer template was not built; the driver case is skipped");
        return;
    }

    const auto target  = spawn_target(0);
    const auto address = literal_address(target.address);
    // The value entry names the symbol the script publishes, so a resolved row
    // proves the two halves of the run met.
    generate_trainer({value_entry("Health", "base")},
                     "script",
                     std::format(R"(
function activate()
    ssymbol("base", {})
    mem.write({}, "i32", 4242)
end

function deactivate()
    usymbol("base")
end
)",
                                 address,
                                 address));

    const auto result = run_trainer(
        generated_trainer("script"),
        {"--pid", std::to_string(target.pid), "--activate", "1", "--wait", "300", "--interval", "50", "--list"});
    CHECK(result.exit_code == 0);
    CHECK(result.output.find("4 Bytes    4242") != std::string::npos);
    CHECK(result.output.find("unresolved") == std::string::npos);
    CHECK(result.output.find("S1") != std::string::npos);
    CHECK(result.output.find("active") != std::string::npos);
    CHECK(target.read() == 4242);
}

TEST_CASE("a script stays idle until it is activated", "[trainer][driver]")
{
    if (!std::filesystem::is_regular_file(trainer_binary()))
    {
        WARN("the trainer template was not built; the driver case is skipped");
        return;
    }

    const auto target  = spawn_target(0);
    const auto address = literal_address(target.address);
    generate_trainer({value_entry("Health", "base")},
                     "script_idle",
                     std::format(R"(
function activate()
    ssymbol("base", {})
    mem.write({}, "i32", 4242)
end
)",
                                 address,
                                 address));

    const auto result = run_trainer(generated_trainer("script_idle"), {"--pid", std::to_string(target.pid), "--list"});
    CHECK(result.exit_code == 0);
    CHECK(result.output.find("idle") != std::string::npos);
    CHECK(result.output.find("unresolved") != std::string::npos);
    CHECK(result.output.find("active") == std::string::npos);
    CHECK(target.read() == kInitial);
}

TEST_CASE("a script that fails does not fail the trainer", "[trainer][driver]")
{
    if (!std::filesystem::is_regular_file(trainer_binary()))
    {
        WARN("the trainer template was not built; the driver case is skipped");
        return;
    }

    const auto target = spawn_target(0);
    generate_trainer({value_entry("Health", literal_address(target.address))},
                     "script_refused",
                     R"(
function activate()
    return false, "the target is not ready"
end
)");

    const auto result = run_trainer(generated_trainer("script_refused"),
                                    {"--pid", std::to_string(target.pid), "--activate", "1", "--list"});
    CHECK(result.exit_code == 0);
    CHECK(result.output.find("the target is not ready") != std::string::npos);
    CHECK(result.output.find("error") != std::string::npos);

    const auto usage = run_trainer(generated_trainer("script_refused"),
                                   {"--pid", std::to_string(target.pid), "--activate", "9", "--list"});
    CHECK(usage.exit_code == 1);
    CHECK(usage.output.find("no script 9") != std::string::npos);
}

// The only case that proves a Linux trainer reaches a game running under
// Wine/Proton. The guest is a real wine process running the mingw-built probe,
// and its name — `slopkit-probe-x86_64.exe` — is longer than the kernel's
// fifteen-character `comm`, so the name slopkit records for a Wine target can
// only be resolved through the guest's own images. Hidden and gated like the
// wine agent cases, because it needs wine and a working wine prefix:
//
//   SLOPKIT_WINE_E2E=1 ctest --test-dir build -R "attaches to a wine target" --timeout 600 --output-on-failure
TEST_CASE("the trainer attaches to a wine target by its guest name", "[trainer][wine_proton][.]")
{
    if (!std::filesystem::is_regular_file(trainer_binary()))
    {
        WARN("the trainer template was not built; the driver case is skipped");
        return;
    }

    const char* enabled = std::getenv("SLOPKIT_WINE_E2E");
    if (enabled == nullptr || std::string {enabled} != "1")
    {
        WARN("set SLOPKIT_WINE_E2E=1 to run the wine target case");
        return;
    }

    const auto      wine  = find_wine();
    const auto      probe = wine_probe();
    std::error_code error;
    if (wine.empty() || !std::filesystem::exists(probe, error))
    {
        WARN("wine or the 64-bit Windows probe is unavailable; the wine target case is skipped");
        return;
    }

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    const GuestGuard guard {child};
    if (child == 0)
    {
        ::execl(wine.c_str(), wine.c_str(), probe.c_str(), nullptr);
        ::_exit(127);
    }

    // The value entry names the guest's own module, so the row has to show an
    // address inside that image — the only way to reach one is the guest module
    // list this case exists to check. Whether the row then carries the value or
    // reports a refused read is the kernel's call: one user process may read
    // another's memory only when it is an ancestor.
    generate_trainer({value_entry("Image", probe.filename().string())}, "wine_target");

    // Wine needs a moment to map the guest; the lookup is retried until it does.
    RunResult  result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {120};
    do
    {
        result = run_trainer(generated_trainer("wine_target"), {"--name", probe.filename().string(), "--list"});
        if (result.exit_code == 0)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {500});
    }
    while (std::chrono::steady_clock::now() < deadline);

    CHECK(result.exit_code == 0);
    INFO(result.output);
    CHECK(result.output.find(std::format("(pid {})", child)) != std::string::npos);
    // The width of the guest's image, not of the 64-bit loader running it.
    CHECK(result.output.find("8-byte pointers") != std::string::npos);
    if (result.output.find("unresolved") != std::string::npos)
    {
        CHECK(result.output.find("cannot access 0x") != std::string::npos);
    }
}

// A script's `alloc` slot is memory the *target's* own code writes — a hook stores
// what it computed into it — so on a Windows target it must be a mapping of its own,
// not a run of zero bytes borrowed from the guest's data: such a run can be a
// module's zero-initialized data that the game fills as it loads, and the value the
// script then read back was the game's own. The mapping must also sit within a near
// jump's reach of the site that names it: `MOV qword ptr [<slot>], RBX` encodes only
// while the slot is a signed 32-bit displacement away, which is the friend's
// `the encoder rejected 'MOV qword ptr [0x1E029DF0000], RBX'`. This needs a Windows
// trainer and a Windows guest, which only Wine can host here:
//
//   SLOPKIT_WINE_E2E=1 ./build/slopkit_tests "a windows trainer maps a script's slot within reach of its site"
TEST_CASE("a windows trainer maps a script's slot within reach of its site", "[trainer][wine_proton][.]")
{
    if (!std::filesystem::is_regular_file(windows_trainer_binary()))
    {
        WARN("the Windows trainer template was not built; the wine slot case is skipped");
        return;
    }

    const char* enabled = std::getenv("SLOPKIT_WINE_E2E");
    if (enabled == nullptr || std::string {enabled} != "1")
    {
        WARN("set SLOPKIT_WINE_E2E=1 to run the wine slot case");
        return;
    }

    const auto      wine  = find_wine();
    const auto      probe = wine_probe();
    std::error_code error;
    if (wine.empty() || !std::filesystem::exists(probe, error))
    {
        WARN("wine or the 64-bit Windows probe is unavailable; the wine slot case is skipped");
        return;
    }

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    const GuestGuard guard {child};
    if (child == 0)
    {
        ::execl(wine.c_str(), wine.c_str(), probe.c_str(), nullptr);
        ::_exit(127);
    }

    // Wine needs a moment to start the guest and map its images.
    std::this_thread::sleep_for(std::chrono::seconds {3});

    std::error_code path_error;
    const auto      module = probe.filename().string();
    const auto      script = std::format(R"(
function activate()
    local found, site = aobscan("near_site", "4D 5A 90 00 03 00 00 00", "{}")
    if not found then
        print("scan failed")
        return false
    end
    local slot = alloc("slot", 8, site)
    local cave = alloc("cave", 8, site)
    print(string.format("site=0x%x slot=0x%x cave=0x%x", site, slot, cave))
    local encoded, reason = assemble(cave, string.format("MOV qword ptr [0x%x], RBX", slot))
    if not encoded then
        print("assemble failed: " .. tostring(reason))
        return false
    end
    write(slot, 1234, "u64")
    local stored = read_u64(slot)
    dealloc("cave")
    dealloc("slot")
    print(string.format("read=0x%x", stored))
    return true
end
)",
                                         module);

    generate_trainer({value_entry("Health", "0x1")}, "wine_slot", script, {}, true);

    const auto trainer = generated_windows_trainer("wine_slot");
    const auto log     = std::filesystem::path {trainer.string() + ".log"};
    std::filesystem::remove(log, path_error);

    const auto result = run_windows_trainer(
        wine,
        trainer,
        {"--name", probe.filename().string(), "--activate", "1", "--wait", "4000", "--list", "--verbose"});
    INFO(result.output);
    CHECK(result.exit_code == 0);
    CHECK(result.output.find("slot=0x") != std::string::npos);
    CHECK(result.output.find("read=0x4d2") != std::string::npos);
    // `MOV qword ptr [<slot>], RBX` encodes only while the slot is within a signed
    // 32-bit displacement of the instruction: the user's `the encoder rejected
    // 'MOV qword ptr [0x1E029DF0000], RBX'` is what a far-away mapping produces.
    CHECK(result.output.find("assemble failed") == std::string::npos);
    CHECK(result.output.find("active") != std::string::npos);

    // Both mappings sit where the code that names the slot can reach: an instruction
    // names an absolute address only within a signed 32-bit displacement, so a
    // mapping on the other side of the address space is rejected by the assembler.
    const auto at = result.output.find("site=0x");
    REQUIRE(at != std::string::npos);
    const auto site = std::stoull(result.output.substr(at + 7), nullptr, 16);
    const auto slot = std::stoull(result.output.substr(result.output.find("slot=0x") + 7), nullptr, 16);
    const auto cave = std::stoull(result.output.substr(result.output.find("cave=0x") + 7), nullptr, 16);
    CHECK((slot > site ? slot - site : site - slot) <= 0x7FFF'0000ull);
    // The payload assembled at the cave has to name the slot: both mappings are
    // within reach of the site, which keeps them within reach of each other.
    CHECK((cave > slot ? cave - slot : slot - cave) <= 0x7FFF'0000ull);

    // The slot reached the trainer's own writes, so it was mapped in the guest, and
    // freeing it gave the mapping back instead of zeroing borrowed bytes.
    const std::string text = std::filesystem::exists(log, path_error) ? read_file(log) : std::string {};
    INFO(text);
    CHECK(text.find("alloc: mapped 8 byte(s)") != std::string::npos);
    CHECK(text.find("unmapped") != std::string::npos);
}
