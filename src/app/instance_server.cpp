#include "app/instance_server.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <format>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <QSocketNotifier>

#include "app/instance.hpp"
#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::app
{
    namespace
    {
        // Matches the client's cap in instance.cpp: a request longer than this is
        // refused without an acknowledgement.
        constexpr std::size_t kMaxRequestBytes = 4096;

        void set_non_blocking(int fd)
        {
            const int flags = ::fcntl(fd, F_GETFL);
            if (flags >= 0)
            {
                static_cast<void>(::fcntl(fd, F_SETFL, flags | O_NONBLOCK));
            }
        }
    } // namespace

    InstanceServer::InstanceServer(QObject* parent) : InstanceServer(instance_socket_name(), parent) {}

    InstanceServer::InstanceServer(std::string socket_name, QObject* parent) : QObject(parent)
    {
        if (socket_name.size() >= sizeof(sockaddr_un::sun_path))
        {
            return;
        }

        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0)
        {
            log::warning(log::category::app, "could not create the single-instance socket");
            listen_fd_ = -1;
            return;
        }
        set_non_blocking(listen_fd_);

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_name.data(), socket_name.size());
        const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socket_name.size());

        if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), length) != 0
            || ::listen(listen_fd_, 16) != 0)
        {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        listen_notifier_ = new QSocketNotifier(listen_fd_, QSocketNotifier::Read, this);
        connect(listen_notifier_,
                &QSocketNotifier::activated,
                this,
                [this](QSocketDescriptor, QSocketNotifier::Type)
                {
                    on_ready_read();
                });
        log::debug(log::category::app, "single-instance socket listening");
    }

    InstanceServer::~InstanceServer()
    {
        for (auto& [fd, connection] : connections_)
        {
            delete connection->notifier;
            ::close(fd);
        }
        connections_.clear();

        delete listen_notifier_;
        listen_notifier_ = nullptr;
        if (listen_fd_ >= 0)
        {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    bool InstanceServer::is_listening() const noexcept
    {
        return listen_fd_ >= 0;
    }

    void InstanceServer::on_ready_read()
    {
        while (true)
        {
            const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return; // EAGAIN when drained, or a real error
            }

            auto connection      = std::make_unique<Connection>();
            connection->fd       = fd;
            auto* notifier       = new QSocketNotifier(fd, QSocketNotifier::Read, this);
            connection->notifier = notifier;
            connect(notifier,
                    &QSocketNotifier::activated,
                    this,
                    [this](QSocketDescriptor descriptor, QSocketNotifier::Type)
                    {
                        on_connection_ready(static_cast<int>(descriptor));
                    });
            connections_.emplace(fd, std::move(connection));
        }
    }

    void InstanceServer::on_connection_ready(int fd)
    {
        const auto found = connections_.find(fd);
        if (found == connections_.end())
        {
            return;
        }
        Connection& connection = *found->second;

        char buffer[512];
        while (true)
        {
            const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count > 0)
            {
                if (connection.buffer.size() + static_cast<std::size_t>(count) > kMaxRequestBytes)
                {
                    close_connection(fd); // no ack: the client falls back
                    return;
                }
                connection.buffer.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                break; // fully drained for now
            }
            close_connection(fd); // peer closed mid-line or a real error
            return;
        }

        const auto newline = connection.buffer.find('\n');
        if (newline == std::string::npos)
        {
            return; // wait for the rest of the line
        }

        const std::string request = connection.buffer.substr(0, newline);
        if (request.empty())
        {
            close_connection(fd);
            return;
        }

        static const std::string ack = "ok\n";
        static_cast<void>(::send(fd, ack.data(), ack.size(), MSG_NOSIGNAL));
        close_connection(fd);

        const QString path = QString::fromUtf8(request.data(), static_cast<qsizetype>(request.size()));
        emit          tableRequested(path);
    }

    void InstanceServer::close_connection(int fd)
    {
        const auto found = connections_.find(fd);
        if (found == connections_.end())
        {
            return;
        }

        auto connection = std::move(found->second);
        connections_.erase(found);

        if (connection->notifier != nullptr)
        {
            connection->notifier->deleteLater();
        }
        ::close(connection->fd);
    }

} // namespace slopkit::app
