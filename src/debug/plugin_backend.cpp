#include "debug/plugin_backend.hpp"

#include <cstdint>
#include <string>
#include <utility>

#include "plugin/plugin_host.hpp"

namespace slopkit::debug
{

    namespace
    {
        process::AccessError classify(const slopkit_result& result)
        {
            switch (result.code)
            {
            case SLOPKIT_ERR_PERMISSION_DENIED:
                return process::AccessError::permission_denied;
            case SLOPKIT_ERR_NOT_FOUND:
                return process::AccessError::not_found;
            case SLOPKIT_ERR_UNSUPPORTED:
                return process::AccessError::unsupported;
            case SLOPKIT_ERR_IO:
                return process::AccessError::io_error;
            case SLOPKIT_ERR_INVALID_ABI:
                return process::AccessError::invalid_abi;
            case SLOPKIT_ERR_INVALID_ARGUMENT:
                return process::AccessError::invalid_argument;
            default:
                return process::AccessError::internal;
            }
        }

        std::int32_t abi_kind(HardwareKind kind)
        {
            switch (kind)
            {
            case HardwareKind::write:
                return SLOPKIT_BP_HW_WRITE;
            case HardwareKind::read_write:
                return SLOPKIT_BP_HW_READ_WRITE;
            case HardwareKind::execute:
            default:
                return SLOPKIT_BP_HW_EXECUTE;
            }
        }

        StopEvent to_stop(const slopkit_stop_info& info)
        {
            StopEvent stop;
            switch (info.reason)
            {
            case SLOPKIT_STOP_BREAKPOINT:
                stop.reason = StopReason::breakpoint;
                break;
            case SLOPKIT_STOP_SINGLE_STEP:
                stop.reason = StopReason::single_step;
                break;
            case SLOPKIT_STOP_EXITED:
                stop.reason = StopReason::exited;
                break;
            case SLOPKIT_STOP_SIGNALED:
                stop.reason = StopReason::signalled;
                break;
            case SLOPKIT_STOP_INTERRUPT:
            default:
                stop.reason = StopReason::interrupt;
                break;
            }
            stop.tid          = info.tid;
            stop.address      = info.address;
            stop.trap_address = info.trap_address;
            stop.signal       = static_cast<std::uint32_t>(info.signal);
            if (info.breakpoint_slot >= 0)
            {
                stop.breakpoint_slot = static_cast<std::uint32_t>(info.breakpoint_slot);
            }
            return stop;
        }
    } // namespace

    PluginBackend::PluginBackend(plugin::PluginHost& host) : host_(host) {}

    PluginBackend::~PluginBackend()
    {
        if (session_)
        {
            (void)detach();
        }
    }

    bool PluginBackend::open() const noexcept
    {
        return session_.has_value() && session_->supports_debug();
    }

    const slopkit_plugin_vtable* PluginBackend::vtable() const noexcept
    {
        return session_ ? session_->vtable() : nullptr;
    }

    bool PluginBackend::supports_debug() const
    {
        return open();
    }

    std::expected<void, process::AccessError> PluginBackend::attach(process::ProcessId pid, std::string_view plugin_id)
    {
        session_.reset();

        auto* plugin = host_.find(plugin_id);
        if (plugin == nullptr)
        {
            return std::unexpected(process::AccessError::not_found);
        }

        auto opened = plugin->open_session(pid);
        if (!opened)
        {
            return std::unexpected(opened.error());
        }
        session_ = std::move(*opened);

        if (!session_->supports_debug())
        {
            session_.reset();
            return std::unexpected(process::AccessError::unsupported);
        }

        std::uint32_t        leader = 0;
        const slopkit_result result = vtable()->debug_attach(session_->handle(), &leader);
        if (result.code != SLOPKIT_OK)
        {
            const auto error = classify(result);
            session_.reset();
            return std::unexpected(error);
        }
        return {};
    }

    std::expected<void, process::AccessError> PluginBackend::detach()
    {
        if (!session_)
        {
            return {};
        }

        std::optional<process::AccessError> error;
        if (open())
        {
            if (const slopkit_result result = vtable()->debug_detach(session_->handle()); result.code != SLOPKIT_OK)
            {
                error = classify(result);
            }
        }
        session_.reset();

        if (error.has_value())
        {
            return std::unexpected(*error);
        }
        return {};
    }

