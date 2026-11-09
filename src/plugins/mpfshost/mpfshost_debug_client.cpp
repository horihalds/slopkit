#include "plugins/mpfshost/mpfshost_client.hpp"

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>
#include <vector>

#include "plugin/plugin_api.h"
#include "plugins/mpfshost/mpfshost_client_impl.hpp"

namespace slopkit::plugins::mpfshost
{
    namespace
    {
        using namespace detail;

        std::expected<::mpfshost::DebugStatus, TransportError>
        decode_status(const std::expected<Frame, TransportError>& frame, ::mpfshost::MessageType expected)
        {
            if (!frame)
            {
                return std::unexpected(frame.error());
            }
            if (const auto checked = expect_type(*frame, expected); !checked)
            {
                return std::unexpected(checked.error());
            }

            Reader     reader {frame->payload};
            const auto status = read_debug_status(reader);
            if (!reader.ok() || !reader.at_end())
            {
                return std::unexpected(make_error(SLOPKIT_ERR_IO, "the host sent a malformed debug status"));
            }
            return status;
        }
    }

    std::expected<::mpfshost::DebugStatus, TransportError> ClientSession::debug_state() const
    {
        return decode_status(impl_->call(::mpfshost::MessageType::DebugStateRequest, {}),
                             ::mpfshost::MessageType::DebugStateResponse);
    }

    std::expected<::mpfshost::DebugStatus, TransportError>
    ClientSession::debug_control(::mpfshost::DebugControlAction action) const
    {
        detail::Writer request;
        request.u8(static_cast<std::uint8_t>(action));
        return decode_status(impl_->call(::mpfshost::MessageType::DebugControlRequest, request.buffer()),
                             ::mpfshost::MessageType::DebugControlResponse);
    }

    std::expected<::mpfshost::DebugAttachResponse, TransportError> ClientSession::debug_attach() const
    {
        auto frame = impl_->call(::mpfshost::MessageType::DebugAttachRequest, {});
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        if (const auto expected = expect_type(*frame, ::mpfshost::MessageType::DebugAttachResponse); !expected)
        {
            return std::unexpected(expected.error());
        }

        detail::Reader                  reader {frame->payload};
        ::mpfshost::DebugAttachResponse message;
        message.status      = detail::read_debug_status(reader);
        message.stubMode    = reader.string();
        message.guestPaused = reader.u8() != 0;
        if (!reader.ok() || !reader.at_end())
        {
            return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent a malformed attach response"));
        }
        return message;
    }

    std::expected<::mpfshost::DebugDetachResponse, TransportError> ClientSession::debug_detach() const
    {
        auto frame = impl_->call(::mpfshost::MessageType::DebugDetachRequest, {});
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        if (const auto expected = expect_type(*frame, ::mpfshost::MessageType::DebugDetachResponse); !expected)
        {
            return std::unexpected(expected.error());
        }

        detail::Reader                  reader {frame->payload};
        ::mpfshost::DebugDetachResponse message;
        message.status       = detail::read_debug_status(reader);
        message.stubStopped  = reader.u8() != 0;
        message.foreignStub  = reader.u8() != 0;
        message.guestResumed = reader.u8() != 0;
        message.detached     = reader.u8() != 0;
        if (!reader.ok() || !reader.at_end())
        {
            return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent a malformed detach response"));
        }
        return message;
    }

