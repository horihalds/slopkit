#include <catch2/catch.hpp>

#include <cstdint>
#include <variant>

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
                           return !backend.software_calls.empty();
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
                           return !backend.hardware_calls.empty();
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
                           return !backend.hardware_calls.empty();
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
