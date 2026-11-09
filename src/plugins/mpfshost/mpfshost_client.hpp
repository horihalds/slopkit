#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/ipc/message.hpp"

namespace slopkit::plugins::mpfshost
{
    struct TransportError
    {
        std::int32_t code {0};
        std::string  message;
    };

    struct Capabilities
    {
        std::string                server_version;
        std::vector<std::uint16_t> request_types;

        [[nodiscard]] bool supports(::mpfshost::MessageType type) const noexcept;
    };

    namespace detail
    {
        struct ClientSessionImpl;
    }

    class ClientSession
    {
    public:
        ClientSession(ClientSession&& other) noexcept;
        ClientSession& operator=(ClientSession&& other) noexcept;
        ~ClientSession();

        ClientSession(const ClientSession&)            = delete;
        ClientSession& operator=(const ClientSession&) = delete;

        [[nodiscard]] const std::string& target_name() const noexcept;

        [[nodiscard]] std::expected<std::vector<::mpfshost::ProcessDescriptor>, TransportError>
        processes(const ::mpfshost::ProcessFilter& filter = {}) const;
        [[nodiscard]] std::expected<std::vector<::mpfshost::ModuleDescriptor>, TransportError>
        modules(std::uint32_t pid) const;
        [[nodiscard]] std::expected<std::vector<::mpfshost::PteDescriptor>, TransportError>
        pte(std::uint32_t pid) const;
        [[nodiscard]] std::expected<std::vector<::mpfshost::ThreadDescriptor>, TransportError>
        threads(std::uint32_t pid) const;
        [[nodiscard]] std::expected<std::vector<std::byte>, TransportError>
        read_memory(std::uint32_t pid, std::uint64_t address, std::uint32_t size) const;
        [[nodiscard]] std::expected<std::uint32_t, TransportError>
        write_memory(std::uint32_t pid, std::uint64_t address, std::span<const std::byte> data) const;

        [[nodiscard]] std::expected<::mpfshost::DebugStatus, TransportError> debug_state() const;
        [[nodiscard]] std::expected<::mpfshost::DebugStatus, TransportError>
        debug_control(::mpfshost::DebugControlAction action) const;
        [[nodiscard]] std::expected<::mpfshost::DebugAttachResponse, TransportError> debug_attach() const;
        [[nodiscard]] std::expected<::mpfshost::DebugDetachResponse, TransportError> debug_detach() const;
        [[nodiscard]] std::expected<std::vector<::mpfshost::DebugRegisterRecord>, TransportError>
        debug_registers(std::uint32_t vcpu = ::mpfshost::kDebugAllVcpus) const;
        [[nodiscard]] std::expected<std::vector<::mpfshost::DebugThreadRecord>, TransportError> debug_threads() const;
        [[nodiscard]] std::expected<::mpfshost::DebugStepResponse, TransportError>
        debug_step(std::uint32_t vcpu, std::string_view thread_id) const;
        [[nodiscard]] std::expected<::mpfshost::DebugBacktraceResponse, TransportError> debug_backtrace(
            std::uint32_t pid, std::uint32_t vcpu, std::string_view thread_id, std::uint32_t max_frames) const;
        [[nodiscard]] std::expected<::mpfshost::DebugBreakpointArmResponse, TransportError>
        debug_arm_breakpoint(std::uint32_t pid, std::uint64_t address, ::mpfshost::DebugBreakpointKind kind) const;
        [[nodiscard]] std::expected<::mpfshost::DebugWatchArmResponse, TransportError> debug_arm_watch(
            std::uint32_t pid, std::uint64_t address, std::uint32_t width, ::mpfshost::DebugWatchAccess access) const;
        [[nodiscard]] std::expected<::mpfshost::DebugResourceRemoveResponse, TransportError>
        debug_remove_breakpoint(std::uint32_t id) const;
        [[nodiscard]] std::expected<::mpfshost::DebugResourceRemoveResponse, TransportError>
                                                                             debug_remove_watch(std::uint32_t id) const;
        [[nodiscard]] std::expected<::mpfshost::DebugStatus, TransportError> debug_continue_armed() const;

        struct WaitResult
        {
            std::vector<::mpfshost::DebugWaitHit> hits;
            ::mpfshost::DebugWaitEnd              end;
        };

        [[nodiscard]] std::expected<WaitResult, TransportError> debug_wait(std::uint32_t timeout_ms,
                                                                           std::uint32_t max_hits) const;

    private:
        friend class Client;
        explicit ClientSession(std::unique_ptr<detail::ClientSessionImpl> impl);

        std::unique_ptr<detail::ClientSessionImpl> impl_;
    };

    class Client
    {
    public:
        explicit Client(std::string socket_path);

        [[nodiscard]] std::expected<Capabilities, TransportError>                              capabilities() const;
        [[nodiscard]] std::expected<std::vector<::mpfshost::TargetDescriptor>, TransportError> targets() const;
        [[nodiscard]] std::expected<ClientSession, TransportError> bind(std::string_view target) const;

        [[nodiscard]] const std::string& socket_path() const noexcept
        {
            return socket_path_;
        }

    private:
        std::string socket_path_;
    };
}
