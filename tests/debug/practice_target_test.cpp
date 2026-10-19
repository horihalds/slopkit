#include <catch2/catch.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include <QString>

#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
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

using slopkit::debug::CodeFilterCandidate;
using slopkit::debug::CodeFilterState;
using slopkit::debug::Controller;
using slopkit::debug::Kind;
using slopkit::debug::PluginBackend;
using slopkit::debug::StopReason;
using slopkit::tests::drain_until_idle;
using slopkit::tests::pump_until;

namespace
{
    volatile std::uint32_t* g_counter = nullptr;

    __attribute__((noinline)) void debug_target()
    {
        *g_counter = *g_counter + 1;
    }

    __attribute__((noinline)) void never_called_target()
    {
        asm volatile("" ::: "memory");
    }

    // Keeping the address live stops the optimiser from discarding a function
    // that no caller names.
    void (*volatile g_never_called_target)() = &never_called_target;

    // A spare stack and body for a second thread, created with clone() because
    // the child is forked from a process whose allocator may be locked.
    alignas(16) std::array<char, 256 * 1024> g_thread_stack {};

    extern "C" int debug_target_thread(void*)
    {
        for (;;)
        {
            debug_target();
        }
        return 0;
    }

    std::size_t thread_count(pid_t pid)
    {
        std::error_code                     error;
        std::size_t                         count = 0;
        std::filesystem::directory_iterator entries {std::format("/proc/{}/task", pid), error};
        for (const auto& entry : entries)
        {
            (void)entry;
            ++count;
        }
        return count;
    }

    // A forked child looping over a known function. It shares the counter mapping
    // with the parent, so the watched address is the same on both sides and the
    // parent can see the counter advance; it is a child of the test process, which
    // Yama's ptrace_scope=1 allows.
    class DebugChild
    {
    public:
        explicit DebugChild(bool with_thread = false)
        {
            mapping_ = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            if (mapping_ != MAP_FAILED)
            {
                g_counter  = static_cast<volatile std::uint32_t*>(mapping_);
                *g_counter = 0;
            }

            const pid_t pid = ::fork();
            if (pid == 0)
            {
                if (with_thread)
                {
                    (void)::clone(debug_target_thread,
                                  g_thread_stack.data() + g_thread_stack.size(),
                                  CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | CLONE_FS | CLONE_FILES,
                                  nullptr);
                }
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
            if (mapping_ != MAP_FAILED)
            {
                ::munmap(mapping_, 4096);
            }
        }

        DebugChild(const DebugChild&)            = delete;
        DebugChild& operator=(const DebugChild&) = delete;

        [[nodiscard]] std::uint32_t pid() const
        {
            return static_cast<std::uint32_t>(pid_);
        }

        [[nodiscard]] std::uintptr_t counter_address() const
        {
            return reinterpret_cast<std::uintptr_t>(g_counter);
        }

    private:
        void* mapping_ {MAP_FAILED};
        pid_t pid_ {-1};
    };

    std::string hex(std::uintptr_t address)
    {
        return std::format("0x{:X}", address);
    }
}

TEST_CASE("an access watch arms on a running target", "[debug][practice]")
{
    DebugChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    PluginBackend backend(host);
    Controller    controller(backend);

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

    controller.start(child.pid(), "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

    const std::uint32_t before = *g_counter;
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return *g_counter != before;
                       }));

    const auto started = controller.watch_address(child.counter_address(), Kind::hardware_write, 4);
    REQUIRE(started.has_value());

    const bool settled = pump_until(
        controller,
        [&]
        {
            const auto* slot = controller.table().find(controller.watch().breakpoint_id());
            return (slot != nullptr && slot->armed)
                || controller.watch().state() == slopkit::debug::WatchState::stopped;
        },
        500);
    INFO("settled=" << settled << " error=" << last_error.toStdString()
                    << " state=" << static_cast<int>(controller.state())
                    << " watch=" << static_cast<int>(controller.watch().state()));
    const auto* armed_slot = controller.table().find(controller.watch().breakpoint_id());
    REQUIRE(armed_slot != nullptr);
    CHECK(armed_slot->armed);
    CHECK(controller.watch().state() == slopkit::debug::WatchState::watching);

    // A watch hit is consumed by the controller, so the target must keep running
    // while the hits arrive.
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.watch().hit_count() >= 1;
                       }));
    INFO("error=" << last_error.toStdString() << " state=" << static_cast<int>(controller.state())
                  << " watch=" << static_cast<int>(controller.watch().state()));
    CHECK(controller.state() == Controller::State::running);
    REQUIRE_FALSE(controller.watch().hits().empty());
    CHECK(controller.watch().hits().front().instruction != 0);
    CHECK(controller.watch().hits().front().tid == child.pid());

    controller.stop();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle;
                       }));
}

