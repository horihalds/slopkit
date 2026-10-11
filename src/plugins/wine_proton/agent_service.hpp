#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "plugins/support/allocate.hpp"
#include "plugins/support/memory.hpp"
#include "plugins/support/session.hpp"
#include "process/types.hpp"

namespace slopkit::plugins::wine_proton
{
    class AgentService
    {
    public:
        [[nodiscard]] static std::expected<std::uint64_t, support::AllocationError>
        allocate(support::Session& session, std::uint64_t size, std::uint64_t near_address);

        [[nodiscard]] static std::expected<void, support::AllocationError> free(support::Session& session,
                                                                                std::uint64_t     address);

        [[nodiscard]] static std::expected<std::vector<platform::MappedRegion>, support::AccessFault>
        regions(support::Session& session);

        [[nodiscard]] static std::expected<std::vector<process::ModuleInfo>, support::AccessFault>
        modules(support::Session& session);

        static void close(process::ProcessId pid);

        [[nodiscard]] static std::vector<process::ProcessId> agent_threads(support::Session& session);

    private:
        AgentService() = delete;
    };

}
