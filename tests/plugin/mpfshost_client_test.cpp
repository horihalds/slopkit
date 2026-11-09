#ifdef SLOPKIT_HAVE_MPFSHOST

#include <catch2/catch.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/ipc/frame.hpp"
#include "core/ipc/message.hpp"
#include "plugins/mpfshost/mpfshost_client.hpp"

namespace
{
    class PayloadWriter
    {
    public:
        void u8(std::uint8_t value)
        {
            data.push_back(static_cast<std::byte>(value));
        }

        void u16(std::uint16_t value)
        {
            for (int index = 0; index < 2; ++index)
            {
                u8(static_cast<std::uint8_t>(value >> (8 * index)));
            }
        }

        void u32(std::uint32_t value)
        {
            for (int index = 0; index < 4; ++index)
            {
                u8(static_cast<std::uint8_t>(value >> (8 * index)));
            }
        }

        void u64(std::uint64_t value)
        {
            for (int index = 0; index < 8; ++index)
            {
                u8(static_cast<std::uint8_t>(value >> (8 * index)));
            }
        }

        void string(std::string_view value)
        {
            u32(static_cast<std::uint32_t>(value.size()));
            for (const auto character : value)
            {
                u8(static_cast<std::uint8_t>(character));
            }
        }

        std::vector<std::byte> data;
    };

    class PayloadReader
    {
    public:
        explicit PayloadReader(std::span<const std::byte> payload) : payload_(payload) {}

        std::uint8_t u8()
        {
            return offset_ < payload_.size() ? std::to_integer<std::uint8_t>(payload_[offset_++]) : 0;
        }

        std::uint16_t u16()
        {
            std::uint16_t value = 0;
            for (int index = 0; index < 2; ++index)
            {
                value |= static_cast<std::uint16_t>(u8()) << (8 * index);
            }
            return value;
        }

        std::uint32_t u32()
        {
            std::uint32_t value = 0;
            for (int index = 0; index < 4; ++index)
            {
                value |= static_cast<std::uint32_t>(u8()) << (8 * index);
            }
            return value;
        }

        std::uint64_t u64()
        {
            std::uint64_t value = 0;
            for (int index = 0; index < 8; ++index)
            {
                value |= static_cast<std::uint64_t>(u8()) << (8 * index);
            }
            return value;
        }

        std::string string()
        {
            const auto length = u32();
            if (offset_ + length > payload_.size())
            {
                return {};
            }
            std::string value {reinterpret_cast<const char*>(payload_.data() + offset_), length};
            offset_ += length;
            return value;
        }

    private:
        std::span<const std::byte> payload_;
        std::size_t                offset_ {};
    };

    struct Request
    {
        ::mpfshost::MessageType type {};
        std::uint64_t           id {};
        std::vector<std::byte>  payload;
    };

