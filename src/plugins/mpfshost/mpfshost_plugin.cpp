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

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "platform/linux/procfs.hpp"
#include "plugins/mpfshost/mpfshost_client.hpp"
#include "plugins/mpfshost/mpfshost_debug.hpp"
#include "plugins/mpfshost/mpfshost_helpers.hpp"
#include "plugins/support/plugin_support.hpp"
#include "plugins/support/session.hpp"

#include <mpfshost/client.hpp>

namespace
{
    using slopkit::plugins::mpfshost::DebugManager;
    using slopkit::plugins::mpfshost::GuestSessions;
    using slopkit::plugins::mpfshost::GuestTarget;
    using slopkit::plugins::mpfshost::TargetTable;
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

    const slopkit_plugin_info kInfo {
        "mpfshost",
        "MemProcFS guest access",
        "0.3.0",
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

    AccessFault client_fault(const ::mpfshost::Error& error)
    {
        return fault(slopkit::plugins::mpfshost::status_for_error(error),
                     slopkit::plugins::mpfshost::message_for_error(error));
    }

    std::optional<GuestTarget> guest_for(Session& session)
    {
        return g_targets.lookup(session.pid);
    }

    std::string target_label(const GuestTarget& guest)
    {
        return std::format("mpfshost://{}/{}", guest.target, guest.name);
    }

    std::expected<std::vector<ProcessInfo>, AccessFault> list_processes()
    {
        const ::mpfshost::Client client {slopkit::plugins::mpfshost::socket_path()};
        const auto               targets = client.targets();
        if (!targets)
        {
            slopkit::log::warning(slopkit::log::category::process,
                                  std::format("mpfshost target listing failed: {}",
                                              slopkit::plugins::mpfshost::message_for_error(targets.error())));
            return std::vector<ProcessInfo> {};
        }

        std::vector<ProcessInfo> result;
        for (const auto& target : *targets)
        {
            auto pool  = slopkit::plugins::mpfshost::sessions_for(target.name);
            auto lease = pool->acquire();
            if (!lease)
            {
                slopkit::log::warning(slopkit::log::category::process,
                                      std::format("mpfshost could not bind {}: {}", target.name, lease.error()));
                continue;
            }

            const auto processes = lease->session().processes();
            if (!processes)
            {
                slopkit::log::warning(slopkit::log::category::process,
                                      std::format("mpfshost could not list processes in {}: {}",
                                                  target.name,
                                                  slopkit::plugins::mpfshost::message_for_error(processes.error())));
                continue;
            }

            for (const auto& process : *processes)
            {
                GuestTarget guest {target.name, process.pid, process.name, process.wow64};
                const auto  synthetic = g_targets.remember(guest);

                ProcessInfo process_info;
                process_info.pid      = synthetic;
                process_info.name     = process.name;
                process_info.exe_path = process.imagePath.empty() ? target_label(guest) : process.imagePath;
                result.push_back(std::move(process_info));
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

        auto pool  = slopkit::plugins::mpfshost::sessions_for(guest->target);
        auto lease = pool->acquire();
        if (!lease)
        {
            return std::unexpected(fault(SLOPKIT_ERR_IO, lease.error()));
        }

        auto filled = lease->session().readMemoryPartial(guest->pid, address, buffer);
        if (!filled)
        {
            return std::unexpected(client_fault(filled.error()));
        }
        return MemoryResult {*filled, SLOPKIT_ACCESS_MPFSHOST};
    }

    std::expected<MemoryResult, AccessFault>
    write(Session& session, std::uint64_t address, std::span<const std::byte> data)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(fault(SLOPKIT_ERR_NOT_FOUND, "the guest process is not known"));
        }

        auto pool  = slopkit::plugins::mpfshost::sessions_for(guest->target);
        auto lease = pool->acquire();
        if (!lease)
        {
            return std::unexpected(fault(SLOPKIT_ERR_IO, lease.error()));
        }

        auto written = lease->session().writeMemory(guest->pid, address, data);
        if (!written)
        {
            return std::unexpected(client_fault(written.error()));
        }
        return MemoryResult {static_cast<std::size_t>(*written), SLOPKIT_ACCESS_MPFSHOST};
    }

    std::expected<std::vector<ModuleInfo>, AccessFault> modules(Session& session)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(fault(SLOPKIT_ERR_NOT_FOUND, "the guest process is not known"));
        }

        auto pool  = slopkit::plugins::mpfshost::sessions_for(guest->target);
        auto lease = pool->acquire();
        if (!lease)
        {
            return std::unexpected(fault(SLOPKIT_ERR_IO, lease.error()));
        }

