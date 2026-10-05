#include <catch2/catch.hpp>

#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <thread>

#include <unistd.h>

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QString>

#include "app/instance.hpp"
#include "app/instance_server.hpp"

namespace
{
    // A QApplication may only exist once per process; Catch2 normally runs each
    // case in its own process, but the binary accepts several.
    void ensure_application()
    {
        if (QCoreApplication::instance() != nullptr)
        {
            return;
        }
        static int          argc      = 1;
        static char         program[] = "slopkit_tests";
        static char*        argv[]    = {program, nullptr};
        static QApplication instance(argc, argv);
    }

    // A private abstract-socket name per test, so cases never reach each other.
    std::string private_name(std::string_view suffix)
    {
        std::string name(1, '\0');
        name += std::format("slopkit-test-{}-{}", static_cast<unsigned long>(::getpid()), suffix);
        return name;
    }

    // Pumps Qt events until `done` holds or the timeout elapses.
    template<typename Predicate>
    bool pump_until(Predicate done, std::chrono::milliseconds timeout = std::chrono::milliseconds {2000})
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            if (done())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        return done();
    }

    TEST_CASE("a hand-off reaches a listening instance and is acknowledged", "[app]")
    {
        ensure_application();

        const std::string            name = private_name("ack");
        slopkit::app::InstanceServer server {name};
        REQUIRE(server.is_listening());

        QString received;
        bool    got = false;
        QObject::connect(&server,
                         &slopkit::app::InstanceServer::tableRequested,
                         [&received, &got](const QString& path)
                         {
                             received = path;
                             got      = true;
                         });

        const QString     path = QStringLiteral("/home/me/My Tables/save 1.skt");
        std::atomic<bool> handed {false};
        std::thread       client {[&]
                                  {
                                handed = slopkit::app::hand_off_open_request(path.toStdString(), name);
                                  }};

        CHECK(pump_until(
            [&]
            {
                return got;
            }));
        client.join();

        CHECK(handed.load());
        CHECK(got);
        CHECK(received == path);
    }

    TEST_CASE("a hand-off with nobody listening fails immediately", "[app]")
    {
        ensure_application();

        const auto start = std::chrono::steady_clock::now();
        CHECK_FALSE(slopkit::app::hand_off_open_request("/tmp/nobody.skt", private_name("nobody")));
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds {500});
    }

    TEST_CASE("a second server on the same name cannot listen", "[app]")
    {
        ensure_application();

        const std::string            name = private_name("taken");
        slopkit::app::InstanceServer first {name};
        REQUIRE(first.is_listening());

        slopkit::app::InstanceServer second {name};
        CHECK_FALSE(second.is_listening());
    }

    TEST_CASE("an over-long path is refused without a hand-off", "[app]")
    {
        ensure_application();

        const std::string            name = private_name("long");
        slopkit::app::InstanceServer server {name};
        REQUIRE(server.is_listening());

        int got = 0;
        QObject::connect(&server,
                         &slopkit::app::InstanceServer::tableRequested,
                         [&got](const QString&)
                         {
                             ++got;
                         });

        const std::string long_path(5000, 'x');
        CHECK_FALSE(slopkit::app::hand_off_open_request(long_path, name));

        // Let any stray activation run, then confirm nothing was delivered.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        CHECK(got == 0);
    }

    TEST_CASE("the instance socket name is uid-scoped", "[app]")
    {
        const std::string name = slopkit::app::instance_socket_name();
        REQUIRE(name.size() > 1);
        CHECK(name.front() == '\0');
        CHECK(name.find(std::to_string(static_cast<unsigned long>(::getuid()))) != std::string::npos);
    }

} // namespace
