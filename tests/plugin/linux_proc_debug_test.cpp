#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/linux/procfs.hpp"
#include "plugin/plugin.hpp"
#include "plugin/plugin_host.hpp"
#include "process/types.hpp"

namespace
{
    using slopkit::process::AccessMethod;

    // Shared with the forked child: it runs this in a loop so the parent always
    // has a stable code address to put breakpoints on.
    volatile std::uint32_t g_tick = 0;

    __attribute__((noinline)) void debug_target()
    {
        g_tick = g_tick + 1;
    }

    // A forked child stuck in a tight loop over a known function.
    class DebugChild
    {
    public:
        DebugChild()
        {
            const pid_t pid = ::fork();
            if (pid == 0)
            {
                for (;;)
                {
                    debug_target();
                }
                ::_exit(0);
            }
            if (pid > 0)
            {
                pid_ = pid;
            }
        }

        ~DebugChild()
        {
            if (pid_ > 0)
            {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, nullptr, 0);
            }
        }

        DebugChild(const DebugChild&)            = delete;
        DebugChild& operator=(const DebugChild&) = delete;

        [[nodiscard]] std::uint32_t pid() const
        {
            return static_cast<std::uint32_t>(pid_);
        }

    private:
        pid_t pid_ {-1};
    };

    std::optional<std::uint64_t>
    register_value(const slopkit_register_value* values, std::size_t count, std::string_view name)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            if (values[i].name != nullptr && std::string_view {values[i].name} == name)
            {
                return values[i].value;
            }
        }
        return std::nullopt;
    }
} // namespace

