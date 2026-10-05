#include <catch2/catch.hpp>

#include <cstdint>
#include <format>
#include <variant>
#include <vector>

#include "debug/controller.hpp"
#include "support/fake_debug.hpp"

using slopkit::debug::Controller;
using slopkit::debug::Kind;
using slopkit::debug::StopEvent;
using slopkit::debug::StopReason;
using slopkit::tests::FakeDebugBackend;
using slopkit::tests::pump_until;

namespace
{
    Controller::MessageKind last_kind {};
    QString                 last_text;
} // namespace

TEST_CASE("controller walks the session state machine", "[debug][controller]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);

    CHECK(controller.state() == Controller::State::idle);
    CHECK_FALSE(controller.has_target());
    CHECK(controller.state_text() == QStringLiteral("Not debugging"));

    controller.start(1234, "linux-proc");
    CHECK(controller.state() == Controller::State::starting);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped
                               && controller.registers().size() == 18 && controller.backtrace().size() == 1;
                       }));

    REQUIRE(backend.attached_pid.has_value());
    CHECK(*backend.attached_pid == 1234);
    CHECK(backend.attached_plugin == "linux-proc");
    CHECK(controller.state_text().startsWith(QStringLiteral("Stopped at")));

    controller.stop();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle;
                       }));
    CHECK(backend.count("detach") == 1);
    CHECK(controller.registers().empty());
    CHECK(controller.state_text() == QStringLiteral("Not debugging"));
}

TEST_CASE("controller refuses to start without a target or while open", "[debug][controller]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);

    int warnings = 0;
    QObject::connect(&controller,
                     &Controller::message,
                     &controller,
                     [&warnings](Controller::MessageKind kind, const QString&)
                     {
                         warnings += kind == Controller::MessageKind::warning ? 1 : 0;
                     });

    controller.start(0, "linux-proc");
    CHECK(controller.state() == Controller::State::idle);
    CHECK(warnings == 1);

    controller.start(1234, "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));

    controller.start(1234, "linux-proc");
    CHECK(warnings == 2);
    CHECK(controller.state() == Controller::State::stopped);
}

TEST_CASE("controller reports an unsupported plugin", "[debug][controller]")
{
    FakeDebugBackend backend;
    backend.attach_error = slopkit::process::AccessError::unsupported;
    Controller controller(backend);

    QString error;
    QObject::connect(&controller,
                     &Controller::message,
                     &controller,
                     [&error](Controller::MessageKind kind, const QString& text)
                     {
                         if (kind == Controller::MessageKind::error)
                         {
                             error = text;
                         }
                     });

    controller.start(1234, "wine-proton");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle && !error.isEmpty();
                       }));
    CHECK(error.contains(QStringLiteral("cannot debug")));
}

TEST_CASE("controller resolves a software trap and rewinds RIP", "[debug][controller]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);
    controller.start(4242, "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));

    const auto id = controller.add_breakpoint("0x1000", Kind::software, 1);
    REQUIRE(id.has_value());
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           const auto* entry = controller.table().find(*id);
                           return !backend.software_calls.empty() && entry != nullptr && entry->armed;
                       }));
    CHECK(std::get<0>(backend.software_calls.front()) == 0);
    CHECK(std::get<1>(backend.software_calls.front()) == 0x1000);
    CHECK(std::get<2>(backend.software_calls.front()));
    CHECK(controller.table().find(*id)->armed);

    backend.stop_replies.push_back(StopEvent {StopReason::breakpoint, 4242, 0x1001, 0x1000, std::nullopt, 0});
    controller.resume();
    CHECK(controller.state() == Controller::State::running);

    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return !backend.register_writes.empty();
                       }));

    CHECK(controller.state() == Controller::State::stopped);
    CHECK(controller.last_stop().trap_address == 0x1000);
    CHECK(controller.last_stop().address == 0x1000);
    CHECK(controller.table().find(*id)->hits == 1);
    CHECK(std::get<1>(backend.register_writes.front()) == "RIP");
    CHECK(std::get<2>(backend.register_writes.front()) == 0x1000);
    CHECK(controller.state_text().contains(QStringLiteral("breakpoint 1")));
}

