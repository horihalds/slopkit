#include "plugins/wine_proton/agent_transport.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <format>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "plugins/wine_proton/wine_target.hpp"

namespace slopkit::plugins::wine_proton
{
    namespace
    {
        constexpr auto kPollInterval      = std::chrono::microseconds(250);
        // Bumping the heartbeat at most this often is enough to keep an idle
        // agent's sleep backoff short while a request is in flight, without
        // turning the wait loop into a busy one.
        constexpr auto kHeartbeatInterval = std::chrono::milliseconds(2);
    } // namespace

    std::expected<AgentTransport, std::string> AgentTransport::create(const std::filesystem::path& directory)
    {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
        {
            return std::unexpected(
                std::format("cannot create the agent directory {}: {}", directory.string(), error.message()));
        }

        const auto path       = directory / SLOPKIT_AGENT_SHM_FILE;
        const int  descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (descriptor < 0)
        {
            return std::unexpected(
                std::format("cannot create the agent region {}: {}", path.string(), std::strerror(errno)));
        }

        const auto region_size = static_cast<off_t>(sizeof(slopkit_agent_region));
        if (::ftruncate(descriptor, region_size) != 0)
        {
            const std::string reason = std::strerror(errno);
            ::close(descriptor);
            std::filesystem::remove(path, error);
            return std::unexpected(std::format("cannot size the agent region {}: {}", path.string(), reason));
        }

        void* const mapped =
            ::mmap(nullptr, sizeof(slopkit_agent_region), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
        if (mapped == MAP_FAILED)
        {
            const std::string reason = std::strerror(errno);
            ::close(descriptor);
            std::filesystem::remove(path, error);
            return std::unexpected(std::format("cannot map the agent region {}: {}", path.string(), reason));
        }

        auto* const region = static_cast<slopkit_agent_region*>(mapped);
        std::memset(region, 0, sizeof(*region));
        region->header.magic       = SLOPKIT_AGENT_MAGIC;
        region->header.version     = SLOPKIT_AGENT_PROTOCOL_VERSION;
        region->header.record_size = static_cast<std::uint32_t>(sizeof(slopkit_agent_record));
        region->header.flags       = SLOPKIT_AGENT_FLAG_NONE;
        region->header.state       = SLOPKIT_AGENT_STATE_IDLE;
        return AgentTransport(directory, descriptor, region);
    }

    AgentTransport::AgentTransport(std::filesystem::path directory,
                                   int                   descriptor,
                                   slopkit_agent_region* region) noexcept
        : directory_(std::move(directory)), descriptor_(descriptor), region_(region)
    {
    }

    AgentTransport::AgentTransport(AgentTransport&& other) noexcept
        : directory_(std::move(other.directory_)), descriptor_(std::exchange(other.descriptor_, -1)),
          region_(std::exchange(other.region_, nullptr)), request_id_(other.request_id_), heartbeat_(other.heartbeat_)
    {
    }

    AgentTransport& AgentTransport::operator=(AgentTransport&& other) noexcept
    {
        if (this != &other)
        {
            this->~AgentTransport();
            directory_  = std::move(other.directory_);
            descriptor_ = std::exchange(other.descriptor_, -1);
            region_     = std::exchange(other.region_, nullptr);
            request_id_ = other.request_id_;
            heartbeat_  = other.heartbeat_;
        }
        return *this;
    }

    AgentTransport::~AgentTransport()
    {
        if (region_ != nullptr)
        {
            ::munmap(region_, sizeof(slopkit_agent_region));
        }
        if (descriptor_ >= 0)
        {
            ::close(descriptor_);
        }
        if (!directory_.empty())
        {
            // The per-session directory holds the region, the DLL and the
            // injector; all three are ours to remove. A DLL still mapped by the
            // target keeps working from the unlinked inode.
            std::error_code error;
            std::filesystem::remove_all(directory_, error);
        }
    }

    const std::filesystem::path& AgentTransport::directory() const noexcept
    {
        return directory_;
    }

    std::string AgentTransport::windows_directory() const
    {
        return linux_path_to_windows(directory_.string());
    }

    const slopkit_agent_header& AgentTransport::header() const noexcept
    {
        return region_->header;
    }

    std::uint32_t AgentTransport::load_state() const noexcept
    {
        return std::atomic_ref<std::uint32_t>(region_->header.state).load(std::memory_order_acquire);
    }

    void AgentTransport::store_state(std::uint32_t value) noexcept
    {
        std::atomic_ref<std::uint32_t>(region_->header.state).store(value, std::memory_order_release);
    }

    std::expected<slopkit_agent_record, std::string> AgentTransport::transact(const slopkit_agent_record& request,
                                                                              std::chrono::milliseconds   timeout)
    {
        slopkit_agent_record outgoing = request;
        outgoing.request_id           = ++request_id_;

        region_->request          = outgoing;
        region_->header.heartbeat = ++heartbeat_;
        store_state(SLOPKIT_AGENT_STATE_REQUEST);

        const auto deadline       = std::chrono::steady_clock::now() + timeout;
        auto       next_heartbeat = std::chrono::steady_clock::now();
        while (true)
        {
            if (load_state() == SLOPKIT_AGENT_STATE_RESPONSE)
            {
                const slopkit_agent_record response = region_->response;
                store_state(SLOPKIT_AGENT_STATE_IDLE);
                if (response.request_id != outgoing.request_id)
                {
                    return std::unexpected(std::format(
                        "the agent answered request {} instead of {}", response.request_id, outgoing.request_id));
                }
                return response;
            }

            const auto flags = std::atomic_ref<std::uint32_t>(region_->header.flags).load(std::memory_order_acquire);
            if ((flags & SLOPKIT_AGENT_FLAG_PROTOCOL_MISMATCH) != 0)
            {
                return std::unexpected(std::string("the agent reported a protocol mismatch"));
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                return std::unexpected(std::format("the agent did not answer within {} ms", timeout.count()));
            }
            if (now >= next_heartbeat)
            {
                region_->header.heartbeat = ++heartbeat_;
                next_heartbeat            = now + kHeartbeatInterval;
            }
            std::this_thread::sleep_for(kPollInterval);
        }
    }

} // namespace slopkit::plugins::wine_proton
