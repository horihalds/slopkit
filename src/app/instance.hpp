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

    [[nodiscard]] bool hand_off_open_request(std::string_view          path,
                                             std::string_view          socket_name = {},
                                             std::chrono::milliseconds ack_timeout = std::chrono::milliseconds {1000});

}
