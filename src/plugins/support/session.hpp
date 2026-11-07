#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

#include "platform/linux/debug_session.hpp"
#include "platform/linux/memory.hpp"
#include "process/types.hpp"

namespace slopkit::plugins::support
{
    struct Session
    {
        Session(process::ProcessId process_id, platform::ForeignSignalPolicy policy);
        ~Session();

        Session(const Session&)            = delete;
        Session& operator=(const Session&) = delete;

        process::ProcessId     pid {};
        platform::MemAccess    mem;
        platform::DebugSession debug;
        bool                   suspended {false};

        std::mutex                                     allocation_mutex;
        std::unordered_map<std::uint64_t, std::size_t> allocations;
        bool                                           allocating {false};
    };

    void                   register_session(Session* session);
    void                   unregister_session(Session* session);
    [[nodiscard]] Session* lookup_session(void* handle);

    void        reset_arena();
    const char* intern(std::string value);

}
