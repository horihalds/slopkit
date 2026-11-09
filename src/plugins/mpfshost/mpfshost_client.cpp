#include "plugins/mpfshost/mpfshost_client.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/ipc/frame.hpp"
#include "plugin/plugin_api.h"
#include "plugins/mpfshost/mpfshost_client_impl.hpp"

namespace slopkit::plugins::mpfshost
{
    namespace detail
    {
        void Writer::u8(std::uint8_t value)
        {
            buffer_.push_back(static_cast<std::byte>(value));
        }

        void Writer::u16(std::uint16_t value)
        {
            for (int index = 0; index < 2; ++index)
            {
                u8(static_cast<std::uint8_t>(value >> (8 * index)));
            }
        }

        void Writer::u32(std::uint32_t value)
        {
            for (int index = 0; index < 4; ++index)
            {
                u8(static_cast<std::uint8_t>(value >> (8 * index)));
            }
        }

        void Writer::u64(std::uint64_t value)
        {
            for (int index = 0; index < 8; ++index)
            {
                u8(static_cast<std::uint8_t>(value >> (8 * index)));
            }
        }

        void Writer::string(std::string_view value)
        {
            u32(static_cast<std::uint32_t>(value.size()));
            for (const auto character : value)
            {
                u8(static_cast<std::uint8_t>(character));
            }
        }

        void Writer::bytes(std::span<const std::byte> value)
        {
            u32(static_cast<std::uint32_t>(value.size()));
            buffer_.insert(buffer_.end(), value.begin(), value.end());
        }

        std::uint8_t Reader::u8()
        {
            if (offset_ + 1 > data_.size())
            {
                failed_ = true;
                return 0;
            }
            return std::to_integer<std::uint8_t>(data_[offset_++]);
        }

        std::uint16_t Reader::u16()
        {
            std::uint16_t value = 0;
            for (int index = 0; index < 2; ++index)
            {
                value |= static_cast<std::uint16_t>(u8()) << (8 * index);
            }
            return value;
        }

        std::uint32_t Reader::u32()
        {
            std::uint32_t value = 0;
            for (int index = 0; index < 4; ++index)
            {
                value |= static_cast<std::uint32_t>(u8()) << (8 * index);
            }
            return value;
        }

        std::uint64_t Reader::u64()
        {
            std::uint64_t value = 0;
            for (int index = 0; index < 8; ++index)
            {
                value |= static_cast<std::uint64_t>(u8()) << (8 * index);
            }
            return value;
        }

        std::string Reader::string()
        {
            const auto length = u32();
            if (failed_ || offset_ + length > data_.size())
            {
                failed_ = true;
                return {};
            }
            std::string value {reinterpret_cast<const char*>(data_.data() + offset_), length};
            offset_ += length;
            return value;
        }