TEST_CASE("controller lifts a trap sitting at RIP when stepping", "[debug][controller]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);
    controller.start(4242, "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));

    const auto id = controller.add_breakpoint("0x1000", Kind::software, 1);
    REQUIRE(id.has_value());
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return !backend.software_calls.empty();
                       }));

    backend.stop_replies.push_back(StopEvent {StopReason::breakpoint, 4242, 0x1001, 0x1000, std::nullopt, 0});
    controller.resume();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return !backend.register_writes.empty();
                       }));

    backend.calls.clear();
    backend.stop_replies.push_back(StopEvent {StopReason::single_step, 4242, 0x1002, 0x1002, std::nullopt, 0});
    controller.step_over();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped && backend.count("cont") >= 1;
                       }));
    CHECK(backend.last_resume_address == 0x1000);
    CHECK(backend.last_resume_step_size == 1);
}

TEST_CASE("controller steps plainly away from a trap", "[debug][controller]")
{
    FakeDebugBackend backend;
    backend.register_file = slopkit::tests::default_registers(0x5000);
    Controller controller(backend);
    controller.start(4242, "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));

    backend.calls.clear();
    controller.step_into();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped && backend.count("step") >= 1;
                       }));
    CHECK(backend.count("cont") == 0);
    CHECK(backend.last_step_tid == 4242);
}

TEST_CASE("controller arms a hardware breakpoint in a DR slot", "[debug][controller]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);
    controller.start(4242, "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));

    const auto id = controller.add_breakpoint("0x2000", Kind::hardware_write, 4);
    REQUIRE(id.has_value());
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           const auto* entry = controller.table().find(*id);
                           return !backend.hardware_calls.empty() && entry != nullptr && entry->armed;
                       }));
    CHECK(std::get<0>(backend.hardware_calls.front()) == 0);
    CHECK(std::get<1>(backend.hardware_calls.front()) == slopkit::debug::HardwareKind::write);
    CHECK(std::get<2>(backend.hardware_calls.front()) == 0x2000);
    CHECK(std::get<3>(backend.hardware_calls.front()) == 4);

    // Removing it disarms the slot.
    backend.hardware_calls.clear();
    controller.remove_breakpoint(*id);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return !backend.hardware_calls.empty() && controller.table().empty();
                       }));
    CHECK_FALSE(std::get<4>(backend.hardware_calls.front()));
    CHECK(controller.table().empty());
}

TEST_CASE("controller refuses an unresolvable breakpoint expression", "[debug][controller]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);

    const auto missing = controller.add_breakpoint("not-a-module+10", Kind::software, 1);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().find("resolve") != std::string::npos);
}

TEST_CASE("controller gates register writes on the stopped state", "[debug][controller]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);

    int warnings = 0;
    QObject::connect(&controller,
                     &Controller::message,
                     &controller,
                     [&warnings](Controller::MessageKind kind, const QString&)
                     {
                         warnings += kind == Controller::MessageKind::warning ? 1 : 0;
                     });

    // Idle: refused, nothing reaches the backend.
    controller.write_register("RAX", 1);
    CHECK(warnings == 1);
    CHECK(backend.count("set_register") == 0);

    controller.start(4242, "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));

    controller.write_register("RAX", 0x42);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return backend.count("set_register") >= 1;
                       }));
    REQUIRE(!backend.register_writes.empty());
    CHECK(std::get<1>(backend.register_writes.front()) == "RAX");
    CHECK(std::get<2>(backend.register_writes.front()) == 0x42);
}

TEST_CASE("controller arms breakpoints while the target runs through one invisible stop", "[debug][controller]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);
    backend.block_continue = true;
    controller.start(4242, "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

    std::vector<Controller::State> states;
    QObject::connect(&controller,
                     &Controller::stateChanged,
                     &controller,
                     [&states, &controller]
                     {
                         states.push_back(controller.state());
                     });
    backend.calls.clear();

    const auto first  = controller.add_breakpoint("0x2000", Kind::hardware_write, 4);
    const auto second = controller.add_breakpoint("0x3000", Kind::hardware_write, 4);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    // Both ops ride a single maintenance stop, and the target never looks stopped.
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.table().find(*first)->armed && controller.table().find(*second)->armed;
                       }));
    CHECK(backend.count("interrupt") == 1);
    CHECK(backend.hardware_calls.size() == 2);
    CHECK(controller.state() == Controller::State::running);
    for (const Controller::State state : states)
    {
        CHECK(state != Controller::State::stopped);
    }
}