TEST_CASE("an access watch arms on a running threaded target", "[debug][practice]")
{
    DebugChild child(true);
    REQUIRE(child.pid() != 0);
    // The second thread must exist before attaching, so the session sees both.
    REQUIRE(slopkit::tests::wait_until(
        [&]
        {
            return thread_count(child.pid()) >= 2;
        }));

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    PluginBackend backend(host);
    Controller    controller(backend);

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

    controller.start(child.pid(), "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

    const std::uint32_t before = *g_counter;
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return *g_counter != before;
                       }));

    const auto started = controller.watch_address(child.counter_address(), Kind::hardware_write, 4);
    REQUIRE(started.has_value());

    const bool settled = pump_until(
        controller,
        [&]
        {
            const auto* slot = controller.table().find(controller.watch().breakpoint_id());
            return (slot != nullptr && slot->armed)
                || controller.watch().state() == slopkit::debug::WatchState::stopped;
        },
        500);
    INFO("settled=" << settled << " error=" << last_error.toStdString()
                    << " state=" << static_cast<int>(controller.state())
                    << " watch=" << static_cast<int>(controller.watch().state()));
    REQUIRE(controller.watch().state() == slopkit::debug::WatchState::watching);
    const auto* armed_slot = controller.table().find(controller.watch().breakpoint_id());
    REQUIRE(armed_slot != nullptr);
    CHECK(armed_slot->armed);

    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.watch().hit_count() >= 1;
                       }));
    INFO("error=" << last_error.toStdString() << " state=" << static_cast<int>(controller.state())
                  << " watch=" << static_cast<int>(controller.watch().state()));
    CHECK(controller.state() == Controller::State::running);
    CHECK(controller.watch().hit_count() >= 1);

    controller.stop();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle;
                       }));
}

TEST_CASE("a real debug session runs against a spawned target", "[debug][practice]")
{
    DebugChild child;
    REQUIRE(child.pid() != 0);
    REQUIRE(g_counter != nullptr);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    PluginBackend backend(host);
    Controller    controller(backend);

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

    bool stopped_emitted = false;
    QObject::connect(&controller,
                     &Controller::stopped,
                     &controller,
                     [&stopped_emitted]
                     {
                         stopped_emitted = true;
                     });

    controller.start(0, "linux-proc");
    CHECK(controller.state() == Controller::State::idle);

    // Attaching leaves the target running: no stop is reported and the register
    // and call-stack panes stay empty until the first deliberate stop.
    controller.start(child.pid(), "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));
    CHECK(controller.registers().empty());
    CHECK(controller.backtrace().empty());
    CHECK_FALSE(stopped_emitted);

    const std::uint32_t before = *g_counter;
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return *g_counter != before;
                       }));
    CHECK(controller.state() == Controller::State::running);
    CHECK_FALSE(stopped_emitted);

    controller.start(child.pid(), "linux-proc");
    CHECK(controller.state() == Controller::State::running);

    controller.interrupt();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped
                               && controller.registers().size() == 18;
                       }));

    REQUIRE_FALSE(controller.backtrace().empty());
    CHECK(controller.backtrace().front().pc != 0);

    const auto watch = controller.add_breakpoint(hex(child.counter_address()), Kind::hardware_write, 4);
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
    CHECK(controller.last_stop().trap_address == child.counter_address());
    CHECK(controller.table().find(*watch)->hits >= 1);

    controller.remove_breakpoint(*watch);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.table().empty();
                       }));

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

    controller.stop();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle;
                       }));
    CHECK(controller.registers().empty());

    controller.stop();
    CHECK(controller.state() == Controller::State::idle);

    CHECK(::kill(static_cast<pid_t>(child.pid()), 0) == 0);
    const auto status = slopkit::platform::read_status(child.pid());
    REQUIRE(status.has_value());
    CHECK(status->tracer_pid == 0);
}

