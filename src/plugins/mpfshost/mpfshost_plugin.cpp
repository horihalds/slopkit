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
#include "plugins/mpfshost/mpfshost_helpers.hpp"
#include "plugins/support/plugin_support.hpp"
#include "plugins/support/session.hpp"

namespace
{
    using slopkit::plugins::mpfshost::Client;
    using slopkit::plugins::mpfshost::GuestTarget;
    using slopkit::plugins::mpfshost::TargetTable;
    using slopkit::plugins::mpfshost::TransportError;
    using slopkit::plugins::support::AccessFault;
    using slopkit::plugins::support::MemoryResult;
    using slopkit::plugins::support::PluginProfile;
    using slopkit::plugins::support::Session;
    using slopkit::process::ModuleInfo;
    using slopkit::process::ModuleKind;
    using slopkit::process::ProcessId;
    using slopkit::process::ProcessInfo;
    using slopkit::process::ThreadInfo;

    constexpr std::size_t kChunkSize = 64u << 20;

    const slopkit_plugin_info kInfo {
        "mpfshost",
        "MemProcFS guest access",
        "0.1.0",
        "Attaches to processes inside QEMU/KVM guests through the system mpfshost daemon; reads, writes, "
        "modules, threads, regions and validation work, while debugging, suspend/resume and allocation do not."};

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

    TargetTable g_targets;

    AccessFault fault(std::int32_t code, std::string message)
    {
        return AccessFault {code, std::move(message)};
    }

    AccessFault client_fault(const TransportError& error)
    {
        return fault(error.code, error.message);
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

        const auto targets = client.list_targets();
        if (!targets)
        {
            return std::vector<ProcessInfo> {};
        }

        std::vector<ProcessInfo> result;
        for (const auto& target : *targets)
        {
            const auto processes = client.list_processes(target.name);
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

        const Client client {slopkit::plugins::mpfshost::socket_path()};
        std::size_t  offset = 0;
        while (offset < buffer.size())
        {
            const auto wanted = std::min(kChunkSize, buffer.size() - offset);

            const auto data =
                client.read_memory(guest->target, guest->pid, address + offset, static_cast<std::uint32_t>(wanted));
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

        const Client client {slopkit::plugins::mpfshost::socket_path()};
        std::size_t  offset = 0;
        while (offset < data.size())
        {
            const auto chunk = std::min(kChunkSize, data.size() - offset);
            const auto span  = data.subspan(offset, chunk);

            const auto written = client.write_memory(guest->target, guest->pid, address + offset, span);
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

        const Client client {slopkit::plugins::mpfshost::socket_path()};
        const auto   descriptors = client.list_modules(guest->target, guest->pid);
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

        const Client client      = Client {slopkit::plugins::mpfshost::socket_path()};
        const auto   descriptors = client.list_pte(guest->target, guest->pid);
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

        const Client client      = Client {slopkit::plugins::mpfshost::socket_path()};
        const auto   descriptors = client.list_threads(guest->target, guest->pid);
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

        constexpr std::size_t kProbeSize = 64 * 1024;
        const Client          client {slopkit::plugins::mpfshost::socket_path()};

        std::size_t offset = 0;
        while (offset < size)
        {
            const auto wanted = std::min(kProbeSize, size - offset);
            const auto data =
                client.read_memory(guest->target, guest->pid, address + offset, static_cast<std::uint32_t>(wanted));
            if (!data || data->size() < wanted)
            {
                return false;
            }
            offset += wanted;
        }
        return true;
    }

    void close_session(Session&) {}

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
                                      false,
                                      false,
                                      false};
}

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host)
{
    return slopkit::plugins::support::entry<kProfile>(host);
}
