#pragma once

#include <expected>
#include <string>

#include "trainer/payload.hpp"

namespace slopkit::trainer
{
    // Reads the trainer's own executable and pulls the appended payload out of it.
    [[nodiscard]] std::expected<Payload, std::string> load_own_payload();

}
