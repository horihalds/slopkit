#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace slopkit::app
{
    // The abstract-namespace socket name ("\0slopkit-<uid>") of this user's
    // instance. The uid keeps two users (or two dev-users) on one machine from
    // reaching each other.
    [[nodiscard]] std::string instance_socket_name();

    // Writes `path` + '\n' to the listening instance and waits for its "ok\n"
    // acknowledgement. Returns false when nobody listens, the write fails, the
    // line is too long or the ack times out; the caller must then start its own
    // instance. `socket_name` defaults to instance_socket_name() and exists so
    // tests can use a private name.
    [[nodiscard]] bool hand_off_open_request(std::string_view          path,
                                             std::string_view          socket_name = {},
                                             std::chrono::milliseconds ack_timeout = std::chrono::milliseconds {1000});

} // namespace slopkit::app
