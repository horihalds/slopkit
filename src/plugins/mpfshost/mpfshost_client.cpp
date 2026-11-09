#include "plugins/mpfshost/mpfshost_client.hpp"

#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "plugin/plugin_api.h"
#include "plugins/mpfshost/mpfshost_helpers.hpp"

namespace slopkit::plugins::mpfshost
{
    GuestSessions::Lease::Lease(GuestSessions* owner, std::size_t slot, ::mpfshost::ClientSession* session) noexcept
        : owner_(owner), slot_(slot), session_(session)
    {
    }

    GuestSessions::Lease::Lease(Lease&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), slot_(other.slot_),
          session_(std::exchange(other.session_, nullptr)), invalidate_(std::exchange(other.invalidate_, false))
    {
    }

    GuestSessions::Lease& GuestSessions::Lease::operator=(Lease&& other) noexcept
    {
        if (this != &other)
        {
            release();
            owner_      = std::exchange(other.owner_, nullptr);
            slot_       = other.slot_;
            session_    = std::exchange(other.session_, nullptr);
            invalidate_ = std::exchange(other.invalidate_, false);
        }
        return *this;
    }

    GuestSessions::Lease::~Lease()
    {
        release();
    }

    void GuestSessions::Lease::release() noexcept
    {
        if (owner_ != nullptr)
        {
            owner_->release(slot_, invalidate_);
            owner_   = nullptr;
            session_ = nullptr;
        }
    }

    GuestSessions::GuestSessions(std::string target) : target_(std::move(target)) {}

    std::unique_ptr<::mpfshost::ClientSession> GuestSessions::connect(std::string& error)
    {
        const ::mpfshost::Client client {socket_path()};
        auto                     bound = client.bind(target_);
        if (!bound)
        {
            error = std::format("{}: {}", ::mpfshost::errorCodeName(bound.error().code), bound.error().message);
            return nullptr;
        }
        return std::make_unique<::mpfshost::ClientSession>(std::move(*bound));
    }

    std::expected<GuestSessions::Lease, std::string> GuestSessions::acquire()
    {
        std::unique_lock lock {mutex_};
        for (;;)
        {
            for (std::size_t index = 0; index < slots_.size(); ++index)
            {
                if (!slots_[index].busy)
                {
                    if (!slots_[index].session)
                    {
                        std::string error;
                        auto        session = connect(error);
                        if (!session)
                        {
                            return std::unexpected(std::move(error));
                        }
                        slots_[index].session = std::move(session);
                    }
                    slots_[index].busy = true;
                    return Lease {this, index, slots_[index].session.get()};
                }
            }

            if (slots_.size() < kMaxSessions)
            {
                std::string error;
                auto        session = connect(error);
                if (!session)
                {
                    return std::unexpected(std::move(error));
                }
                slots_.push_back(Slot {std::move(session), true});
                return Lease {this, slots_.size() - 1, slots_.back().session.get()};
            }

            available_.wait(lock);
        }
    }

    void GuestSessions::release(std::size_t slot, bool invalidate) noexcept
    {
        {
            const std::lock_guard<std::mutex> lock {mutex_};
            if (slot >= slots_.size())
            {
                return;
            }
            if (invalidate)
            {
                slots_[slot].session.reset();
            }
            slots_[slot].busy = false;
        }
        available_.notify_one();
    }

    std::shared_ptr<GuestSessions> sessions_for(std::string_view target)
    {
        static std::mutex                                                      mutex;
        static std::unordered_map<std::string, std::shared_ptr<GuestSessions>> pools;

        const std::string                 key {target};
        const std::lock_guard<std::mutex> lock {mutex};
        const auto                        found = pools.find(key);
        if (found != pools.end())
        {
            return found->second;
        }
        auto pool = std::make_shared<GuestSessions>(key);
        pools.emplace(key, pool);
        return pool;
    }

    std::int32_t status_for_error(const ::mpfshost::Error& error) noexcept
    {
        using ::mpfshost::ErrorCode;
        switch (error.code)
        {
        case ErrorCode::AccessDenied:
            return SLOPKIT_ERR_PERMISSION_DENIED;
        case ErrorCode::TargetNotFound:
        case ErrorCode::TargetUnavailable:
        case ErrorCode::NotFound:
            return SLOPKIT_ERR_NOT_FOUND;
        case ErrorCode::CapabilityUnavailable:
            return SLOPKIT_ERR_UNSUPPORTED;
        case ErrorCode::MalformedPayload:
        case ErrorCode::UnknownMessage:
        case ErrorCode::InvalidRange:
        case ErrorCode::ResultTooLarge:
        case ErrorCode::UnknownVm:
        case ErrorCode::InvalidState:
            return SLOPKIT_ERR_INVALID_ARGUMENT;
        case ErrorCode::ConnectionFailed:
            return SLOPKIT_ERR_NOT_FOUND;
        case ErrorCode::InvalidArgument:
            return SLOPKIT_ERR_INVALID_ARGUMENT;
        case ErrorCode::ProtocolError:
        default:
            return SLOPKIT_ERR_IO;
        }
    }

    std::string message_for_error(const ::mpfshost::Error& error)
    {
        if (error.message.empty())
        {
            return std::string {::mpfshost::errorCodeName(error.code)};
        }
        return error.message;
    }
}
