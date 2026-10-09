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
    // One open plugin session: the target, its cached memory access and the
    // opt-in ptrace debug session. `policy` is the plugin's foreign-signal
    // policy (linux-proc suppresses a target's own signals, wine-proton
    // forwards them because Wine's machinery signals the target constantly).
    struct Session
    {
        Session(process::ProcessId process_id, platform::ForeignSignalPolicy policy);
        // Resumes a target this session stopped, so closing, detaching or
        // replacing a session never leaves the target frozen.
        ~Session();

        Session(const Session&)            = delete;
        Session& operator=(const Session&) = delete;

        process::ProcessId     pid {};
        platform::MemAccess    mem;
        platform::DebugSession debug;
        // True while this session has stopped the target itself; the destructor
        // resumes only a target slopkit stopped, never one the user did.
        bool                   suspended {false};

        // Allocation state, guarded by `allocation_mutex`. `allocations` maps a
        // mapping this session created to its page-rounded size, so free_memory
        // can unmap exactly it; `allocating` is set while a remote syscall is in
        // flight so a debug attach and an allocation never overlap.
        std::mutex                                     allocation_mutex;
        std::unordered_map<std::uint64_t, std::size_t> allocations;
        bool                                           allocating {false};
    };

    // The registry of open sessions, mutex-guarded because the host and the
    // debug worker can reach it from different threads. Every plugin shared
    // library instance owns its own registry: the loader dlopen's plugins with
    // RTLD_LOCAL, so the archive's statics are not shared between two plugins.
    void                   register_session(Session* session);
    void                   unregister_session(Session* session);
    [[nodiscard]] Session* lookup_session(void* handle);

    // The strings handed to the host stay valid until the next call on the same
    // host thread. The arena is per host thread because two host threads can
    // call into this plugin at once, and std::deque keeps references stable
    // across push_back.
    void        reset_arena();
    const char* intern(std::string value);

} // namespace slopkit::plugins::support
