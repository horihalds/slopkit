#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "plugins/mpfshost/mpfshost_client.hpp"
#include "plugins/mpfshost/mpfshost_debug.hpp"
#include "plugins/mpfshost/mpfshost_helpers.hpp"
#include "plugins/support/plugin_support.hpp"
#include "plugins/support/session.hpp"

namespace
{
    using slopkit::plugins::mpfshost::Client;
    using slopkit::plugins::mpfshost::ClientSession;
    using slopkit::plugins::mpfshost::DebugManager;
    using slopkit::plugins::mpfshost::GuestTarget;
    using slopkit::plugins::mpfshost::TargetTable;
    using slopkit::plugins::mpfshost::TransportError;
    using slopkit::plugins::support::AccessFault;
    using slopkit::plugins::support::DebugHooks;
    using slopkit::plugins::support::MemoryResult;
    using slopkit::plugins::support::PluginProfile;
    using slopkit::plugins::support::Session;

    namespace process  = slopkit::process;
    namespace platform = slopkit::platform;
    using slopkit::process::AccessError;
    using slopkit::process::ModuleInfo;
    using slopkit::process::ModuleKind;
    using slopkit::process::ProcessId;
    using slopkit::process::ProcessInfo;
    using slopkit::process::ThreadInfo;

    constexpr std::size_t kChunkSize = 64u << 20;

    const slopkit_plugin_info kInfo {
        "mpfshost",
        "MemProcFS guest access",
        "0.2.0",
        "Attaches to processes inside QEMU/KVM guests through the system mpfshost daemon; reads, writes, modules, "
        "threads, regions, validation and x86-64 guest debugging work, suspend/resume pauses the whole guest, and "
        "target allocation does not."};

    const slopkit_plugin_info* info() noexcept
    {
        return &kInfo;
    }

    std::int32_t precedence() noexcept
    {
        return 20;
    }

    std::uint32_t access_methods() noexcept
    {
        return SLOPKIT_ACCESS_MPFSHOST;
    }

    TargetTable  g_targets;
    DebugManager g_debug;

    AccessFault fault(std::int32_t code, std::string message)
    {
        return AccessFault {code, std::move(message)};
    }

    AccessFault client_fault(const TransportError& error)
    {
        return fault(error.code, error.message);
    }

    std::expected<ClientSession, AccessFault> bind_guest(const GuestTarget& guest)
    {
        const Client client {slopkit::plugins::mpfshost::socket_path()};
        auto         session = client.bind(guest.target);
        if (!session)
        {
            return std::unexpected(client_fault(session.error()));
        }
        return std::move(*session);
    }

    std::optional<GuestTarget> guest_for(Session& session)
    {
        return g_targets.lookup(session.pid);
    }

    std::string target_label(const GuestTarget& guest)
    {
        return std::format("/mpfshost/{}/{}", guest.target, guest.name);
    }

    std::expected<std::vector<ProcessInfo>, AccessFault> list_processes()
    {
        const Client client {slopkit::plugins::mpfshost::socket_path()};

        const auto targets = client.targets();
        if (!targets)
        {
            return std::vector<ProcessInfo> {};
        }

        std::vector<ProcessInfo> result;
        for (const auto& target : *targets)
        {
            auto session = client.bind(target.name);
            if (!session)
            {
                continue;
            }

            const auto processes = session->processes();
            if (!processes)
            {
                continue;
            }

            for (const auto& process : *processes)
            {
                GuestTarget guest {target.name, process.pid, process.name, process.wow64};
                const auto  synthetic = g_targets.remember(guest);

                ProcessInfo info;
                info.pid      = synthetic;
                info.name     = process.name;
                info.exe_path = target_label(guest);
                result.push_back(std::move(info));
            }
        }
        return result;
    }

    bool resolve_session(ProcessId pid, std::string& out_name)
    {
        const auto guest = g_targets.lookup(pid);
        if (!guest)
        {
            return false;
        }
        out_name = guest->name;
        return true;
    }