        std::vector<std::byte> Reader::bytes()
        {
            const auto length = u32();
            if (failed_ || offset_ + length > data_.size())
            {
                failed_ = true;
                return {};
            }
            std::vector<std::byte> value(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
                                         data_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
            offset_ += length;
            return value;
        }

        TransportError make_error(std::int32_t code, std::string message)
        {
            return TransportError {code, std::move(message)};
        }

        std::int32_t status_for_code(std::uint16_t code) noexcept
        {
            switch (static_cast<::mpfshost::ErrorCode>(code))
            {
            case ::mpfshost::ErrorCode::AccessDenied:
                return SLOPKIT_ERR_PERMISSION_DENIED;
            case ::mpfshost::ErrorCode::TargetNotFound:
            case ::mpfshost::ErrorCode::TargetUnavailable:
            case ::mpfshost::ErrorCode::NotFound:
                return SLOPKIT_ERR_NOT_FOUND;
            case ::mpfshost::ErrorCode::CapabilityUnavailable:
                return SLOPKIT_ERR_UNSUPPORTED;
            case ::mpfshost::ErrorCode::MalformedPayload:
            case ::mpfshost::ErrorCode::UnknownMessage:
            case ::mpfshost::ErrorCode::InvalidRange:
            case ::mpfshost::ErrorCode::ResultTooLarge:
            case ::mpfshost::ErrorCode::UnknownVm:
                return SLOPKIT_ERR_INVALID_ARGUMENT;
            default:
                return SLOPKIT_ERR_IO;
            }
        }

        std::int32_t status_for_errno(int error) noexcept
        {
            switch (error)
            {
            case EACCES:
            case EPERM:
                return SLOPKIT_ERR_PERMISSION_DENIED;
            case ENOENT:
            case ECONNREFUSED:
            case ENOTSOCK:
                return SLOPKIT_ERR_NOT_FOUND;
            default:
                return SLOPKIT_ERR_IO;
            }
        }

        std::expected<Connection, TransportError> Connection::open(std::string_view path)
        {
            const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
            {
                return std::unexpected(
                    make_error(status_for_errno(errno), std::format("socket: {}", std::strerror(errno))));
            }
            Connection connection {fd};

            timeval timeout {};
            timeout.tv_sec = 60;
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

            sockaddr_un address {};
            address.sun_family = AF_UNIX;
            if (path.size() >= sizeof(address.sun_path))
            {
                return std::unexpected(make_error(SLOPKIT_ERR_INVALID_ARGUMENT, "the socket path is too long"));
            }
            std::memcpy(address.sun_path, path.data(), path.size());

            if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            {
                return std::unexpected(
                    make_error(status_for_errno(errno), std::format("connect failed: {}", std::strerror(errno))));
            }
            return connection;
        }

        Connection::Connection(Connection&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

        Connection& Connection::operator=(Connection&& other) noexcept
        {
            if (this != &other)
            {
                close();
                fd_ = std::exchange(other.fd_, -1);
            }
            return *this;
        }

        Connection::~Connection()
        {
            close();
        }

        void Connection::close() noexcept
        {
            if (fd_ >= 0)
            {
                ::close(fd_);
                fd_ = -1;
            }
        }

        std::expected<void, TransportError>
        Connection::send(::mpfshost::MessageType type, std::uint64_t request_id, std::span<const std::byte> payload)
        {
            Writer header;
            header.u32(::mpfshost::kProtocolMagic);
            header.u16(::mpfshost::kProtocolVersion);
            header.u16(static_cast<std::uint16_t>(type));
            header.u64(request_id);
            header.u32(static_cast<std::uint32_t>(payload.size()));

            std::vector<std::byte> frame = header.buffer();
            frame.insert(frame.end(), payload.begin(), payload.end());

            std::size_t offset = 0;
            while (offset < frame.size())
            {
                const auto written = ::write(fd_, frame.data() + offset, frame.size() - offset);
                if (written < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    return std::unexpected(
                        make_error(SLOPKIT_ERR_IO, std::format("write failed: {}", std::strerror(errno))));
                }
                if (written == 0)
                {
                    return std::unexpected(make_error(SLOPKIT_ERR_IO, "connection closed while writing"));
                }
                offset += static_cast<std::size_t>(written);
            }
            return {};
        }

        std::expected<void, TransportError> Connection::read_exact(std::span<std::byte> buffer)
        {
            std::size_t offset = 0;
            while (offset < buffer.size())
            {
                const auto count = ::read(fd_, buffer.data() + offset, buffer.size() - offset);
                if (count < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    return std::unexpected(
                        make_error(SLOPKIT_ERR_IO, std::format("read failed: {}", std::strerror(errno))));
                }
                if (count == 0)
                {
                    return std::unexpected(make_error(SLOPKIT_ERR_IO, "connection closed while reading"));
                }
                offset += static_cast<std::size_t>(count);
            }
            return {};
        }

        std::expected<Frame, TransportError> Connection::receive()
        {
            std::array<std::byte, ::mpfshost::kFrameHeaderSize> header {};
            if (const auto read = read_exact(header); !read)
            {
                return std::unexpected(read.error());
            }

            Reader     reader {header};
            const auto magic      = reader.u32();
            const auto version    = reader.u16();
            const auto type       = reader.u16();
            const auto request_id = reader.u64();
            const auto length     = reader.u32();
            if (!reader.ok() || magic != ::mpfshost::kProtocolMagic)
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed frame header"));
            }
            if (version != ::mpfshost::kProtocolVersion)
            {
                return std::unexpected(
                    make_error(SLOPKIT_ERR_INVALID_ABI, "the host speaks an unsupported protocol version"));
            }
            if (length > ::mpfshost::kMaxPayloadLength)
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent an oversized frame"));
            }

            Frame frame;
            frame.type       = static_cast<::mpfshost::MessageType>(type);
            frame.request_id = request_id;
            frame.payload.resize(length);
            if (length != 0)
            {
                if (const auto read = read_exact(frame.payload); !read)
                {
                    return std::unexpected(read.error());
                }
            }
            return frame;
        }