        const auto descriptors = lease->session().modules(guest->pid);
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
            module.size         = descriptor.imageSize != 0 ? descriptor.imageSize : descriptor.rawFileSize;
            module.entry        = descriptor.entry;
            module.kind         = ModuleKind::pe;
            module.name         = descriptor.name.empty() ? descriptor.fullName : descriptor.name;
            module.path         = descriptor.fullName;
            module.is_main      = slopkit::plugins::mpfshost::is_main_module(module.name, guest->name);
            module.pointer_size = guest->wow64 ? 4u : 8u;
            result.push_back(std::move(module));
        }
        if (descriptors->truncated)
        {
            slopkit::log::warning(slopkit::log::category::process,
                                  std::format("mpfshost truncated the module list of {} at {} entries",
                                              guest->name,
                                              descriptors->size()));
        }
        return result;
    }

    std::expected<std::vector<platform::MappedRegion>, AccessFault> regions(Session& session)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(fault(SLOPKIT_ERR_NOT_FOUND, "the guest process is not known"));
        }

        auto pool  = slopkit::plugins::mpfshost::sessions_for(guest->target);
        auto lease = pool->acquire();
        if (!lease)
        {
            return std::unexpected(fault(SLOPKIT_ERR_IO, lease.error()));
        }

        const auto mapped = lease->session().pte(guest->pid, true);
        if (!mapped)
        {
            return std::unexpected(client_fault(mapped.error()));
        }

        std::vector<platform::MappedRegion> result;
        result.reserve(mapped->regions.size());
        for (const auto& descriptor : mapped->regions)
        {
            if (descriptor.pages == 0)
            {
                continue;
            }

            const auto protection = slopkit::plugins::mpfshost::classify_pte_flags(descriptor.pageFlags);
            const auto size       = descriptor.pages * 0x1000;
            if (size == 0 || descriptor.vaBase + size < descriptor.vaBase)
            {
                continue;
            }

            platform::MappedRegion region;
            region.start      = descriptor.vaBase;
            region.end        = descriptor.vaBase + size;
            region.readable   = protection.readable;
            region.writable   = protection.writable;
            region.executable = protection.executable;
            region.path       = descriptor.text;
            result.push_back(std::move(region));
        }
        if (mapped->regions.truncated)
        {
            slopkit::log::warning(slopkit::log::category::process,
                                  std::format("mpfshost truncated the page table of {} at {} entries",
                                              guest->name,
                                              mapped->regions.size()));
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

        auto pool  = slopkit::plugins::mpfshost::sessions_for(guest->target);
        auto lease = pool->acquire();
        if (!lease)
        {
            return std::unexpected(fault(SLOPKIT_ERR_IO, lease.error()));
        }

        const auto descriptors = lease->session().threads(guest->pid);
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
        if (descriptors->truncated)
        {
            slopkit::log::warning(slopkit::log::category::process,
                                  std::format("mpfshost truncated the thread list of {} at {} entries",
                                              guest->name,
                                              descriptors->size()));
        }
        return result;
    }

    bool region_readable(const ::mpfshost::VirtualRegion& region) noexcept
    {
        constexpr std::uint32_t kMemCommit    = 0x1000;
        constexpr std::uint32_t kPageNoAccess = 0x01;
        return (region.state & kMemCommit) != 0 && (region.protect & kPageNoAccess) == 0 && region.regionSize != 0;
    }

    std::expected<bool, AccessFault> range_is_readable(Session& session, std::uint64_t address, std::size_t size)
    {
        const auto guest = guest_for(session);
        if (!guest)
        {
            return std::unexpected(fault(SLOPKIT_ERR_NOT_FOUND, "the guest process is not known"));
        }

        if (size == 0)
        {
            return true;
        }
        if (address > UINT64_MAX - size)
        {
            return false;
        }

        auto pool  = slopkit::plugins::mpfshost::sessions_for(guest->target);
        auto lease = pool->acquire();
        if (!lease)
        {
            return std::unexpected(fault(SLOPKIT_ERR_IO, lease.error()));
        }

        const std::uint64_t end    = address + size;
        std::uint64_t       cursor = address;
        while (cursor < end)
        {
            const auto region = lease->session().queryVirtual(guest->pid, cursor);
            if (!region)
            {
                if (region.error().code == ::mpfshost::ErrorCode::CapabilityUnavailable
                    || region.error().code == ::mpfshost::ErrorCode::UnknownMessage)
                {
                    // Older hosts do not answer virtual queries; fall back to a direct read probe.
                    constexpr std::size_t  kProbeSize = 64 * 1024;
                    std::vector<std::byte> buffer(std::min<std::size_t>(kProbeSize, end - cursor));
                    const auto             filled = lease->session().readMemoryPartial(guest->pid, cursor, buffer);
                    if (!filled)
                    {
                        return false;
                    }
                    if (*filled < buffer.size())
                    {
                        return false;
                    }
                    cursor += *filled;
                    continue;
                }
                return false;
            }
            if (!region_readable(*region))
            {
                return false;
            }

            const std::uint64_t region_end = region->baseAddress + region->regionSize;
            if (region_end <= cursor)
            {
                return false;
            }
            cursor = std::min(end, region_end);
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
        return g_debug.interrupt(*guest, session.pid);
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
