#include <catch2/catch.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QString>
#include <QWidget>

#include "support/fake_debug.hpp"
#include "support/fake_process.hpp"
#include "ui/debug_session.hpp"

using slopkit::debug::Controller;
using slopkit::process::AttachedTarget;
using slopkit::ui::DebugSessionGate;

namespace
{
    // Drains the controller and the Qt event loop until `done` holds, so the
    // deferred ready-callbacks the gate posts actually run.
    template<typename Predicate>
    bool pump_debug(Controller& controller, Predicate done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            controller.drain();
            QCoreApplication::processEvents();
            if (done())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    }
} // namespace

TEST_CASE("the debug session gate reuses a live session without prompting", "[ui][debug_session]")
{
    slopkit::test::application();

    slopkit::tests::FakeDebugBackend backend;
    Controller                       controller {backend};
    backend.block_continue = true;
    controller.start(42, "fake");
    REQUIRE(pump_debug(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

    AttachedTarget                target = slopkit::test::fake_target();
    QWidget                       owner;
    slopkit::ui::DebugSessionGate gate {controller, target, owner};

    int prompt_calls = 0;
    gate.set_attach_prompt(
        [&prompt_calls](const AttachedTarget&, const QString&)
        {
            ++prompt_calls;
            return false;
        });

    int ready = 0;
    gate.with_session(QStringLiteral("find out what writes this address"),
                      [&ready]
                      {
                          ++ready;
                      });
    CHECK(ready == 1); // the live session hands control back synchronously
    CHECK(prompt_calls == 0);
    CHECK(backend.count("attach") == 1); // only the start that was already there
    CHECK(gate.session_live());
}

TEST_CASE("the debug session gate attaches after consent and waits for running", "[ui][debug_session]")
{
    slopkit::test::application();

    slopkit::tests::FakeDebugBackend backend;
    backend.block_continue = true;
    Controller controller {backend};

    AttachedTarget                target = slopkit::test::fake_target();
    QWidget                       owner;
    slopkit::ui::DebugSessionGate gate {controller, target, owner};

    std::vector<QString> progress;
    std::vector<bool>    errors;
    QObject::connect(&gate,
                     &DebugSessionGate::progress,
                     &gate,
                     [&progress, &errors](const QString& text, bool error)
                     {
                         progress.push_back(text);
                         errors.push_back(error);
                     });

    int prompt_calls = 0;
    gate.set_attach_prompt(
        [&prompt_calls](const AttachedTarget& offered, const QString& reason)
        {
            ++prompt_calls;
            CHECK(offered.pid == 42);
            CHECK(reason == QStringLiteral("find out what writes this address"));
            return true;
        });

    int ready = 0;
    gate.with_session(QStringLiteral("find out what writes this address"),
                      [&ready]
                      {
                          ++ready;
                      });

    CHECK(prompt_calls == 1);
    CHECK(ready == 0); // not before the session runs
    CHECK(controller.state() == Controller::State::starting);
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.front().contains(QStringLiteral("Attaching the debugger")));
    CHECK_FALSE(errors.front());

    REQUIRE(pump_debug(controller,
                       [&]
                       {
                           return ready == 1;
                       }));
    CHECK(controller.state() == Controller::State::running);
    REQUIRE(backend.attached_pid.has_value());
    CHECK(*backend.attached_pid == 42);
    CHECK(backend.attached_plugin == "fake");
    CHECK(backend.count("attach") == 1);
    REQUIRE(progress.size() >= 2);
    CHECK(progress[1].contains(QStringLiteral("Debugger attached")));
    CHECK_FALSE(errors[1]);
}

TEST_CASE("the debug session gate drops the work when the prompt is declined", "[ui][debug_session]")
{
    slopkit::test::application();

    slopkit::tests::FakeDebugBackend backend;
    Controller                       controller {backend};

    AttachedTarget                target = slopkit::test::fake_target();
    QWidget                       owner;
    slopkit::ui::DebugSessionGate gate {controller, target, owner};

    QString cancelled;
    QObject::connect(&gate,
                     &DebugSessionGate::progress,
                     &gate,
                     [&cancelled](const QString& text, bool error)
                     {
                         if (!error)
                         {
                             cancelled = text;
                         }
                     });
    int prompt_calls = 0;
    gate.set_attach_prompt(
        [&prompt_calls](const AttachedTarget&, const QString&)
        {
            ++prompt_calls;
            return false;
        });

    int ready = 0;
    gate.with_session(QStringLiteral("find out what writes this address"),
                      [&ready]
                      {
                          ++ready;
                      });
    CHECK(prompt_calls == 1);
    CHECK(ready == 0);
    CHECK(controller.state() == Controller::State::idle);
    CHECK(backend.count("attach") == 0);
    CHECK(cancelled.contains(QStringLiteral("Attach cancelled")));
}

TEST_CASE("the debug session gate refuses when no target is attached", "[ui][debug_session]")
{
    slopkit::test::application();

    slopkit::tests::FakeDebugBackend backend;
    Controller                       controller {backend};

    AttachedTarget                detached; // invalid
    QWidget                       owner;
    slopkit::ui::DebugSessionGate gate {controller, detached, owner};

    int prompt_calls = 0;
    gate.set_attach_prompt(
        [&prompt_calls](const AttachedTarget&, const QString&)
        {
            ++prompt_calls;
            return true;
        });

    QString reported;
    bool    was_error = false;
    QObject::connect(&gate,
                     &DebugSessionGate::progress,
                     &gate,
                     [&reported, &was_error](const QString& text, bool error)
                     {
                         reported  = text;
                         was_error = error;
                     });

    int ready = 0;
    gate.with_session(QStringLiteral("find out what writes this address"),
                      [&ready]
                      {
                          ++ready;
                      });
    CHECK(prompt_calls == 0);
    CHECK(ready == 0);
    CHECK(was_error);
    CHECK(reported.contains(QStringLiteral("Attach to a target before")));
}

TEST_CASE("the debug session gate reports a failed attach and drops the queue", "[ui][debug_session]")
{
    slopkit::test::application();

    slopkit::tests::FakeDebugBackend backend;
    backend.attach_error = slopkit::process::AccessError::unsupported;
    Controller controller {backend};

    AttachedTarget                target = slopkit::test::fake_target();
    QWidget                       owner;
    slopkit::ui::DebugSessionGate gate {controller, target, owner};

    QString failed;
    QObject::connect(&gate,
                     &DebugSessionGate::progress,
                     &gate,
                     [&failed](const QString& text, bool error)
                     {
                         if (error)
                         {
                             failed = text;
                         }
                     });
    gate.set_attach_prompt(
        [](const AttachedTarget&, const QString&)
        {
            return true;
        });

    int ready = 0;
    gate.with_session(QStringLiteral("find out what writes this address"),
                      [&ready]
                      {
                          ++ready;
                      });
    REQUIRE(pump_debug(controller,
                       [&]
                       {
                           return !failed.isEmpty();
                       }));
    CHECK(ready == 0);
    CHECK(controller.state() == Controller::State::idle);
    CHECK(failed.contains(QStringLiteral("could not attach")));
}

TEST_CASE("the debug session gate dispatches queued work in arrival order", "[ui][debug_session]")
{
    slopkit::test::application();

    slopkit::tests::FakeDebugBackend backend;
    backend.block_continue = true;
    Controller controller {backend};

    AttachedTarget                target = slopkit::test::fake_target();
    QWidget                       owner;
    slopkit::ui::DebugSessionGate gate {controller, target, owner};

    int prompt_calls = 0;
    gate.set_attach_prompt(
        [&prompt_calls](const AttachedTarget&, const QString&)
        {
            ++prompt_calls;
            return true;
        });

    std::vector<int> order;
    gate.with_session(QStringLiteral("find out what writes this address"),
                      [&order]
                      {
                          order.push_back(1);
                      });
    gate.with_session(QStringLiteral("find out what accesses this address"),
                      [&order]
                      {
                          order.push_back(2);
                      });

    // The second command rides the attach the first one already started.
    CHECK(prompt_calls == 1);
    CHECK(controller.state() == Controller::State::starting);

    REQUIRE(pump_debug(controller,
                       [&]
                       {
                           return order.size() == 2;
                       }));
    CHECK(order == std::vector<int> {1, 2});
    CHECK(backend.count("attach") == 1);
}