TEST_CASE("linux-proc debugs a spawned target through the ABI", "[linux_proc][debug]")
{
    DebugChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_debug());

    auto* vtable = session->vtable();
    void* handle = session->handle();
    REQUIRE(vtable != nullptr);

    // Reads go through the normal process_vm/procfs path, so the original byte
    // at the target can be compared before and after arming.
    const auto read_target = [&session](std::uint64_t address)
    {
        AccessMethod method = AccessMethod::none;
        return session->read(address, 1, method);
    };

    const auto target = reinterpret_cast<std::uint64_t>(&debug_target);
    const auto before = read_target(target);
    REQUIRE(before.has_value());
    REQUIRE(before->size() == 1);
    const auto original = (*before)[0];
    CHECK(original != std::byte {0xCC});

    std::uint32_t leader = 0;
    REQUIRE(vtable->debug_attach(handle, &leader).code == SLOPKIT_OK);
    CHECK(leader == child.pid());

    // Attaching twice is idempotent.
    REQUIRE(vtable->debug_attach(handle, &leader).code == SLOPKIT_OK);

    // The 18 registers are readable and writable while stopped.
    slopkit_register_value* registers      = nullptr;
    std::size_t             register_count = 0;
    REQUIRE(vtable->debug_get_registers(handle, leader, &registers, &register_count).code == SLOPKIT_OK);
    REQUIRE(register_count == 18);
    const auto rip = register_value(registers, register_count, "RIP");
    REQUIRE(rip.has_value());
    CHECK(*rip != 0);

    REQUIRE(vtable->debug_set_register(handle, leader, "RAX", 0xDEAD'BEEF).code == SLOPKIT_OK);
    slopkit_register_value* reread      = nullptr;
    std::size_t             reread_size = 0;
    REQUIRE(vtable->debug_get_registers(handle, leader, &reread, &reread_size).code == SLOPKIT_OK);
    CHECK(register_value(reread, reread_size, "RAX") == 0xDEAD'BEEF);

    CHECK(vtable->debug_set_register(handle, leader, "NOT_A_REGISTER", 1).code == SLOPKIT_ERR_INVALID_ARGUMENT);

    // Arm a software breakpoint and run into it.
    REQUIRE(vtable->debug_set_software_breakpoint(handle, 0, target, 1).code == SLOPKIT_OK);
    const auto armed = read_target(target);
    REQUIRE(armed.has_value());
    CHECK((*armed)[0] == std::byte {0xCC});

    slopkit_stop_info stop {};
    REQUIRE(vtable->debug_continue(handle, 0, 0, &stop).code == SLOPKIT_OK);
    CHECK(stop.reason == SLOPKIT_STOP_BREAKPOINT);
    CHECK(stop.trap_address == target);
    CHECK(stop.address == target + 1);
    CHECK(stop.breakpoint_slot == -1);

    // Step over the trap: the original byte runs, the trap is re-inserted.
    slopkit_stop_info stepped {};
    REQUIRE(vtable->debug_continue(handle, target, 1, &stepped).code == SLOPKIT_OK);
    CHECK(stepped.reason == SLOPKIT_STOP_SINGLE_STEP);
    CHECK(stepped.address != target);
    const auto rearmed = read_target(target);
    REQUIRE(rearmed.has_value());
    CHECK((*rearmed)[0] == std::byte {0xCC});

    // One more plain single step advances one instruction from a valid RIP.
    slopkit_stop_info single {};
    REQUIRE(vtable->debug_step(handle, stepped.tid, &single).code == SLOPKIT_OK);
    CHECK(single.reason == SLOPKIT_STOP_SINGLE_STEP);
    CHECK(single.address != stepped.address);

    // The top frame of the backtrace is the stopped instruction pointer.
    slopkit_frame_info* frames      = nullptr;
    std::size_t         frame_count = 0;
    REQUIRE(vtable->debug_backtrace(handle, single.tid, &frames, &frame_count).code == SLOPKIT_OK);
    REQUIRE(frame_count >= 1);
    CHECK(frames[0].pc == single.address);

    // Running on hits the re-armed breakpoint again.
    slopkit_stop_info again {};
    REQUIRE(vtable->debug_continue(handle, 0, 0, &again).code == SLOPKIT_OK);
    CHECK(again.reason == SLOPKIT_STOP_BREAKPOINT);
    CHECK(again.trap_address == target);

    // Leave the trap cleanly, then take it away.
    slopkit_stop_info cleared {};
    REQUIRE(vtable->debug_continue(handle, target, 1, &cleared).code == SLOPKIT_OK);
    CHECK(cleared.reason == SLOPKIT_STOP_SINGLE_STEP);
    REQUIRE(vtable->debug_set_software_breakpoint(handle, 0, target, 0).code == SLOPKIT_OK);
    const auto restored = read_target(target);
    REQUIRE(restored.has_value());
    CHECK((*restored)[0] == original);

    // A hardware execute breakpoint fires and names its DR slot.
    REQUIRE(vtable->debug_set_hardware_breakpoint(handle, 0, SLOPKIT_BP_HW_EXECUTE, target, 1, 1).code == SLOPKIT_OK);
    slopkit_stop_info hardware {};
    REQUIRE(vtable->debug_continue(handle, 0, 0, &hardware).code == SLOPKIT_OK);
    CHECK(hardware.reason == SLOPKIT_STOP_BREAKPOINT);
    CHECK(hardware.breakpoint_slot == 0);
    CHECK(hardware.trap_address == target);

    // Slot 4 does not exist.
    CHECK(vtable->debug_set_hardware_breakpoint(handle, 4, SLOPKIT_BP_HW_EXECUTE, target, 1, 1).code
          == SLOPKIT_ERR_INVALID_ARGUMENT);
    REQUIRE(vtable->debug_set_hardware_breakpoint(handle, 0, SLOPKIT_BP_HW_EXECUTE, target, 1, 0).code == SLOPKIT_OK);

    // Detaching resumes the target and drops the tracer.
    REQUIRE(vtable->debug_detach(handle).code == SLOPKIT_OK);
    CHECK(vtable->debug_detach(handle).code == SLOPKIT_OK); // idempotent

    CHECK(::kill(static_cast<pid_t>(child.pid()), 0) == 0);
    const auto status = slopkit::platform::read_status(child.pid());
    REQUIRE(status.has_value());
    CHECK(status->tracer_pid == 0);
}