        std::expected<void, TransportError> expect_type(const Frame& frame, ::mpfshost::MessageType expected)
        {
            if (frame.type == ::mpfshost::MessageType::ErrorResponse)
            {
                Reader     reader {frame.payload};
                const auto code = reader.u16();
                auto       text = reader.string();
                if (!reader.ok() || !reader.at_end())
                {
                    return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed error"));
                }
                if (text.empty())
                {
                    text = "the host refused the request";
                }
                return std::unexpected(make_error(status_for_code(code), std::move(text)));
            }
            if (frame.type != expected)
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent an unexpected response"));
            }
            return {};
        }

        std::expected<void, TransportError>
        bind(Connection& connection, std::string_view target, std::uint64_t request_id)
        {
            Writer request;
            request.string(target);
            if (const auto sent = connection.send(::mpfshost::MessageType::BindRequest, request_id, request.buffer());
                !sent)
            {
                return sent;
            }

            auto response = connection.receive();
            if (!response)
            {
                return std::unexpected(response.error());
            }
            return expect_type(*response, ::mpfshost::MessageType::BindResponse);
        }

        ClientSessionImpl::ClientSessionImpl(std::string name, Connection connection_, std::uint64_t id)
            : target_name(std::move(name)), connection(std::move(connection_)), next_id(id)
        {
        }

