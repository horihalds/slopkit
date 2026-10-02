#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "process/types.hpp"

namespace slopkit::process
{

    // Backend that owns a concrete attachment to a target. Implemented on top of
    // the plugin layer; the UI only ever sees Session.
    class SessionBackend
    {
    public:
        virtual ~SessionBackend() = default;

        [[nodiscard]] virtual ProcessId        pid() const noexcept                = 0;
        [[nodiscard]] virtual std::string_view plugin_id() const noexcept          = 0;
        [[nodiscard]] virtual AccessMethod     advertised_methods() const noexcept = 0;
        [[nodiscard]] virtual AccessMethod     last_method() const noexcept        = 0;

        virtual std::expected<std::vector<std::byte>, AccessError>  read(std::uint64_t address, std::size_t size) = 0;
        virtual std::expected<std::size_t, AccessError>             write(std::uint64_t              address,
                                                                          std::span<const std::byte> data)        = 0;
        virtual std::expected<std::vector<ModuleInfo>, AccessError> modules()                                     = 0;
        virtual std::expected<std::vector<ThreadInfo>, AccessError> threads()                                     = 0;
        virtual std::expected<std::vector<RegionInfo>, AccessError> regions()                                     = 0;
    };

    // Move-only handle to an attached process. All operations return
    // std::expected so failures can be surfaced instead of throwing.
    class Session
    {
    public:
        Session() = default;

        explicit Session(std::unique_ptr<SessionBackend> backend) : backend_(std::move(backend)) {}

        ~Session() = default;

        Session(Session&&) noexcept            = default;
        Session& operator=(Session&&) noexcept = default;
        Session(const Session&)                = delete;
        Session& operator=(const Session&)     = delete;

        [[nodiscard]] ProcessId pid() const noexcept
        {
            return backend_ ? backend_->pid() : 0;
        }

        [[nodiscard]] std::string_view plugin_id() const noexcept
        {
            return backend_ ? backend_->plugin_id() : std::string_view {};
        }

        [[nodiscard]] AccessMethod advertised_methods() const noexcept
        {
            return backend_ ? backend_->advertised_methods() : AccessMethod::none;
        }

        // Access primitive actually used for the most recent read/write.
        [[nodiscard]] AccessMethod last_method() const noexcept
        {
            return backend_ ? backend_->last_method() : AccessMethod::none;
        }

        std::expected<std::vector<std::byte>, AccessError> read(std::uint64_t address, std::size_t size)
        {
            if (!backend_)
            {
                return std::unexpected(AccessError::internal);
            }
            return backend_->read(address, size);
        }

        std::expected<std::size_t, AccessError> write(std::uint64_t address, std::span<const std::byte> data)
        {
            if (!backend_)
            {
                return std::unexpected(AccessError::internal);
            }
            return backend_->write(address, data);
        }

        std::expected<std::vector<ModuleInfo>, AccessError> modules()
        {
            if (!backend_)
            {
                return std::unexpected(AccessError::internal);
            }
            return backend_->modules();
        }

        std::expected<std::vector<ThreadInfo>, AccessError> threads()
        {
            if (!backend_)
            {
                return std::unexpected(AccessError::internal);
            }
            return backend_->threads();
        }

        std::expected<std::vector<RegionInfo>, AccessError> regions()
        {
            if (!backend_)
            {
                return std::unexpected(AccessError::internal);
            }
            return backend_->regions();
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return backend_ != nullptr;
        }

    private:
        std::unique_ptr<SessionBackend> backend_;
    };

    // The seam the UI depends on. The embedded implementation lives in
    // plugin_access; an out-of-process transport can replace it without any UI
    // change because nothing above this interface knows about dlopen or plugins.
    class ProcessAccess
    {
    public:
        virtual ~ProcessAccess() = default;

        virtual std::expected<std::vector<ProcessInfo>, AccessError> list_processes()                 = 0;
        virtual std::expected<Session, AccessError> attach(ProcessId pid, std::string_view plugin_id) = 0;
    };

} // namespace slopkit::process