    std::expected<StopEvent, process::AccessError> PluginBackend::cont(std::uint64_t resume_address,
                                                                       std::size_t   resume_step_size)
    {
        if (!open())
        {
            return std::unexpected(process::AccessError::internal);
        }
        slopkit_stop_info    info {};
        const slopkit_result result =
            vtable()->debug_continue(session_->handle(), resume_address, resume_step_size, &info);
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(classify(result));
        }
        return to_stop(info);
    }

    std::expected<StopEvent, process::AccessError> PluginBackend::step(std::uint32_t tid)
    {
        if (!open())
        {
            return std::unexpected(process::AccessError::internal);
        }
        slopkit_stop_info    info {};
        const slopkit_result result = vtable()->debug_step(session_->handle(), tid, &info);
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(classify(result));
        }
        return to_stop(info);
    }

    std::expected<void, process::AccessError> PluginBackend::interrupt(std::uint32_t tid)
    {
        if (!open())
        {
            return std::unexpected(process::AccessError::internal);
        }
        const slopkit_result result = vtable()->debug_interrupt(session_->handle(), tid);
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(classify(result));
        }
        return {};
    }

    std::expected<std::vector<RegisterValue>, process::AccessError> PluginBackend::registers(std::uint32_t tid)
    {
        if (!open())
        {
            return std::unexpected(process::AccessError::internal);
        }

        slopkit_register_value* values = nullptr;
        std::size_t             count  = 0;
        const slopkit_result    result = vtable()->debug_get_registers(session_->handle(), tid, &values, &count);
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(classify(result));
        }

        std::vector<RegisterValue> registers;
        registers.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            RegisterValue value;
            if (values[i].name != nullptr)
            {
                value.name = values[i].name;
            }
            value.value = values[i].value;
            registers.push_back(std::move(value));
        }
        session_->dealloc(values);
        return registers;
    }

    std::expected<void, process::AccessError>
    PluginBackend::set_register(std::uint32_t tid, std::string_view name, std::uint64_t value)
    {
        if (!open())
        {
            return std::unexpected(process::AccessError::internal);
        }
        const std::string    owned {name};
        const slopkit_result result = vtable()->debug_set_register(session_->handle(), tid, owned.c_str(), value);
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(classify(result));
        }
        return {};
    }

    std::expected<void, process::AccessError>
    PluginBackend::set_software_breakpoint(std::uint32_t slot, std::uint64_t address, bool insert)
    {
        if (!open())
        {
            return std::unexpected(process::AccessError::internal);
        }
        const slopkit_result result =
            vtable()->debug_set_software_breakpoint(session_->handle(), slot, address, insert ? 1 : 0);
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(classify(result));
        }
        return {};
    }

    std::expected<void, process::AccessError> PluginBackend::set_hardware_breakpoint(
        std::uint32_t slot, HardwareKind kind, std::uint64_t address, std::size_t size, bool insert)
    {
        if (!open())
        {
            return std::unexpected(process::AccessError::internal);
        }
        const slopkit_result result = vtable()->debug_set_hardware_breakpoint(
            session_->handle(), slot, abi_kind(kind), address, size, insert ? 1 : 0);
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(classify(result));
        }
        return {};
    }

    std::expected<std::vector<Frame>, process::AccessError> PluginBackend::backtrace(std::uint32_t tid)
    {
        if (!open())
        {
            return std::unexpected(process::AccessError::internal);
        }

        slopkit_frame_info*  frames = nullptr;
        std::size_t          count  = 0;
        const slopkit_result result = vtable()->debug_backtrace(session_->handle(), tid, &frames, &count);
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(classify(result));
        }

        std::vector<Frame> result_frames;
        result_frames.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            result_frames.push_back(Frame {frames[i].pc, frames[i].frame_pointer});
        }
        session_->dealloc(frames);
        return result_frames;
    }

} // namespace slopkit::debug
