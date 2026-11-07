#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "trainer/payload.hpp"
#include "trainer/runtime/action.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/scripts.hpp"

namespace slopkit::trainer::gui
{
    using Perform = std::function<std::string(const Action&)>;
    using Step    = std::function<void()>;
    using Pid     = std::function<std::uint32_t()>;

    struct Options
    {
        std::string   title;
        std::uint32_t interval_ms {250};
    };

    // A display this platform can actually open; main.cpp uses it to choose
    // between the window and the printed rows. The Win32 implementation always
    // answers true.
    [[nodiscard]] bool window_available();

    // Builds the platform window, starts the session worker and runs the event
    // loop on the calling thread; returns the process exit code.
    [[nodiscard]] int run(const Payload&               payload,
                          std::span<const LiveEntry>   live,
                          std::span<const ScriptState> scripts,
                          Pid                          pid,
                          Options                      options,
                          Perform                      perform,
                          Step                         step);
}
