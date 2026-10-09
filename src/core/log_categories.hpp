#pragma once

#include <string_view>

namespace slopkit::log::category
{
    inline constexpr std::string_view app {"app"};         // CLI, startup/shutdown, settings
    inline constexpr std::string_view plugin {"plugin"};   // discovery, load, plugin messages
    inline constexpr std::string_view process {"process"}; // inventory, probe, attach, worker jobs
    inline constexpr std::string_view memory {"memory"};   // reads, writes, page loads
    inline constexpr std::string_view scan {"scan"};       // engine and matcher lifecycle
    inline constexpr std::string_view table {"table"};     // address table and its files
    inline constexpr std::string_view script {"script"};   // running a Lua script entry and its output
    inline constexpr std::string_view debug {"debug"};     // debug session lifecycle, stops and failures
    inline constexpr std::string_view ui {"ui"};           // user actions in the Qt layer
} // namespace slopkit::log::category