        std::expected<Frame, TransportError> ClientSessionImpl::call(::mpfshost::MessageType    type,
                                                                     std::span<const std::byte> payload) const
        {
            const auto request_id = next_id++;
            if (const auto sent = connection.send(type, request_id, payload); !sent)
            {
                return std::unexpected(sent.error());
            }
            auto frame = connection.receive();
            if (!frame)
            {
                return std::unexpected(frame.error());
            }
            if (frame->request_id != request_id)
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host answered a different request"));
            }
            return frame;
        }

        ::mpfshost::DebugStatus read_debug_status(Reader& reader)
        {
            ::mpfshost::DebugStatus status;
            status.runState     = static_cast<::mpfshost::DebugRunState>(reader.u8());
            status.stubActive   = reader.u8() != 0;
            status.hostPaused   = reader.u8() != 0;
            status.vcpuCount    = reader.u32();
            status.watchCount   = reader.u32();
            status.stubOwned    = reader.u8() != 0;
            status.foreignStub  = reader.u8() != 0;
            status.architecture = reader.string();
            status.watchAccess  = static_cast<::mpfshost::DebugWatchAccess>(reader.u8());
            return status;
        }

        void write_debug_record(Writer& writer, const ::mpfshost::DebugRegisterRecord& record)
        {
            writer.u32(record.vcpu);
            writer.string(record.threadId);
            writer.u64(record.rip);
            writer.u64(record.rsp);
            writer.u64(record.rbp);
            writer.u64(record.eflags);
            writer.u64(record.cr0);
            writer.u64(record.cr3);
            writer.u64(record.cr4);
            writer.u64(record.cr8);
            writer.u64(record.rax);
            writer.u64(record.rbx);
            writer.u64(record.rcx);
            writer.u64(record.rdx);
            writer.u64(record.rsi);
            writer.u64(record.rdi);
            writer.u64(record.r8);
            writer.u64(record.r9);
            writer.u64(record.r10);
            writer.u64(record.r11);
            writer.u64(record.r12);
            writer.u64(record.r13);
            writer.u64(record.r14);
            writer.u64(record.r15);
            writer.u32(record.guestPid);
            writer.u32(record.guestTid);
            writer.u8(record.processFound ? 1 : 0);
            writer.u8(record.threadFound ? 1 : 0);
        }

        ::mpfshost::DebugRegisterRecord read_debug_record(Reader& reader)
        {
            ::mpfshost::DebugRegisterRecord record;
            record.vcpu         = reader.u32();
            record.threadId     = reader.string();
            record.rip          = reader.u64();
            record.rsp          = reader.u64();
            record.rbp          = reader.u64();
            record.eflags       = reader.u64();
            record.cr0          = reader.u64();
            record.cr3          = reader.u64();
            record.cr4          = reader.u64();
            record.cr8          = reader.u64();
            record.rax          = reader.u64();
            record.rbx          = reader.u64();
            record.rcx          = reader.u64();
            record.rdx          = reader.u64();
            record.rsi          = reader.u64();
            record.rdi          = reader.u64();
            record.r8           = reader.u64();
            record.r9           = reader.u64();
            record.r10          = reader.u64();
            record.r11          = reader.u64();
            record.r12          = reader.u64();
            record.r13          = reader.u64();
            record.r14          = reader.u64();
            record.r15          = reader.u64();
            record.guestPid     = reader.u32();
            record.guestTid     = reader.u32();
            record.processFound = reader.u8() != 0;
            record.threadFound  = reader.u8() != 0;
            return record;
        }
    }

    bool Capabilities::supports(::mpfshost::MessageType type) const noexcept
    {
        return std::ranges::find(request_types, static_cast<std::uint16_t>(type)) != request_types.end();
    }

    Client::Client(std::string socket_path) : socket_path_(std::move(socket_path)) {}

    std::expected<Capabilities, TransportError> Client::capabilities() const
    {
        using namespace detail;
        auto connection = Connection::open(socket_path_);
        if (!connection)
        {
            return std::unexpected(connection.error());
        }

        if (const auto sent = connection->send(::mpfshost::MessageType::CapabilitiesRequest, 1, {}); !sent)
        {
            return std::unexpected(sent.error());
        }
        auto frame = connection->receive();
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        if (const auto expected = expect_type(*frame, ::mpfshost::MessageType::CapabilitiesResponse); !expected)
        {
            return std::unexpected(expected.error());
        }

        Reader       reader {frame->payload};
        Capabilities capabilities;
        capabilities.server_version = reader.string();
        const auto count            = reader.u32();
        if (!reader.ok() || count > frame->payload.size())
        {
            return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent malformed capabilities"));
        }
        capabilities.request_types.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            capabilities.request_types.push_back(reader.u16());
        }
        if (!reader.ok() || !reader.at_end())
        {
            return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent malformed capabilities"));
        }
        return capabilities;
    }

    std::expected<std::vector<::mpfshost::TargetDescriptor>, TransportError> Client::targets() const
    {
        using namespace detail;
        auto connection = Connection::open(socket_path_);
        if (!connection)
        {
            return std::unexpected(connection.error());
        }

        if (const auto sent = connection->send(::mpfshost::MessageType::ListTargetsRequest, 1, {}); !sent)
        {
            return std::unexpected(sent.error());
        }
        auto frame = connection->receive();
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        if (const auto expected = expect_type(*frame, ::mpfshost::MessageType::ListTargetsResponse); !expected)
        {
            return std::unexpected(expected.error());
        }

        Reader     reader {frame->payload};
        const auto count = reader.u32();
        if (!reader.ok() || count > frame->payload.size())
        {
            return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed target list"));
        }

        std::vector<::mpfshost::TargetDescriptor> targets;
        targets.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            ::mpfshost::TargetDescriptor target;
            target.name          = reader.string();
            target.qemuPid       = reader.u32();
            target.qmpSocketPath = reader.string();
            targets.push_back(std::move(target));
        }
        if (!reader.ok() || !reader.at_end())
        {
            return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed target list"));
        }
        return targets;
    }

    std::expected<ClientSession, TransportError> Client::bind(std::string_view target) const
    {
        using namespace detail;
        auto connection = Connection::open(socket_path_);
        if (!connection)
        {
            return std::unexpected(connection.error());
        }
        if (const auto bound = detail::bind(*connection, target, 1); !bound)
        {
            return std::unexpected(bound.error());
        }

        std::string name {target};
        return ClientSession {std::make_unique<ClientSessionImpl>(std::move(name), std::move(*connection), 2)};
    }

    ClientSession::ClientSession(std::unique_ptr<detail::ClientSessionImpl> impl) : impl_(std::move(impl)) {}

    ClientSession::ClientSession(ClientSession&& other) noexcept            = default;
    ClientSession& ClientSession::operator=(ClientSession&& other) noexcept = default;
    ClientSession::~ClientSession()                                         = default;

    const std::string& ClientSession::target_name() const noexcept
    {
        return impl_->target_name;
    }

    std::expected<std::vector<::mpfshost::ProcessDescriptor>, TransportError>
    ClientSession::processes(const ::mpfshost::ProcessFilter& filter) const
    {
        using namespace detail;
        Writer request;
        request.u32(filter.pid);
        request.string(filter.nameFilter);

        auto frame = impl_->call(::mpfshost::MessageType::ProcessListRequest, request.buffer());
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        if (const auto expected = expect_type(*frame, ::mpfshost::MessageType::ProcessListResponse); !expected)
        {
            return std::unexpected(expected.error());
        }

        Reader     reader {frame->payload};
        const auto count = reader.u32();
        if (!reader.ok() || count > frame->payload.size())
        {
            return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed process list"));
        }

        std::vector<::mpfshost::ProcessDescriptor> processes;
        processes.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            ::mpfshost::ProcessDescriptor process;
            process.pid       = reader.u32();
            process.parentPid = reader.u32();
            process.wow64     = reader.u8() != 0;
            process.name      = reader.string();
            processes.push_back(std::move(process));
        }
        if (!reader.ok() || !reader.at_end())
        {
            return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed process list"));
        }
        return processes;
    }

    std::expected<std::vector<::mpfshost::ModuleDescriptor>, TransportError>
    ClientSession::modules(std::uint32_t pid) const
    {
        using namespace detail;
        Writer request;
        request.u32(pid);
        request.u16(0);
        request.string({});

        auto frame = impl_->call(::mpfshost::MessageType::ModuleListRequest, request.buffer());
        if (!frame)
        {
            return std::unexpected(frame.error());
        }

        std::vector<::mpfshost::ModuleDescriptor> modules;
        for (;;)
        {
            if (frame->type == ::mpfshost::MessageType::ErrorResponse)
            {
                const auto expected = expect_type(*frame, ::mpfshost::MessageType::ModuleListEnd);
                return std::unexpected(expected.error());
            }
            if (frame->type == ::mpfshost::MessageType::ModuleListEnd)
            {
                Reader     reader {frame->payload};
                const auto status = reader.u8();
                (void)reader.u64();
                const auto error = reader.string();
                if (!reader.ok() || !reader.at_end())
                {
                    return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed module list"));
                }
                if (status != 0)
                {
                    return std::unexpected(
                        make_error(SLOPKIT_ERR_IO, error.empty() ? "the host could not list the modules" : error));
                }
                return modules;
            }
            if (frame->type != ::mpfshost::MessageType::ModuleListData)
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent an unexpected module frame"));
            }

            Reader     reader {frame->payload};
            const auto count = reader.u32();
            if (!reader.ok() || count > frame->payload.size())
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed module list"));
            }
            for (std::uint32_t index = 0; index < count; ++index)
            {
                ::mpfshost::ModuleDescriptor module;
                module.base         = reader.u64();
                module.entry        = reader.u64();
                module.imageSize    = reader.u32();
                module.rawFileSize  = reader.u32();
                module.kind         = reader.u16();
                module.sectionCount = reader.u16();
                module.eatCount     = reader.u32();
                module.iatCount     = reader.u32();
                module.wow64        = reader.u8() != 0;
                module.name         = reader.string();
                module.fullName     = reader.string();
                module.pdbGuid      = reader.string();
                module.pdbName      = reader.string();
                module.version      = reader.string();
                modules.push_back(std::move(module));
            }
            if (!reader.ok() || !reader.at_end())
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed module list"));
            }

            frame = impl_->connection.receive();
            if (!frame)
            {
                return std::unexpected(frame.error());
            }
        }
    }

    std::expected<std::vector<::mpfshost::PteDescriptor>, TransportError> ClientSession::pte(std::uint32_t pid) const
    {
        using namespace detail;
        Writer request;
        request.u32(pid);
        request.u8(0);
        request.u64(0);
        request.u64(0);
        request.string({});

        auto frame = impl_->call(::mpfshost::MessageType::PteListRequest, request.buffer());
        if (!frame)
        {
            return std::unexpected(frame.error());
        }

        std::vector<::mpfshost::PteDescriptor> entries;
        for (;;)
        {
            if (frame->type == ::mpfshost::MessageType::ErrorResponse)
            {
                const auto expected = expect_type(*frame, ::mpfshost::MessageType::PteListEnd);
                return std::unexpected(expected.error());
            }
            if (frame->type == ::mpfshost::MessageType::PteListEnd)
            {
                Reader     reader {frame->payload};
                const auto status = reader.u8();
                (void)reader.u64();
                (void)reader.u64();
                (void)reader.u8();
                const auto error = reader.string();
                if (!reader.ok() || !reader.at_end())
                {
                    return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed PTE list"));
                }
                if (status != 0)
                {
                    return std::unexpected(
                        make_error(SLOPKIT_ERR_IO, error.empty() ? "the host could not list the page tables" : error));
                }
                return entries;
            }
            if (frame->type != ::mpfshost::MessageType::PteListData)
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent an unexpected PTE frame"));
            }

            Reader     reader {frame->payload};
            const auto count = reader.u32();
            if (!reader.ok() || count > frame->payload.size())
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed PTE list"));
            }
            for (std::uint32_t index = 0; index < count; ++index)
            {
                ::mpfshost::PteDescriptor entry;
                entry.vaBase        = reader.u64();
                entry.pages         = reader.u64();
                entry.pageFlags     = reader.u64();
                entry.softwarePages = reader.u32();
                entry.wow64         = reader.u8();
                entry.text          = reader.string();
                entries.push_back(std::move(entry));
            }
            if (!reader.ok() || !reader.at_end())
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed PTE list"));
            }

            frame = impl_->connection.receive();
            if (!frame)
            {
                return std::unexpected(frame.error());
            }
        }
    }

    std::expected<std::vector<::mpfshost::ThreadDescriptor>, TransportError>
    ClientSession::threads(std::uint32_t pid) const
    {
        using namespace detail;
        Writer request;
        request.u32(pid);
        request.u32(0);
        request.u16(0);

        auto frame = impl_->call(::mpfshost::MessageType::ThreadListRequest, request.buffer());
        if (!frame)
        {
            return std::unexpected(frame.error());
        }

        std::vector<::mpfshost::ThreadDescriptor> threads;
        for (;;)
        {
            if (frame->type == ::mpfshost::MessageType::ErrorResponse)
            {
                const auto expected = expect_type(*frame, ::mpfshost::MessageType::ThreadListEnd);
                return std::unexpected(expected.error());
            }
            if (frame->type == ::mpfshost::MessageType::ThreadListEnd)
            {
                Reader     reader {frame->payload};
                const auto status = reader.u8();
                (void)reader.u64();
                const auto error = reader.string();
                if (!reader.ok() || !reader.at_end())
                {
                    return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed thread list"));
                }
                if (status != 0)
                {
                    return std::unexpected(
                        make_error(SLOPKIT_ERR_IO, error.empty() ? "the host could not list the threads" : error));
                }
                return threads;
            }
            if (frame->type != ::mpfshost::MessageType::ThreadListData)
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent an unexpected thread frame"));
            }

            Reader     reader {frame->payload};
            const auto count = reader.u32();
            if (!reader.ok() || count > frame->payload.size())
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed thread list"));
            }
            for (std::uint32_t index = 0; index < count; ++index)
            {
                ::mpfshost::ThreadDescriptor thread;
                thread.tid                = reader.u32();
                thread.pid                = reader.u32();
                thread.exitStatus         = reader.u32();
                thread.userTime           = reader.u32();
                thread.kernelTime         = reader.u32();
                thread.ethread            = reader.u64();
                thread.teb                = reader.u64();
                thread.startAddress       = reader.u64();
                thread.win32StartAddress  = reader.u64();
                thread.stackBaseUser      = reader.u64();
                thread.stackLimitUser     = reader.u64();
                thread.stackBaseKernel    = reader.u64();
                thread.stackLimitKernel   = reader.u64();
                thread.trapFrame          = reader.u64();
                thread.instructionPointer = reader.u64();
                thread.stackPointer       = reader.u64();
                thread.affinity           = reader.u64();
                thread.createTime         = reader.u64();
                thread.exitTime           = reader.u64();
                thread.impersonationToken = reader.u64();
                thread.state              = reader.u8();
                thread.running            = reader.u8() != 0;
                thread.priority           = reader.u8();
                thread.basePriority       = reader.u8();
                thread.suspendCount       = reader.u8();
                thread.waitReason         = reader.u8();
                thread.callstackStatus    = reader.u8();
                const auto frame_count    = reader.u32();
                if (!reader.ok() || frame_count > frame->payload.size())
                {
                    return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed thread list"));
                }
                for (std::uint32_t frame_index = 0; frame_index < frame_count; ++frame_index)
                {
                    ::mpfshost::ThreadFrame thread_frame;
                    thread_frame.index            = reader.u32();
                    thread_frame.returnAddress    = reader.u64();
                    thread_frame.stackPointer     = reader.u64();
                    thread_frame.frameBase        = reader.u64();
                    thread_frame.displacement     = reader.u32();
                    thread_frame.registersPresent = reader.u8() != 0;
                    thread_frame.module           = reader.string();
                    thread_frame.function         = reader.string();
                    thread.frames.push_back(std::move(thread_frame));
                }
                threads.push_back(std::move(thread));
            }
            if (!reader.ok() || !reader.at_end())
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed thread list"));
            }

            frame = impl_->connection.receive();
            if (!frame)
            {
                return std::unexpected(frame.error());
            }
        }
    }

    std::expected<std::vector<std::byte>, TransportError>
    ClientSession::read_memory(std::uint32_t pid, std::uint64_t address, std::uint32_t size) const
    {
        using namespace detail;
        Writer request;
        request.u32(pid);
        request.u64(address);
        request.u32(size);

        auto frame = impl_->call(::mpfshost::MessageType::MemoryReadRequest, request.buffer());
        if (!frame)
        {
            return std::unexpected(frame.error());
        }

        std::vector<std::byte> data;
        for (;;)
        {
            if (frame->type == ::mpfshost::MessageType::ErrorResponse)
            {
                const auto expected = expect_type(*frame, ::mpfshost::MessageType::MemoryReadEnd);
                return std::unexpected(expected.error());
            }
            if (frame->type == ::mpfshost::MessageType::MemoryReadEnd)
            {
                Reader     reader {frame->payload};
                const auto status    = reader.u8();
                const auto delivered = reader.u64();
                const auto error     = reader.string();
                if (!reader.ok() || !reader.at_end())
                {
                    return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed read end"));
                }
                if (status != 0 || !error.empty())
                {
                    return std::unexpected(
                        make_error(SLOPKIT_ERR_IO, error.empty() ? "the host could not read the range" : error));
                }
                if (delivered != data.size())
                {
                    return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host delivered an incomplete read"));
                }
                return data;
            }
            if (frame->type != ::mpfshost::MessageType::MemoryReadData)
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent an unexpected read frame"));
            }
            data.insert(data.end(), frame->payload.begin(), frame->payload.end());

            frame = impl_->connection.receive();
            if (!frame)
            {
                return std::unexpected(frame.error());
            }
        }
    }

    std::expected<std::uint32_t, TransportError>
    ClientSession::write_memory(std::uint32_t pid, std::uint64_t address, std::span<const std::byte> data) const
    {
        using namespace detail;
        Writer request;
        request.u32(pid);
        request.u64(address);
        request.bytes(data);

        auto frame = impl_->call(::mpfshost::MessageType::MemoryWriteRequest, request.buffer());
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        if (const auto expected = expect_type(*frame, ::mpfshost::MessageType::MemoryWriteResponse); !expected)
        {
            return std::unexpected(expected.error());
        }

        Reader     reader {frame->payload};
        const auto written = reader.u32();
        if (!reader.ok() || !reader.at_end())
        {
            return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed write response"));
        }
        return written;
    }
}
