#include <catch2/catch.hpp>

#include <cstdint>
#include <format>
#include <string>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "debug/controller.hpp"
#include "debug/plugin_backend.hpp"
#include "platform/linux/procfs.hpp"
#include "plugin/plugin_host.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "support/fake_debug.hpp"

using slopkit::debug::Controller;
using slopkit::debug::Kind;
using slopkit::debug::PluginBackend;
using slopkit::debug::StopReason;
using slopkit::tests::pump_until;

namespace
{
    volatile std::uint32_t g_tick = 0;

    __attribute__((noinline)) void debug_target()
    {
        g_tick = g_tick + 1;
    }

    // A forked child looping over a known function. It inherits the parent's
    // address space, so the watched address is the same on both sides and it is
    // a child of the test process, which Yama's ptrace_scope=1 allows.
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

    std::string hex(std::uintptr_t address)
    {
        return std::format("0x{:X}", address);
    }
} // namespace

TEST_CASE("a real debug session runs against a spawned target", "[debug][practice]")
{
    DebugChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    PluginBackend backend(host);
    Controller    controller(backend);

    // Records the last refusal so a failed stop is not a mystery.
    QString last_error;
    QObject::connect(&controller,
                     &Controller::message,
                     &controller,
                     [&last_error](Controller::MessageKind kind, const QString& text)
                     {
                         if (kind == Controller::MessageKind::warning || kind == Controller::MessageKind::error)
                         {
                             last_error = text;
                         }
                     });

    // Start Debugging with no attached target is refused.
    controller.start(0, "linux-proc");
    CHECK(controller.state() == Controller::State::idle);

    controller.start(child.pid(), "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped
                               && controller.registers().size() == 18;
                       }));

    // A second start while a session is open is refused.
    controller.start(child.pid(), "linux-proc");
    CHECK(controller.state() == Controller::State::stopped);

    // The call stack has at least the stopped frame.
    REQUIRE_FALSE(controller.backtrace().empty());
    CHECK(controller.backtrace().front().pc != 0);

    // A write watchpoint on the animated value stops the target and names its slot.
    const auto watch =
        controller.add_breakpoint(hex(reinterpret_cast<std::uintptr_t>(&g_tick)), Kind::hardware_write, 4);
    REQUIRE(watch.has_value());
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.table().find(*watch)->armed;
                       }));

    controller.resume();
    pump_until(controller,
               [&]
               {
                   return controller.state() != Controller::State::running;
               });
    INFO(last_error.toStdString());
    INFO("watch state=" << static_cast<int>(controller.state())
                        << " reason=" << static_cast<int>(controller.last_stop().reason) << " slot="
                        << (controller.last_stop().breakpoint_slot
                                ? static_cast<int>(*controller.last_stop().breakpoint_slot)
                                : -1)
                        << " trap=0x" << std::hex << controller.last_stop().trap_address);
    REQUIRE(controller.state() == Controller::State::stopped);
    REQUIRE(controller.last_stop().reason == StopReason::breakpoint);
    REQUIRE(controller.last_stop().breakpoint_slot.has_value());
    CHECK(*controller.last_stop().breakpoint_slot == 0);
    CHECK(controller.last_stop().trap_address == reinterpret_cast<std::uintptr_t>(&g_tick));
    CHECK(controller.table().find(*watch)->hits >= 1);

    // Put the watchpoint away again.
    controller.remove_breakpoint(*watch);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.table().empty();
                       }));

    // A software breakpoint on the loop body: resume into it, step over it.
    const auto trap =
        controller.add_breakpoint(hex(reinterpret_cast<std::uintptr_t>(&debug_target)), Kind::software, 1);
    REQUIRE(trap.has_value());
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.table().find(*trap)->armed;
                       }));

    controller.resume();
    pump_until(controller,
               [&]
               {
                   return controller.state() != Controller::State::running;
               });
    INFO(last_error.toStdString());
    INFO("sw state=" << static_cast<int>(controller.state())
                     << " reason=" << static_cast<int>(controller.last_stop().reason) << " trap=0x" << std::hex
                     << controller.last_stop().trap_address << " target=0x"
                     << reinterpret_cast<std::uintptr_t>(&debug_target));
    REQUIRE(controller.state() == Controller::State::stopped);
    REQUIRE(controller.last_stop().trap_address == reinterpret_cast<std::uintptr_t>(&debug_target));
    CHECK(controller.last_stop().address == reinterpret_cast<std::uintptr_t>(&debug_target));
    CHECK(controller.table().find(*trap)->hits == 1);

    // Step Over the trap: the original instruction runs, the trap is re-armed.
    controller.step_over();
    pump_until(controller,
               [&]
               {
                   return controller.state() != Controller::State::running;
               });
    INFO(last_error.toStdString());
    INFO("step state=" << static_cast<int>(controller.state())
                       << " reason=" << static_cast<int>(controller.last_stop().reason) << " slot="
                       << (controller.last_stop().breakpoint_slot
                               ? static_cast<int>(*controller.last_stop().breakpoint_slot)
                               : -1)
                       << " trap=0x" << std::hex << controller.last_stop().trap_address << " target=0x"
                       << reinterpret_cast<std::uintptr_t>(&debug_target));
    CHECK(controller.table().find(*trap)->armed);
    CHECK(controller.last_stop().reason == StopReason::single_step);

    // Stopping the session detaches and resumes the target.
    controller.stop();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle;
                       }));
    CHECK(controller.registers().empty());

    // Stopping an idle session is a no-op.
    controller.stop();
    CHECK(controller.state() == Controller::State::idle);

    CHECK(::kill(static_cast<pid_t>(child.pid()), 0) == 0);
    const auto status = slopkit::platform::read_status(child.pid());
    REQUIRE(status.has_value());
    CHECK(status->tracer_pid == 0);
}
