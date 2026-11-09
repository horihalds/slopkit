#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/ipc/frame.hpp"
#include "core/ipc/message.hpp"
#include "plugins/mpfshost/mpfshost_client.hpp"

namespace slopkit::plugins::mpfshost::detail
{
    class Writer
    {
    public:
        void u8(std::uint8_t value);
        void u16(std::uint16_t value);
        void u32(std::uint32_t value);
        void u64(std::uint64_t value);
        void string(std::string_view value);
        void bytes(std::span<const std::byte> value);

        [[nodiscard]] const std::vector<std::byte>& buffer() const noexcept
        {
            return buffer_;
        }

    private:
        std::vector<std::byte> buffer_;
    };

    class Reader
    {
    public:
        explicit Reader(std::span<const std::byte> data) noexcept : data_(data) {}

        [[nodiscard]] std::uint8_t           u8();
        [[nodiscard]] std::uint16_t          u16();
        [[nodiscard]] std::uint32_t          u32();
        [[nodiscard]] std::uint64_t          u64();
        [[nodiscard]] std::string            string();
        [[nodiscard]] std::vector<std::byte> bytes();

        [[nodiscard]] bool ok() const noexcept
        {
            return !failed_;
        }

        [[nodiscard]] bool at_end() const noexcept
        {
            return offset_ == data_.size();
        }

    private:
        std::span<const std::byte> data_;
        std::size_t                offset_ {};
        bool                       failed_ {};
    };

    struct Frame
    {
        ::mpfshost::MessageType type {};
        std::uint64_t           request_id {};
        std::vector<std::byte>  payload;
    };

    class Connection
    {
    public:
        static std::expected<Connection, TransportError> open(std::string_view path);

        Connection(Connection&& other) noexcept;
        Connection& operator=(Connection&& other) noexcept;
        Connection(const Connection&)            = delete;
        Connection& operator=(const Connection&) = delete;
        ~Connection();

        [[nodiscard]] std::expected<void, TransportError>
        send(::mpfshost::MessageType type, std::uint64_t request_id, std::span<const std::byte> payload);
        [[nodiscard]] std::expected<Frame, TransportError> receive();

    private:
        explicit Connection(int fd) noexcept : fd_(fd) {}

        void                                              close() noexcept;
        [[nodiscard]] std::expected<void, TransportError> read_exact(std::span<std::byte> buffer);

        int fd_ {-1};
    };

    struct ClientSessionImpl
    {
        ClientSessionImpl(std::string target_name, Connection connection, std::uint64_t next_id);

        ClientSessionImpl(ClientSessionImpl&&) noexcept            = default;
        ClientSessionImpl& operator=(ClientSessionImpl&&) noexcept = default;
        ClientSessionImpl(const ClientSessionImpl&)                = delete;
        ClientSessionImpl& operator=(const ClientSessionImpl&)     = delete;

        [[nodiscard]] std::expected<Frame, TransportError> call(::mpfshost::MessageType    type,
                                                                std::span<const std::byte> payload) const;

        std::string           target_name;
        mutable Connection    connection;
        mutable std::uint64_t next_id {2};
    };

    [[nodiscard]] TransportError                      make_error(std::int32_t code, std::string message);
    [[nodiscard]] std::expected<void, TransportError> expect_type(const Frame& frame, ::mpfshost::MessageType expected);
    [[nodiscard]] std::expected<void, TransportError>
                               bind(Connection& connection, std::string_view target, std::uint64_t request_id);
    [[nodiscard]] std::int32_t status_for_code(std::uint16_t code) noexcept;

    void write_debug_record(Writer& writer, const ::mpfshost::DebugRegisterRecord& record);
    [[nodiscard]] ::mpfshost::DebugRegisterRecord read_debug_record(Reader& reader);
    [[nodiscard]] ::mpfshost::DebugStatus         read_debug_status(Reader& reader);
}
