#include "plugins/support/session.hpp"

#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

namespace slopkit::plugins::support
{
    Session::Session(process::ProcessId process_id, platform::ForeignSignalPolicy policy)
        : pid(process_id), mem(process_id), debug(process_id, mem, policy)
    {
    }

    namespace
    {
        std::mutex& sessions_mutex()
        {
            static std::mutex value;
            return value;
        }

        std::unordered_set<Session*>& sessions()
        {
            static std::unordered_set<Session*> value;
            return value;
        }

        std::deque<std::string>& string_arena()
        {
            thread_local std::deque<std::string> value;
            return value;
        }
    } // namespace

    void register_session(Session* session)
    {
        const std::lock_guard lock(sessions_mutex());
        sessions().insert(session);
    }

    void unregister_session(Session* session)
    {
        const std::lock_guard lock(sessions_mutex());
        sessions().erase(session);
    }

    Session* lookup_session(void* handle)
    {
        auto* session = static_cast<Session*>(handle);
        if (session == nullptr)
        {
            return nullptr;
        }
        const std::lock_guard lock(sessions_mutex());
        return sessions().contains(session) ? session : nullptr;
    }

    void reset_arena()
    {
        string_arena().clear();
    }

    const char* intern(std::string value)
    {
        string_arena().push_back(std::move(value));
        return string_arena().back().c_str();
    }

} // namespace slopkit::plugins::support
