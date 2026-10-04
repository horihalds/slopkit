#include "app/instance.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <format>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace slopkit::app
{
    namespace
    {
        // A request line, including the path and the newline, is capped so a
        // runaway client cannot grow the server buffer without bound.
        constexpr std::size_t kMaxRequestBytes = 4096;

        bool write_all(int fd, std::string_view data)
        {
            std::size_t written = 0;
            while (written < data.size())
            {
                const ssize_t count = ::send(fd, data.data() + written, data.size() - written, MSG_NOSIGNAL);
                if (count < 0 && errno == EINTR)
                {
                    continue;
                }
                if (count <= 0)
                {
                    return false;
                }
                written += static_cast<std::size_t>(count);
            }
            return true;
        }

        bool read_ack(int fd, std::chrono::milliseconds timeout)
        {
            const auto  deadline = std::chrono::steady_clock::now() + timeout;
            std::string ack;
            while (ack.size() < 3)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                {
                    return false;
                }
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();

                pollfd    descriptor {.fd = fd, .events = POLLIN, .revents = 0};
                const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
                if (ready < 0 && errno == EINTR)
                {
                    continue;
                }
                if (ready <= 0)
                {
                    return false;
                }

                char          buffer[16];
                const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
                if (count <= 0)
                {
                    return false;
                }
                ack.append(buffer, static_cast<std::size_t>(count));
            }
            return ack.starts_with("ok\n");
        }
    } // namespace

    std::string instance_socket_name()
    {
        std::string name(1, '\0'); // Abstract namespace: no filesystem entry.
        name += std::format("slopkit-{}", static_cast<unsigned long>(::getuid()));
        return name;
    }

    bool
    hand_off_open_request(std::string_view path, std::string_view socket_name, std::chrono::milliseconds ack_timeout)
    {
        if (path.size() + 1 > kMaxRequestBytes)
        {
            return false;
        }

        const std::string name = socket_name.empty() ? instance_socket_name() : std::string(socket_name);
        if (name.size() >= sizeof(sockaddr_un::sun_path))
        {
            return false;
        }

        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
        {
            return false;
        }

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, name.data(), name.size());
        const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size());

        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), length) != 0)
        {
            ::close(fd);
            return false;
        }

        std::string request(path);
        request.push_back('\n');
        const bool acknowledged = write_all(fd, request) && read_ack(fd, ack_timeout);
        ::close(fd);
        return acknowledged;
    }

} // namespace slopkit::app
