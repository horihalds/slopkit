#pragma once

// Shared by the Wine end-to-end tests: the loader a guest is started through and
// the mingw-built Windows probe they run inside it. Both need a working wine and
// a wine prefix, so every test that uses them is hidden and gated behind
// `SLOPKIT_WINE_E2E`.

#include <filesystem>
#include <system_error>

namespace
{
    // `/usr/bin/wine64` first: the tests run the 64-bit probe.
    std::filesystem::path find_wine()
    {
        for (const auto* candidate : {"/usr/bin/wine64", "/usr/bin/wine"})
        {
            std::error_code error;
            if (std::filesystem::exists(candidate, error))
            {
                return candidate;
            }
        }
        return {};
    }

    std::filesystem::path wine_probe()
    {
        return std::filesystem::path {SLOPKIT_AGENT_PROBE_DIR} / "slopkit-probe-x86_64.exe";
    }
}