TEST_CASE("controller disarms a breakpoint while the target runs", "[debug][controller]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);
    backend.block_continue = true;
    controller.start(4242, "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

    const auto id = controller.add_breakpoint("0x2000", Kind::hardware_write, 4);
    REQUIRE(id.has_value());
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.table().find(*id)->armed;
                       }));

    backend.calls.clear();
    backend.hardware_calls.clear();
    controller.remove_breakpoint(*id);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.table().find(*id) == nullptr;
                       }));

    CHECK(controller.state() == Controller::State::running);
    CHECK(backend.count("interrupt") == 1);
    REQUIRE(backend.hardware_calls.size() == 1);
    CHECK_FALSE(std::get<4>(backend.hardware_calls.front()));
}

namespace
{
    const slopkit::debug::Breakpoint* hidden_entry(const Controller& controller)
    {
        for (const slopkit::debug::Breakpoint& entry : controller.breakpoints())
        {
            if (entry.hidden)
            {
                return &entry;
            }
        }
        return nullptr;
    }
} // namespace

TEST_CASE("controller collects access watch hits while the target runs", "[debug][controller]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);
    backend.block_continue = true;
    controller.start(4242, "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

    // The watch arms through the maintenance stop and leaves the target running.
    REQUIRE(controller.watch_address(0x4000, Kind::hardware_write, 4).has_value());
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           const auto* entry = hidden_entry(controller);
                           return entry != nullptr && entry->armed;
                       }));
    CHECK(controller.watch().state() == slopkit::debug::WatchState::watching);
    CHECK(controller.watch().address() == 0x4000);
    CHECK(controller.watch().kind() == Kind::hardware_write);
    CHECK(controller.watch().size() == 4);
    CHECK(controller.state() == Controller::State::running);
    const std::uint32_t slot = hidden_entry(controller)->slot;

    // Two stops on the watched slot coalesce into one row with a count, and the
    // target is resumed after each without ever looking stopped.
    backend.stop_replies.push_back(StopEvent {StopReason::breakpoint, 4242, 0x5005, 0x5005, slot, 0});
    backend.stop_replies.push_back(StopEvent {StopReason::breakpoint, 4242, 0x5005, 0x5005, slot, 0});

    controller.interrupt();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.watch().hit_count() == 1;
                       }));
    CHECK(controller.state() == Controller::State::running);
    controller.interrupt();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.watch().hit_count() == 2;
                       }));

    CHECK(controller.state() == Controller::State::running);
    REQUIRE(controller.watch().hits().size() == 1);
    CHECK(controller.watch().hits().front().instruction == 0x5005);
    CHECK(controller.watch().hits().front().tid == 4242);
    CHECK(controller.watch().hits().front().count == 2);

    // Clear empties the rows but keeps collecting.
    controller.clear_watch_hits();
    CHECK(controller.watch().hits().empty());
    CHECK(controller.watch().hit_count() == 0);
    CHECK(controller.watch().state() == slopkit::debug::WatchState::watching);

    // A new watch replaces the running one and drops the rows.
    REQUIRE(controller.watch_address(0x6000, Kind::hardware_read_write, 8).has_value());
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           const auto* entry = hidden_entry(controller);
                           return entry != nullptr && entry->address == 0x6000 && entry->armed;
                       }));
    CHECK(controller.watch().address() == 0x6000);
    CHECK(controller.watch().size() == 8);
    CHECK(controller.watch().hits().empty());
    CHECK(controller.state() == Controller::State::running);

    // Stop disarms the slot and leaves the target running with the rows kept.
    controller.stop_watch();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return hidden_entry(controller) == nullptr;
                       }));
    CHECK(controller.watch().state() == slopkit::debug::WatchState::stopped);
    CHECK(controller.state() == Controller::State::running);
}

