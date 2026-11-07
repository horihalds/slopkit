#pragma once

#include <cstdint>
#include <expected>
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

    class Client
    {
    public:
        explicit Client(std::string socket_path);

        [[nodiscard]] std::expected<std::vector<::mpfshost::TargetDescriptor>, TransportError> list_targets() const;
        [[nodiscard]] std::expected<std::vector<::mpfshost::ProcessDescriptor>, TransportError>
        list_processes(std::string_view target) const;
        [[nodiscard]] std::expected<std::vector<::mpfshost::ModuleDescriptor>, TransportError>
        list_modules(std::string_view target, std::uint32_t pid) const;
        [[nodiscard]] std::expected<std::vector<::mpfshost::PteDescriptor>, TransportError>
        list_pte(std::string_view target, std::uint32_t pid) const;
        [[nodiscard]] std::expected<std::vector<::mpfshost::ThreadDescriptor>, TransportError>
        list_threads(std::string_view target, std::uint32_t pid) const;
        [[nodiscard]] std::expected<std::vector<std::byte>, TransportError>
        read_memory(std::string_view target, std::uint32_t pid, std::uint64_t address, std::uint32_t size) const;
        [[nodiscard]] std::expected<std::uint32_t, TransportError> write_memory(std::string_view           target,
                                                                                std::uint32_t              pid,
                                                                                std::uint64_t              address,
                                                                                std::span<const std::byte> data) const;

    private:
        std::string socket_path_;
    };
}