TEST_CASE("a code filter collects while the practice target keeps running", "[debug][practice]")
{
    DebugChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    PluginBackend backend(host);
    Controller    controller(backend);

    controller.start(child.pid(), "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

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

    bool stopped_emitted = false;
    QObject::connect(&controller,
                     &Controller::stopped,
                     &controller,
                     [&stopped_emitted]
                     {
                         stopped_emitted = true;
                     });

    const std::uint64_t called = reinterpret_cast<std::uintptr_t>(&debug_target);
    const std::uint64_t never  = reinterpret_cast<std::uintptr_t>(g_never_called_target);
    REQUIRE(called != 0);
    REQUIRE(never != 0);
    REQUIRE(called != never);

    std::vector<CodeFilterCandidate> candidates(2);
    candidates[0].target = called;
    candidates[1].target = never;
    controller.start_code_filter(std::move(candidates));
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.code_filter().armed_count() == 2;
                       }));

    CHECK(controller.state() == Controller::State::running);
    CHECK_FALSE(stopped_emitted);
    CHECK(last_error.isEmpty());

    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.code_filter().candidates()[0].executions >= 1
                               && controller.table().software_at(called) == nullptr;
                       }));
    CHECK(controller.state() == Controller::State::running);
    CHECK_FALSE(stopped_emitted);
    CHECK(last_error.isEmpty());
    CHECK(controller.code_filter().candidates()[1].executions == 0);
    CHECK(controller.table().software_at(never) != nullptr);

    controller.start_code_filter(std::vector<CodeFilterCandidate> {CodeFilterCandidate {.target = never}});
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.code_filter().armed_count() == 1
                               && controller.code_filter().waiting_count() == 0;
                       }));
    CHECK(controller.state() == Controller::State::running);
    CHECK_FALSE(stopped_emitted);
    CHECK(last_error.isEmpty());

    controller.interrupt();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));

    controller.stop_code_filter();
    CHECK(controller.code_filter().state() == CodeFilterState::idle);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.table().empty();
                       }));

    controller.stop();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle;
                       }));
}

TEST_CASE("a code filter consumes its traps without stopping the practice target", "[debug][practice]")
{
    DebugChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    PluginBackend backend(host);
    Controller    controller(backend);

    controller.start(child.pid(), "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

    // A user breakpoint is the natural way into a stopped state here: an
    // interrupt leaves a pending group-stop that the next continue would report.
    const auto probe =
        controller.add_breakpoint(hex(reinterpret_cast<std::uintptr_t>(&debug_target)), Kind::software, 1);
    REQUIRE(probe.has_value());
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped
                               && controller.last_stop().trap_address
                                      == reinterpret_cast<std::uintptr_t>(&debug_target);
                       }));
    controller.remove_breakpoint(*probe);
    REQUIRE(drain_until_idle(controller));

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

    bool stopped_emitted = false;
    QObject::connect(&controller,
                     &Controller::stopped,
                     &controller,
                     [&stopped_emitted]
                     {
                         stopped_emitted = true;
                     });

    const std::uint64_t called = reinterpret_cast<std::uintptr_t>(&debug_target);
    const std::uint64_t never  = reinterpret_cast<std::uintptr_t>(g_never_called_target);
    REQUIRE(called != 0);
    REQUIRE(never != 0);
    REQUIRE(called != never);

    std::vector<CodeFilterCandidate> candidates(2);
    candidates[0].target = called;
    candidates[1].target = never;
    controller.start_code_filter(std::move(candidates));
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.code_filter().armed_count() == 2;
                       }));

    controller.resume();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.code_filter().candidates()[0].executions >= 1
                               && controller.table().software_at(called) == nullptr;
                       }));

    INFO("error=" << last_error.toStdString() << " state=" << static_cast<int>(controller.state())
                  << " stopped=" << stopped_emitted << " reason=" << static_cast<int>(controller.last_stop().reason)
                  << " trap=0x" << std::hex << controller.last_stop().trap_address << " called=0x" << called
                  << " entries=" << controller.table().entries().size());
    CHECK(controller.state() == Controller::State::running);
    CHECK_FALSE(stopped_emitted);
    CHECK(controller.code_filter().candidates()[0].executions == 1);
    CHECK(controller.code_filter().candidates()[1].executions == 0);
    CHECK(controller.table().software_at(called) == nullptr);
    CHECK(controller.table().software_at(never) != nullptr);

    controller.interrupt();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));

    controller.stop_code_filter();
    CHECK(controller.code_filter().state() == CodeFilterState::idle);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.table().empty();
                       }));

    controller.stop();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle;
                       }));
}