    std::expected<std::vector<::mpfshost::DebugRegisterRecord>, TransportError>
    ClientSession::debug_registers(std::uint32_t vcpu) const
    {
        detail::Writer request;
        request.u32(vcpu);

        auto frame = impl_->call(::mpfshost::MessageType::DebugRegistersRequest, request.buffer());
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        if (const auto expected = expect_type(*frame, ::mpfshost::MessageType::DebugRegistersResponse); !expected)
        {
            return std::unexpected(expected.error());
        }

        detail::Reader reader {frame->payload};
        const auto     count = reader.u32();
        if (!reader.ok() || count > frame->payload.size() || count > ::mpfshost::kMaxDebugVcpuRecords)
        {
            return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent malformed registers"));
        }

        std::vector<::mpfshost::DebugRegisterRecord> records;
        records.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            records.push_back(detail::read_debug_record(reader));
            if (!reader.ok())
            {
                return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent malformed registers"));
            }
        }
        if (!reader.at_end())
        {
            return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent malformed registers"));
        }
        return records;
    }

    std::expected<std::vector<::mpfshost::DebugThreadRecord>, TransportError> ClientSession::debug_threads() const
    {
        auto frame = impl_->call(::mpfshost::MessageType::DebugThreadsRequest, {});
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        if (const auto expected = expect_type(*frame, ::mpfshost::MessageType::DebugThreadsResponse); !expected)
        {
            return std::unexpected(expected.error());
        }

        detail::Reader reader {frame->payload};
        const auto     count = reader.u32();
        if (!reader.ok() || count > frame->payload.size() || count > ::mpfshost::kMaxDebugVcpuRecords)
        {
            return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent malformed debug threads"));
        }

        std::vector<::mpfshost::DebugThreadRecord> threads;
        threads.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            ::mpfshost::DebugThreadRecord thread;
            thread.vcpu         = reader.u32();
            thread.threadId     = reader.string();
            thread.rip          = reader.u64();
            thread.rsp          = reader.u64();
            thread.cr3          = reader.u64();
            thread.processFound = reader.u8() != 0;
            thread.pid          = reader.u32();
            thread.threadFound  = reader.u8() != 0;
            thread.tid          = reader.u32();
            if (!reader.ok())
            {
                return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent malformed debug threads"));
            }
            threads.push_back(std::move(thread));
        }
        if (!reader.at_end())
        {
            return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent malformed debug threads"));
        }
        return threads;
    }

    std::expected<::mpfshost::DebugStepResponse, TransportError>
    ClientSession::debug_step(std::uint32_t vcpu, std::string_view thread_id) const
    {
        detail::Writer request;
        request.u32(vcpu);
        request.string(thread_id);

        auto frame = impl_->call(::mpfshost::MessageType::DebugStepRequest, request.buffer());
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        if (const auto expected = expect_type(*frame, ::mpfshost::MessageType::DebugStepResponse); !expected)
        {
            return std::unexpected(expected.error());
        }

        detail::Reader                reader {frame->payload};
        ::mpfshost::DebugStepResponse message;
        message.record = detail::read_debug_record(reader);
        if (!reader.ok() || !reader.at_end())
        {
            return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent a malformed step response"));
        }
        return message;
    }

    std::expected<::mpfshost::DebugBacktraceResponse, TransportError> ClientSession::debug_backtrace(
        std::uint32_t pid, std::uint32_t vcpu, std::string_view thread_id, std::uint32_t max_frames) const
    {
        detail::Writer request;
        request.u32(pid);
        request.u32(vcpu);
        request.string(thread_id);
        request.u32(max_frames);

        auto frame = impl_->call(::mpfshost::MessageType::DebugBacktraceRequest, request.buffer());
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        if (const auto expected = expect_type(*frame, ::mpfshost::MessageType::DebugBacktraceResponse); !expected)
        {
            return std::unexpected(expected.error());
        }

        detail::Reader reader {frame->payload};
        const auto     count = reader.u32();
        if (!reader.ok() || count > frame->payload.size() || count > ::mpfshost::kMaxDebugBacktraceFrames)
        {
            return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent a malformed backtrace"));
        }

        ::mpfshost::DebugBacktraceResponse message;
        message.frames.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            ::mpfshost::DebugBacktraceFrame frame_record;
            frame_record.address         = reader.u64();
            frame_record.framePointer    = reader.u64();
            frame_record.module          = reader.string();
            frame_record.moduleOffset    = reader.u64();
            frame_record.exportFound     = reader.u8() != 0;
            frame_record.exportAddress   = reader.u64();
            frame_record.exportOffset    = reader.u64();
            frame_record.exportName      = reader.string();
            frame_record.pdbFound        = reader.u8() != 0;
            frame_record.pdbSymbol       = reader.string();
            frame_record.pdbDisplacement = reader.u32();
            if (!reader.ok())
            {
                return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent a malformed backtrace"));
            }
            message.frames.push_back(std::move(frame_record));
        }
        message.truncated  = reader.u8() != 0;
        message.stopReason = reader.string();
        if (!reader.ok() || !reader.at_end())
        {
            return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent a malformed backtrace"));
        }
        return message;
    }

    std::expected<::mpfshost::DebugBreakpointArmResponse, TransportError> ClientSession::debug_arm_breakpoint(
        std::uint32_t pid, std::uint64_t address, ::mpfshost::DebugBreakpointKind kind) const
    {
        detail::Writer request;
        request.u32(pid);
        request.u64(address);
        request.u8(static_cast<std::uint8_t>(kind));

        auto frame = impl_->call(::mpfshost::MessageType::DebugBreakpointArmRequest, request.buffer());
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        if (const auto expected = expect_type(*frame, ::mpfshost::MessageType::DebugBreakpointArmResponse); !expected)
        {
            return std::unexpected(expected.error());
        }

        detail::Reader                         reader {frame->payload};
        ::mpfshost::DebugBreakpointArmResponse message;
        message.id      = reader.u32();
        message.pid     = reader.u32();
        message.address = reader.u64();
        message.kind    = static_cast<::mpfshost::DebugBreakpointKind>(reader.u8());
        message.slot    = reader.u32();
        if (!reader.ok() || !reader.at_end())
        {
            return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent a malformed breakpoint"));
        }
        return message;
    }

    std::expected<::mpfshost::DebugWatchArmResponse, TransportError> ClientSession::debug_arm_watch(
        std::uint32_t pid, std::uint64_t address, std::uint32_t width, ::mpfshost::DebugWatchAccess access) const
    {
        detail::Writer request;
        request.u32(pid);
        request.u64(address);
        request.u32(width);
        request.u8(static_cast<std::uint8_t>(access));

        auto frame = impl_->call(::mpfshost::MessageType::DebugWatchArmRequest, request.buffer());
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        if (const auto expected = expect_type(*frame, ::mpfshost::MessageType::DebugWatchArmResponse); !expected)
        {
            return std::unexpected(expected.error());
        }

        detail::Reader                    reader {frame->payload};
        ::mpfshost::DebugWatchArmResponse message;
        message.id      = reader.u32();
        message.pid     = reader.u32();
        message.address = reader.u64();
        message.width   = reader.u32();
        message.access  = static_cast<::mpfshost::DebugWatchAccess>(reader.u8());
        message.slot    = reader.u32();
        if (!reader.ok() || !reader.at_end())
        {
            return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent a malformed watchpoint"));
        }
        return message;
    }

    namespace
    {
        std::expected<::mpfshost::DebugResourceRemoveResponse, TransportError>
        decode_remove(const std::expected<detail::Frame, TransportError>& frame, ::mpfshost::MessageType expected)
        {
            if (!frame)
            {
                return std::unexpected(frame.error());
            }
            if (const auto checked = detail::expect_type(*frame, expected); !checked)
            {
                return std::unexpected(checked.error());
            }

            detail::Reader                          reader {frame->payload};
            ::mpfshost::DebugResourceRemoveResponse message;
            message.id      = reader.u32();
            message.removed = reader.u8() != 0;
            if (!reader.ok() || !reader.at_end())
            {
                return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent a malformed remove response"));
            }
            return message;
        }
    }

    std::expected<::mpfshost::DebugResourceRemoveResponse, TransportError>
    ClientSession::debug_remove_breakpoint(std::uint32_t id) const
    {
        detail::Writer request;
        request.u32(id);
        return decode_remove(impl_->call(::mpfshost::MessageType::DebugBreakpointRemoveRequest, request.buffer()),
                             ::mpfshost::MessageType::DebugResourceRemoveResponse);
    }

    std::expected<::mpfshost::DebugResourceRemoveResponse, TransportError>
    ClientSession::debug_remove_watch(std::uint32_t id) const
    {
        detail::Writer request;
        request.u32(id);
        return decode_remove(impl_->call(::mpfshost::MessageType::DebugWatchRemoveRequest, request.buffer()),
                             ::mpfshost::MessageType::DebugResourceRemoveResponse);
    }

    std::expected<::mpfshost::DebugStatus, TransportError> ClientSession::debug_continue_armed() const
    {
        return decode_status(impl_->call(::mpfshost::MessageType::DebugContinueRequest, {}),
                             ::mpfshost::MessageType::DebugContinueResponse);
    }

    std::expected<ClientSession::WaitResult, TransportError> ClientSession::debug_wait(std::uint32_t timeout_ms,
                                                                                       std::uint32_t max_hits) const
    {
        detail::Writer request;
        request.u32(timeout_ms);
        request.u32(max_hits);

        auto frame = impl_->call(::mpfshost::MessageType::DebugWaitRequest, request.buffer());
        if (!frame)
        {
            return std::unexpected(frame.error());
        }

        WaitResult result;
        for (;;)
        {
            if (frame->type == ::mpfshost::MessageType::ErrorResponse)
            {
                const auto expected = expect_type(*frame, ::mpfshost::MessageType::DebugWaitEnd);
                return std::unexpected(expected.error());
            }
            if (frame->type == ::mpfshost::MessageType::DebugWaitEnd)
            {
                detail::Reader reader {frame->payload};
                const auto     status    = static_cast<::mpfshost::DebugWatchStatus>(reader.u8());
                const auto     hit_count = reader.u32();
                const auto     skipped   = reader.u32();
                const auto     error     = reader.string();
                if (!reader.ok() || !reader.at_end())
                {
                    return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent a malformed wait end"));
                }
                result.end.status       = status;
                result.end.hitCount     = hit_count;
                result.end.skippedCount = skipped;
                result.end.error        = error;
                return result;
            }
            if (frame->type != ::mpfshost::MessageType::DebugWaitHit)
            {
                return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent an unexpected wait frame"));
            }

            detail::Reader           reader {frame->payload};
            ::mpfshost::DebugWaitHit hit;
            hit.id              = reader.u32();
            hit.kind            = static_cast<::mpfshost::DebugResourceKind>(reader.u8());
            hit.breakpointKind  = static_cast<::mpfshost::DebugBreakpointKind>(reader.u8());
            hit.address         = reader.u64();
            hit.record          = detail::read_debug_record(reader);
            hit.module          = reader.string();
            hit.moduleOffset    = reader.u64();
            hit.exportFound     = reader.u8() != 0;
            hit.exportAddress   = reader.u64();
            hit.exportOffset    = reader.u64();
            hit.exportName      = reader.string();
            hit.pdbFound        = reader.u8() != 0;
            hit.pdbSymbol       = reader.string();
            hit.pdbDisplacement = reader.u32();
            hit.unverified      = reader.u8() != 0;
            if (!reader.ok() || !reader.at_end())
            {
                return std::unexpected(detail::make_error(SLOPKIT_ERR_IO, "the host sent a malformed wait hit"));
            }
            result.hits.push_back(std::move(hit));

            frame = impl_->connection.receive();
            if (!frame)
            {
                return std::unexpected(frame.error());
            }
        }
    }
}