    std::expected<MemoryResult, AccessFault> read(Session& session, std::uint64_t address, std::span<std::byte> buffer)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(fault(SLOPKIT_ERR_NOT_FOUND, "the guest process is not known"));
        }

        auto session_or_error = bind_guest(*guest);
        if (!session_or_error)
        {
            return std::unexpected(session_or_error.error());
        }
        auto& session_client = *session_or_error;

        std::size_t offset = 0;
        while (offset < buffer.size())
        {
            const auto wanted = std::min(kChunkSize, buffer.size() - offset);

            const auto data =
                session_client.read_memory(guest->pid, address + offset, static_cast<std::uint32_t>(wanted));
            if (!data)
            {
                if (offset > 0)
                {
                    return MemoryResult {offset, SLOPKIT_ACCESS_MPFSHOST};
                }
                return std::unexpected(client_fault(data.error()));
            }

            const auto copied = std::min<std::size_t>(data->size(), wanted);
            std::copy_n(data->begin(), static_cast<std::ptrdiff_t>(copied), buffer.begin() + offset);
            offset += copied;
            if (copied < wanted)
            {
                break;
            }
        }
        return MemoryResult {offset, SLOPKIT_ACCESS_MPFSHOST};
    }

    std::expected<MemoryResult, AccessFault>
    write(Session& session, std::uint64_t address, std::span<const std::byte> data)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(fault(SLOPKIT_ERR_NOT_FOUND, "the guest process is not known"));
        }

        auto session_or_error = bind_guest(*guest);
        if (!session_or_error)
        {
            return std::unexpected(session_or_error.error());
        }
        auto& session_client = *session_or_error;

        std::size_t offset = 0;
        while (offset < data.size())
        {
            const auto chunk = std::min(kChunkSize, data.size() - offset);
            const auto span  = data.subspan(offset, chunk);

            const auto written = session_client.write_memory(guest->pid, address + offset, span);
            if (!written)
            {
                return std::unexpected(client_fault(written.error()));
            }
            if (*written == 0)
            {
                return std::unexpected(fault(SLOPKIT_ERR_IO, "the host wrote no bytes"));
            }
            offset += static_cast<std::size_t>(*written);
        }
        return MemoryResult {offset, SLOPKIT_ACCESS_MPFSHOST};
    }

    std::expected<std::vector<ModuleInfo>, AccessFault> modules(Session& session)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(fault(SLOPKIT_ERR_NOT_FOUND, "the guest process is not known"));
        }

        auto session_or_error = bind_guest(*guest);
        if (!session_or_error)
        {
            return std::unexpected(session_or_error.error());
        }
        auto& session_client = *session_or_error;

        const auto descriptors = session_client.modules(guest->pid);
        if (!descriptors)
        {
            return std::unexpected(client_fault(descriptors.error()));
        }

        std::vector<ModuleInfo> result;
        result.reserve(descriptors->size());
        for (const auto& descriptor : *descriptors)
        {
            ModuleInfo module;
            module.base         = descriptor.base;
            module.size         = descriptor.imageSize;
            module.entry        = descriptor.entry;
            module.kind         = ModuleKind::pe;
            module.name         = descriptor.name.empty() ? descriptor.fullName : descriptor.name;
            module.path         = descriptor.fullName;
            module.is_main      = slopkit::plugins::mpfshost::is_main_module(module.name, guest->name);
            module.pointer_size = guest->wow64 ? 4u : 8u;
            result.push_back(std::move(module));
        }
        return result;
    }

    std::expected<std::vector<slopkit::platform::MappedRegion>, AccessFault> regions(Session& session)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(fault(SLOPKIT_ERR_NOT_FOUND, "the guest process is not known"));
        }

        auto session_or_error = bind_guest(*guest);
        if (!session_or_error)
        {
            return std::unexpected(session_or_error.error());
        }
        auto& session_client = *session_or_error;

        const auto descriptors = session_client.pte(guest->pid);
        if (!descriptors)
        {
            return std::unexpected(client_fault(descriptors.error()));
        }

        std::vector<slopkit::platform::MappedRegion> result;
        result.reserve(descriptors->size());
        for (const auto& descriptor : *descriptors)
        {
            if (descriptor.pages == 0)
            {
                continue;
            }

            const auto protection = slopkit::plugins::mpfshost::classify_pte_flags(descriptor.pageFlags);
            const auto size       = descriptor.pages * ::mpfshost::kPageSize;
            if (size == 0 || descriptor.vaBase + size < descriptor.vaBase)
            {
                continue;
            }

            slopkit::platform::MappedRegion region;
            region.start      = descriptor.vaBase;
            region.end        = descriptor.vaBase + size;
            region.readable   = protection.readable;
            region.writable   = protection.writable;
            region.executable = protection.executable;
            region.path       = descriptor.text;
            result.push_back(std::move(region));
        }
        return result;
    }

    std::expected<std::vector<ThreadInfo>, AccessFault> threads(Session& session)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(fault(SLOPKIT_ERR_NOT_FOUND, "the guest process is not known"));
        }

        auto session_or_error = bind_guest(*guest);
        if (!session_or_error)
        {
            return std::unexpected(session_or_error.error());
        }
        auto& session_client = *session_or_error;

        const auto descriptors = session_client.threads(guest->pid);
        if (!descriptors)
        {
            return std::unexpected(client_fault(descriptors.error()));
        }

        std::vector<ThreadInfo> result;
        result.reserve(descriptors->size());
        for (const auto& descriptor : *descriptors)
        {
            result.push_back(ThreadInfo {descriptor.tid, {}});
        }
        return result;
    }

    std::expected<bool, AccessFault> range_is_readable(Session& session, std::uint64_t address, std::size_t size)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(fault(SLOPKIT_ERR_NOT_FOUND, "the guest process is not known"));
        }

        constexpr std::size_t kProbeSize       = 64 * 1024;
        auto                  session_or_error = bind_guest(*guest);
        if (!session_or_error)
        {
            return std::unexpected(session_or_error.error());
        }
        auto& session_client = *session_or_error;

        std::size_t offset = 0;
        while (offset < size)
        {
            const auto wanted = std::min(kProbeSize, size - offset);
            const auto data =
                session_client.read_memory(guest->pid, address + offset, static_cast<std::uint32_t>(wanted));
            if (!data || data->size() < wanted)
            {
                return false;
            }
            offset += wanted;
        }
        return true;
    }

    std::expected<process::ProcessId, process::AccessError> debug_attach(Session& session,
                                                                         std::span<const process::ProcessId>)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        return g_debug.attach(*guest, session.pid);
    }

    std::expected<void, process::AccessError> debug_detach(Session& session)
    {
        return g_debug.detach(session.pid);
    }

    std::expected<platform::StopStatus, process::AccessError>
    debug_resume(Session& session, std::uint64_t resume_address, std::size_t resume_step_size)
    {
        return g_debug.resume(session.pid, resume_address, resume_step_size);
    }

    std::expected<platform::StopStatus, process::AccessError> debug_step(Session& session, std::uint32_t tid)
    {
        return g_debug.step(session.pid, tid);
    }

    std::expected<void, process::AccessError> debug_interrupt(Session& session, std::uint32_t)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        return g_debug.interrupt(*guest);
    }

    std::expected<platform::Registers, process::AccessError> debug_registers(Session& session, std::uint32_t tid)
    {
        return g_debug.registers(session.pid, tid);
    }

    std::expected<void, process::AccessError>
    debug_set_register(Session& session, std::uint32_t tid, std::string_view name, std::uint64_t value)
    {
        return g_debug.set_register(session.pid, tid, name, value);
    }

    std::expected<void, process::AccessError>
    debug_arm_software(Session& session, std::uint32_t slot, std::uint64_t address, bool insert)
    {
        return g_debug.arm_software(session.pid, slot, address, insert);
    }

    std::expected<void, process::AccessError> debug_arm_hardware(
        Session& session, std::uint32_t slot, std::int32_t kind, std::uint64_t address, std::size_t size, bool insert)
    {
        return g_debug.arm_hardware(session.pid, slot, kind, address, size, insert);
    }

    std::expected<std::vector<platform::DebugFrame>, process::AccessError> debug_backtrace(Session&      session,
                                                                                           std::uint32_t tid)
    {
        return g_debug.backtrace(session.pid, tid);
    }

    std::expected<void, process::AccessError> suspend(Session& session)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        return g_debug.suspend(*guest);
    }

    std::expected<void, process::AccessError> resume_target(Session& session)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        return g_debug.resume_target(*guest);
    }

    constexpr DebugHooks kDebugHooks {debug_attach,
                                      debug_detach,
                                      debug_resume,
                                      debug_step,
                                      debug_interrupt,
                                      debug_registers,
                                      debug_set_register,
                                      debug_arm_software,
                                      debug_arm_hardware,
                                      debug_backtrace};

    void close_session(Session& session)
    {
        g_debug.forget(session.pid);
    }

    constexpr PluginProfile kProfile {info,
                                      precedence,
                                      access_methods,
                                      nullptr,
                                      nullptr,
                                      modules,
                                      slopkit::platform::ForeignSignalPolicy::suppress,
                                      nullptr,
                                      nullptr,
                                      close_session,
                                      regions,
                                      range_is_readable,
                                      read,
                                      write,
                                      nullptr,
                                      threads,
                                      list_processes,
                                      resolve_session,
                                      true,
                                      true,
                                      false,
                                      &kDebugHooks,
                                      suspend,
                                      resume_target};
}

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host)
{
    return slopkit::plugins::support::entry<kProfile>(host);
}