    bool read_exact(int fd, std::span<std::byte> buffer)
    {
        std::size_t offset = 0;
        while (offset < buffer.size())
        {
            const auto count = ::read(fd, buffer.data() + offset, buffer.size() - offset);
            if (count <= 0)
            {
                return false;
            }
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool write_exact(int fd, std::span<const std::byte> buffer)
    {
        std::size_t offset = 0;
        while (offset < buffer.size())
        {
            const auto count = ::write(fd, buffer.data() + offset, buffer.size() - offset);
            if (count <= 0)
            {
                return false;
            }
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }

    std::optional<Request> read_request(int fd)
    {
        std::array<std::byte, ::mpfshost::kFrameHeaderSize> header {};
        if (!read_exact(fd, header))
        {
            return std::nullopt;
        }

        PayloadReader reader {header};
        (void)reader.u32();
        (void)reader.u16();
        Request request;
        request.type      = static_cast<::mpfshost::MessageType>(reader.u16());
        request.id        = reader.u64();
        const auto length = reader.u32();
        request.payload.resize(length);
        if (length != 0 && !read_exact(fd, request.payload))
        {
            return std::nullopt;
        }
        return request;
    }

    void write_response(int fd, ::mpfshost::MessageType type, std::uint64_t id, std::span<const std::byte> payload)
    {
        PayloadWriter header;
        header.u32(::mpfshost::kProtocolMagic);
        header.u16(::mpfshost::kProtocolVersion);
        header.u16(static_cast<std::uint16_t>(type));
        header.u64(id);
        header.u32(static_cast<std::uint32_t>(payload.size()));
        std::vector<std::byte> frame = header.data;
        frame.insert(frame.end(), payload.begin(), payload.end());
        write_exact(fd, frame);
    }

    void write_status(PayloadWriter& writer, ::mpfshost::DebugRunState state, bool stub_active, bool stub_owned)
    {
        writer.u8(static_cast<std::uint8_t>(state));
        writer.u8(stub_active ? 1 : 0);
        writer.u8(0);
        writer.u32(4);
        writer.u32(0);
        writer.u8(stub_owned ? 1 : 0);
        writer.u8(0);
        writer.string("x86_64");
        writer.u8(static_cast<std::uint8_t>(::mpfshost::DebugWatchAccess::Write));
    }

    void write_register_record(PayloadWriter& writer, std::uint32_t guest_tid)
    {
        writer.u32(0);
        writer.string("1");
        writer.u64(0x401000);
        writer.u64(0x700000);
        writer.u64(0x700100);
        writer.u64(0x246);
        writer.u64(0);
        writer.u64(0x1000);
        writer.u64(0);
        writer.u64(0);
        writer.u64(1);
        writer.u64(2);
        writer.u64(3);
        writer.u64(4);
        writer.u64(5);
        writer.u64(6);
        writer.u64(7);
        writer.u64(8);
        writer.u64(9);
        writer.u64(10);
        writer.u64(11);
        writer.u64(12);
        writer.u64(13);
        writer.u64(14);
        writer.u32(1234);
        writer.u32(guest_tid);
        writer.u8(1);
        writer.u8(1);
    }

    class FakeHost
    {
    public:
        explicit FakeHost(std::function<void(int)> handler)
            : path_(std::filesystem::temp_directory_path()
                    / std::format("slopkit-mpfshost-{}-{}.sock", ::getpid(), counter_++))
        {
            fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            REQUIRE(fd_ >= 0);

            sockaddr_un address {};
            address.sun_family = AF_UNIX;
            REQUIRE(path_.string().size() < sizeof(address.sun_path));
            std::memcpy(address.sun_path, path_.c_str(), path_.string().size());

            REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
            REQUIRE(::listen(fd_, 4) == 0);

            thread_ = std::jthread(
                [this, handler = std::move(handler)]()
                {
                    for (;;)
                    {
                        const int client = ::accept(fd_, nullptr, nullptr);
                        if (client < 0)
                        {
                            return;
                        }
                        handler(client);
                        ::close(client);
                        if (stop_)
                        {
                            return;
                        }
                    }
                });
        }

        FakeHost(const FakeHost&)            = delete;
        FakeHost& operator=(const FakeHost&) = delete;

        ~FakeHost()
        {
            stop();
            std::filesystem::remove(path_);
        }

        [[nodiscard]] std::string path() const
        {
            return path_.string();
        }

        void stop()
        {
            if (stop_.exchange(true))
            {
                return;
            }
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
            if (thread_.joinable())
            {
                thread_.join();
            }
        }

    private:
        static inline std::atomic<std::uint32_t> counter_ {0};

        std::filesystem::path path_;
        int                   fd_ {-1};
        std::atomic<bool>     stop_ {false};
        std::jthread          thread_;
    };
}

TEST_CASE("the mpfshost client reads capabilities and targets", "[mpfshost]")
{
    FakeHost host {[](int client)
                   {
                       const auto request = read_request(client);
                       REQUIRE(request);
                       REQUIRE(request->type == ::mpfshost::MessageType::CapabilitiesRequest);

                       PayloadWriter payload;
                       payload.string("0.2.0");
                       payload.u32(2);
                       payload.u16(static_cast<std::uint16_t>(::mpfshost::MessageType::DebugAttachRequest));
                       payload.u16(static_cast<std::uint16_t>(::mpfshost::MessageType::DebugWaitRequest));
                       write_response(client, ::mpfshost::MessageType::CapabilitiesResponse, request->id, payload.data);
                   }};

    const slopkit::plugins::mpfshost::Client client {host.path()};
    const auto                               capabilities = client.capabilities();
    REQUIRE(capabilities);
    CHECK(capabilities->server_version == "0.2.0");
    CHECK(capabilities->supports(::mpfshost::MessageType::DebugAttachRequest));
    CHECK(capabilities->supports(::mpfshost::MessageType::DebugWaitRequest));
    CHECK_FALSE(capabilities->supports(::mpfshost::MessageType::DebugDetachRequest));
}

TEST_CASE("the mpfshost client session binds and lists processes", "[mpfshost]")
{
    FakeHost host {[](int client)
                   {
                       const auto bind_request = read_request(client);
                       REQUIRE(bind_request);
                       REQUIRE(bind_request->type == ::mpfshost::MessageType::BindRequest);

                       PayloadWriter bound;
                       bound.u64(42);
                       bound.string("win11");
                       bound.u32(99);
                       bound.string("/run/qmp.sock");
                       write_response(client, ::mpfshost::MessageType::BindResponse, bind_request->id, bound.data);

                       const auto list = read_request(client);
                       REQUIRE(list);
                       REQUIRE(list->type == ::mpfshost::MessageType::ProcessListRequest);

                       PayloadWriter processes;
                       processes.u32(1);
                       processes.u32(1234);
                       processes.u32(1);
                       processes.u8(0);
                       processes.string("notepad.exe");
                       write_response(client, ::mpfshost::MessageType::ProcessListResponse, list->id, processes.data);
                   }};

    const slopkit::plugins::mpfshost::Client client {host.path()};
    auto                                     session = client.bind("win11");
    REQUIRE(session);
    CHECK(session->target_name() == "win11");

    const auto processes = session->processes();
    REQUIRE(processes);
    REQUIRE(processes->size() == 1);
    CHECK((*processes)[0].pid == 1234);
    CHECK((*processes)[0].name == "notepad.exe");
}

TEST_CASE("the mpfshost client decodes debug attach, registers and a wait hit", "[mpfshost]")
{
    FakeHost host {
        [](int client)
        {
            const auto bind_request = read_request(client);
            REQUIRE(bind_request);
            PayloadWriter bound;
            bound.u64(42);
            bound.string("win11");
            bound.u32(99);
            bound.string("/run/qmp.sock");
            write_response(client, ::mpfshost::MessageType::BindResponse, bind_request->id, bound.data);

            const auto attach_request = read_request(client);
            REQUIRE(attach_request);
            REQUIRE(attach_request->type == ::mpfshost::MessageType::DebugAttachRequest);
            PayloadWriter attach;
            write_status(attach, ::mpfshost::DebugRunState::Paused, true, true);
            attach.string("unix");
            attach.u8(1);
            write_response(client, ::mpfshost::MessageType::DebugAttachResponse, attach_request->id, attach.data);

            const auto registers_request = read_request(client);
            REQUIRE(registers_request);
            REQUIRE(registers_request->type == ::mpfshost::MessageType::DebugRegistersRequest);
            PayloadWriter registers;
            registers.u32(1);
            write_register_record(registers, 77);
            write_response(
                client, ::mpfshost::MessageType::DebugRegistersResponse, registers_request->id, registers.data);

            const auto arm_request = read_request(client);
            REQUIRE(arm_request);
            REQUIRE(arm_request->type == ::mpfshost::MessageType::DebugBreakpointArmRequest);
            PayloadWriter armed;
            armed.u32(9);
            armed.u32(1234);
            armed.u64(0x401000);
            armed.u8(static_cast<std::uint8_t>(::mpfshost::DebugBreakpointKind::Software));
            armed.u32(::mpfshost::kNoDebugHardwareSlot);
            write_response(client, ::mpfshost::MessageType::DebugBreakpointArmResponse, arm_request->id, armed.data);

            const auto wait_request = read_request(client);
            REQUIRE(wait_request);
            REQUIRE(wait_request->type == ::mpfshost::MessageType::DebugWaitRequest);
            PayloadWriter hit;
            hit.u32(9);
            hit.u8(static_cast<std::uint8_t>(::mpfshost::DebugResourceKind::Breakpoint));
            hit.u8(static_cast<std::uint8_t>(::mpfshost::DebugBreakpointKind::Software));
            hit.u64(0x401000);
            write_register_record(hit, 77);
            hit.string({});
            hit.u64(0);
            hit.u8(0);
            hit.u64(0);
            hit.u64(0);
            hit.string({});
            hit.u8(0);
            hit.string({});
            hit.u32(0);
            hit.u8(0);
            write_response(client, ::mpfshost::MessageType::DebugWaitHit, wait_request->id, hit.data);

            PayloadWriter wait_end;
            wait_end.u8(static_cast<std::uint8_t>(::mpfshost::DebugWatchStatus::Completed));
            wait_end.u32(1);
            wait_end.u32(0);
            wait_end.string({});
            write_response(client, ::mpfshost::MessageType::DebugWaitEnd, wait_request->id, wait_end.data);

            const auto detach_request = read_request(client);
            REQUIRE(detach_request);
            REQUIRE(detach_request->type == ::mpfshost::MessageType::DebugDetachRequest);
            PayloadWriter detached;
            write_status(detached, ::mpfshost::DebugRunState::Running, false, false);
            detached.u8(1);
            detached.u8(0);
            detached.u8(1);
            detached.u8(1);
            write_response(client, ::mpfshost::MessageType::DebugDetachResponse, detach_request->id, detached.data);
        }};

    const slopkit::plugins::mpfshost::Client client {host.path()};
    auto                                     session = client.bind("win11");
    REQUIRE(session);

    const auto attached = session->debug_attach();
    REQUIRE(attached);
    CHECK(attached->status.runState == ::mpfshost::DebugRunState::Paused);
    CHECK(attached->stubMode == "unix");

    const auto registers = session->debug_registers();
    REQUIRE(registers);
    REQUIRE(registers->size() == 1);
    CHECK((*registers)[0].threadFound);
    CHECK((*registers)[0].guestTid == 77);

    const auto armed = session->debug_arm_breakpoint(1234, 0x401000, ::mpfshost::DebugBreakpointKind::Software);
    REQUIRE(armed);
    CHECK(armed->id == 9);

    const auto waited = session->debug_wait(1000, 1);
    REQUIRE(waited);
    REQUIRE(waited->hits.size() == 1);
    CHECK(waited->hits[0].id == 9);
    CHECK(waited->hits[0].record.guestTid == 77);
    CHECK(waited->end.status == ::mpfshost::DebugWatchStatus::Completed);

    const auto detached = session->debug_detach();
    REQUIRE(detached);
    CHECK(detached->detached);
}

#endif
