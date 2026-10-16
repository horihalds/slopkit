#include <catch2/catch.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

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

    ChildTarget spawn_target(std::uint32_t rewrite_ms)
    {
        int fds[2] {0, 0};
        REQUIRE(::pipe(fds) == 0);

        const pid_t child = ::fork();
        REQUIRE(child >= 0);

        if (child == 0)
        {
            ::close(fds[0]);
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

    struct RunResult
    {
        int         exit_code {0};
        std::string output;
    };

    RunResult run_trainer(const std::filesystem::path& trainer, const std::vector<std::string>& arguments)
    {
        std::string command = trainer.string();
        for (const auto& argument : arguments)
        {
            command += " ";
            command += argument;
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

    void generate_trainer(const std::vector<AddressEntry>& entries, std::string_view name)
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

        Request request;
        request.table            = &table;
        request.pointer_size     = sizeof(void*);
        request.linux_target     = true;
        request.windows_target   = false;
        request.output_directory = directory;
        request.base_name        = "practice";

        const auto result = slopkit::trainer::generate(request);
        REQUIRE(result.has_value());
        REQUIRE(result->errors.empty());
        REQUIRE(result->written.size() == 1);
        REQUIRE(std::filesystem::is_regular_file(trainer_binary()));
    }

    std::filesystem::path generated_trainer(std::string_view name)
    {
        return std::filesystem::path {SLOPKIT_TMP_DIR} / "trainer_driver" / name / "practice.trainer";
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