TEST_CASE("controller refuses an access watch it cannot arm", "[debug][controller]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);
    controller.start(4242, "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));

    // All four debug slots taken: the watch cannot be armed.
    for (int index = 0; index < 4; ++index)
    {
        const auto id =
            controller.add_breakpoint(std::format("0x{:X}", 0x1000 + index * 0x100), Kind::hardware_execute, 1);
        REQUIRE(id.has_value());
    }
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           std::size_t armed = 0;
                           for (const slopkit::debug::Breakpoint& entry : controller.breakpoints())
                           {
                               armed += entry.armed ? 1 : 0;
                           }
                           return armed == 4;
                       }));

    const auto no_slot = controller.watch_address(0x9000, Kind::hardware_write, 4);
    REQUIRE_FALSE(no_slot.has_value());
    CHECK(no_slot.error() == "no hardware slot is free");
    CHECK(controller.watch().state() == slopkit::debug::WatchState::idle);

    // An address a hardware breakpoint already covers is refused too.
    controller.clear_breakpoints();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.breakpoints().empty();
                       }));
    const auto covered = controller.add_breakpoint("0x4000", Kind::hardware_write, 4);
    REQUIRE(covered.has_value());
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.table().find(*covered)->armed;
                       }));

    const auto duplicate = controller.watch_address(0x4000, Kind::hardware_write, 4);
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error() == "a breakpoint already covers this address");
    CHECK(controller.watch().state() == slopkit::debug::WatchState::idle);
}

TEST_CASE("controller captures the registers of a running target invisibly", "[debug][controller]")
{
    FakeDebugBackend backend;
    backend.register_file = slopkit::tests::default_registers(0x7000);
    Controller controller(backend);
    backend.block_continue = true;
    controller.start(4242, "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

    int registers_changed = 0;
    int stopped_reports   = 0;
    int state_changes     = 0;
    QObject::connect(&controller,
                     &Controller::registersChanged,
                     &controller,
                     [&registers_changed]
                     {
                         ++registers_changed;
                     });
    QObject::connect(&controller,
                     &Controller::stopped,
                     &controller,
                     [&stopped_reports]
                     {
                         ++stopped_reports;
                     });
    QObject::connect(&controller,
                     &Controller::stateChanged,
                     &controller,
                     [&state_changes]
                     {
                         ++state_changes;
                     });

    backend.calls.clear();
    int captured = 0;
    controller.capture_registers(
        [&captured]
        {
            ++captured;
        });

    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return captured == 1;
                       }));
    CHECK(captured == 1);
    CHECK(registers_changed == 1);
    CHECK(stopped_reports == 0);
    CHECK(state_changes == 0);
    CHECK(controller.state() == Controller::State::running);
    REQUIRE(controller.registers().size() == 18);
    bool rip_visible = false;
    for (const auto& value : controller.registers())
    {
        rip_visible = rip_visible || (value.name == "RIP" && value.value == 0x7000);
    }
    CHECK(rip_visible);
    CHECK(backend.count("interrupt") == 1);
    CHECK(backend.count("registers") == 1);
    // The target was put back running after the capture.
    CHECK(pump_until(controller,
                     [&]
                     {
                         return backend.count("cont") >= 1;
                     }));
}

TEST_CASE("controller captures immediately when the target is stopped or absent", "[debug][controller]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);

    // No session: the callback runs right away, nothing reaches the backend.
    int idle_captured = 0;
    controller.capture_registers(
        [&idle_captured]
        {
            ++idle_captured;
        });
    CHECK(idle_captured == 1);
    CHECK(backend.count("interrupt") == 0);
    CHECK(backend.count("registers") == 0);

    controller.start(4242, "linux-proc");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped
                               && controller.registers().size() == 18;
                       }));

    backend.calls.clear();
    int stopped_captured = 0;
    controller.capture_registers(
        [&stopped_captured]
        {
            ++stopped_captured;
        });
    CHECK(stopped_captured == 1);
    CHECK(backend.count("interrupt") == 0);
    CHECK(backend.count("registers") == 0);
    CHECK(controller.state() == Controller::State::stopped);
}
