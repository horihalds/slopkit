#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mpfshost/client.hpp>

namespace slopkit::plugins::mpfshost
{
    // The plugin keeps a small pool of bound sessions per VM target. A ClientSession is not thread-safe and the
    // host gives each connection a worker thread, so the pool is deliberately bounded: scan threads share a few
    // connections instead of opening one per read.
    class GuestSessions final
    {
    public:
        class Lease
        {
        public:
            Lease(GuestSessions* owner, std::size_t slot, ::mpfshost::ClientSession* session) noexcept;
            Lease(const Lease&)            = delete;
            Lease& operator=(const Lease&) = delete;
            Lease(Lease&& other) noexcept;
            Lease& operator=(Lease&& other) noexcept;
            ~Lease();

            [[nodiscard]] ::mpfshost::ClientSession& session() const noexcept
            {
                return *session_;
            }

            // Called when the connection itself failed, so the next borrower creates a fresh session.
            void invalidate() noexcept
            {
                invalidate_ = true;
            }

        private:
            void release() noexcept;

            GuestSessions*             owner_ {nullptr};
            std::size_t                slot_ {};
            ::mpfshost::ClientSession* session_ {nullptr};
            bool                       invalidate_ {};
        };

        explicit GuestSessions(std::string target);

        [[nodiscard]] std::expected<Lease, std::string> acquire();

    private:
        struct Slot
        {
            std::unique_ptr<::mpfshost::ClientSession> session;
            bool                                       busy {};
        };

        static constexpr std::size_t kMaxSessions = 4;

        [[nodiscard]] std::unique_ptr<::mpfshost::ClientSession> connect(std::string& error);
        void                                                     release(std::size_t slot, bool invalidate) noexcept;

        std::string             target_;
        std::mutex              mutex_;
        std::condition_variable available_;
        std::vector<Slot>       slots_;
    };

    [[nodiscard]] std::shared_ptr<GuestSessions> sessions_for(std::string_view target);

    [[nodiscard]] std::int32_t status_for_error(const ::mpfshost::Error& error) noexcept;
    [[nodiscard]] std::string  message_for_error(const ::mpfshost::Error& error);
}
