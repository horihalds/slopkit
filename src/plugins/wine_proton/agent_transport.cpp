#include "plugins/wine_proton/agent_transport.hpp"

#include <algorithm>
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
        constexpr auto kHeartbeatInterval = std::chrono::milliseconds(2);
    }

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
        /* Announce the debug block so the agent refuses one it cannot parse
           instead of aliasing it. */
        region->debug.offset       = static_cast<std::uint32_t>(offsetof(slopkit_agent_region, debug));
        region->debug.size         = static_cast<std::uint32_t>(sizeof(slopkit_agent_debug));
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
          region_(std::exchange(other.region_, nullptr)), pool_size_(other.pool_size_), available_(other.available_),
          retired_in_pool_(other.retired_in_pool_), in_use_(other.in_use_), retired_(other.retired_),
          request_id_(other.request_id_.load()), heartbeat_(other.heartbeat_.load())
    {
    }

    AgentTransport& AgentTransport::operator=(AgentTransport&& other) noexcept
    {
        if (this != &other)
        {
            this->~AgentTransport();
            directory_       = std::move(other.directory_);
            descriptor_      = std::exchange(other.descriptor_, -1);
            region_          = std::exchange(other.region_, nullptr);
            pool_size_       = other.pool_size_;
            available_       = other.available_;
            retired_in_pool_ = other.retired_in_pool_;
            in_use_          = other.in_use_;
            retired_         = other.retired_;
            request_id_.store(other.request_id_.load());
            heartbeat_.store(other.heartbeat_.load());
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

    slopkit_agent_slot& AgentTransport::slot(std::uint32_t index) noexcept
    {
        return region_->slots[index];
    }

    const slopkit_agent_slot& AgentTransport::slot(std::uint32_t index) const noexcept
    {
        return region_->slots[index];
    }

    std::uint32_t AgentTransport::load_slot_state(std::uint32_t index) const noexcept
    {
        return std::atomic_ref<std::uint32_t>(region_->slots[index].state).load(std::memory_order_acquire);
    }

    void AgentTransport::store_slot_state(std::uint32_t index, std::uint32_t value) noexcept
    {
        std::atomic_ref<std::uint32_t>(region_->slots[index].state).store(value, std::memory_order_release);
    }

    std::uint32_t AgentTransport::slot_count() const
    {
        const std::lock_guard lock(mutex_);
        return pool_size_;
    }

    std::uint32_t AgentTransport::available_slots() const
    {
        const std::lock_guard lock(mutex_);
        return available_;
    }

    std::uint32_t AgentTransport::retired_slots() const
    {
        const std::lock_guard lock(mutex_);
        return retired_in_pool_;
    }

    bool AgentTransport::exhausted() const
    {
        const std::lock_guard lock(mutex_);
        return retired_in_pool_ >= pool_size_;
    }

    slopkit_agent_region* AgentTransport::region() const noexcept
    {
        return region_;
    }

    void AgentTransport::advertise_capacity(std::uint32_t slots) noexcept
    {
        const std::lock_guard lock(mutex_);
        pool_size_       = std::clamp(slots, 1u, static_cast<std::uint32_t>(SLOPKIT_AGENT_SLOT_COUNT));
        available_       = 0;
        retired_in_pool_ = 0;
        for (std::uint32_t index = 0; index < SLOPKIT_AGENT_SLOT_COUNT; ++index)
        {
            in_use_[index]  = false;
            retired_[index] = index >= pool_size_;
        }
        for (std::uint32_t index = 0; index < pool_size_; ++index)
        {
            ++available_;
        }
        ready_.notify_all();
    }

    std::expected<AgentTransport::Slot, std::string> AgentTransport::acquire(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        const auto       deadline = std::chrono::steady_clock::now() + timeout;
        while (true)
        {
            for (std::uint32_t index = 0; index < pool_size_; ++index)
            {
                if (!in_use_[index] && !retired_[index])
                {
                    in_use_[index] = true;
                    --available_;
                    return Slot(*this, index);
                }
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return std::unexpected(std::format("no agent slot came free within {} ms", timeout.count()));
            }
            ready_.wait_until(lock, deadline);
        }
    }

    void AgentTransport::release(std::uint32_t index) noexcept
    {
        const std::lock_guard lock(mutex_);
        if (index >= pool_size_ || retired_[index] || !in_use_[index])
        {
            return;
        }
        in_use_[index] = false;
        ++available_;
        ready_.notify_one();
    }

    void AgentTransport::retire(std::uint32_t index) noexcept
    {
        const std::lock_guard lock(mutex_);
        if (index >= pool_size_ || retired_[index])
        {
            return;
        }
        retired_[index] = true;
        in_use_[index]  = false;
        ++retired_in_pool_;
        ready_.notify_all();
    }

    AgentTransport::Slot::Slot(AgentTransport& transport, std::uint32_t index) noexcept
        : transport_(&transport), index_(index)
    {
    }

    AgentTransport::Slot::Slot(Slot&& other) noexcept
        : transport_(std::exchange(other.transport_, nullptr)), index_(other.index_)
    {
    }

    AgentTransport::Slot& AgentTransport::Slot::operator=(Slot&& other) noexcept
    {
        if (this != &other)
        {
            if (transport_ != nullptr)
            {
                transport_->release(index_);
            }
            transport_ = std::exchange(other.transport_, nullptr);
            index_     = other.index_;
        }
        return *this;
    }

    AgentTransport::Slot::~Slot()
    {
        if (transport_ != nullptr)
        {
            transport_->release(index_);
        }
    }

    std::uint32_t AgentTransport::Slot::index() const noexcept
    {
        return index_;
    }

    void AgentTransport::Slot::retire() noexcept
    {
        if (transport_ != nullptr)
        {
            transport_->retire(index_);
        }
    }

    std::expected<slopkit_agent_record, std::string>
    AgentTransport::Slot::transact(const slopkit_agent_record& request,
                                   std::chrono::milliseconds   timeout,
                                   std::span<const std::byte>  request_payload,
                                   std::span<std::byte>        response_payload)
    {
        if (transport_ == nullptr)
        {
            return std::unexpected(std::string("the agent slot is not held"));
        }
        AgentTransport&     owner  = *transport_;
        slopkit_agent_slot& target = owner.region_->slots[index_];

        slopkit_agent_record outgoing = request;
        outgoing.request_id           = owner.request_id_.fetch_add(1, std::memory_order_relaxed) + 1;

        if (!request_payload.empty())
        {
            const auto count = std::min(request_payload.size(), sizeof(target.payload));
            std::memcpy(target.payload, request_payload.data(), count);
        }

        target.request = outgoing;
        std::atomic_ref<std::uint32_t>(owner.region_->header.heartbeat)
            .store(owner.heartbeat_.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        std::atomic_ref<std::uint32_t>(target.state).store(SLOPKIT_AGENT_SLOT_REQUEST, std::memory_order_release);

        const auto deadline       = std::chrono::steady_clock::now() + timeout;
        auto       next_heartbeat = std::chrono::steady_clock::now();
        while (true)
        {
            if (std::atomic_ref<std::uint32_t>(target.state).load(std::memory_order_acquire)
                == SLOPKIT_AGENT_SLOT_RESPONSE)
            {
                const slopkit_agent_record response = target.response;
                if (!response_payload.empty())
                {
                    const auto count = std::min(response_payload.size(), sizeof(target.payload));
                    std::memcpy(response_payload.data(), target.payload, count);
                }
                std::atomic_ref<std::uint32_t>(target.state).store(SLOPKIT_AGENT_SLOT_IDLE, std::memory_order_release);
                if (response.request_id != outgoing.request_id)
                {
                    retire();
                    return std::unexpected(std::format(
                        "the agent answered request {} instead of {}", response.request_id, outgoing.request_id));
                }
                return response;
            }

            const auto flags =
                std::atomic_ref<std::uint32_t>(owner.region_->header.flags).load(std::memory_order_acquire);
            if ((flags & SLOPKIT_AGENT_FLAG_PROTOCOL_MISMATCH) != 0)
            {
                retire();
                return std::unexpected(std::string("the agent reported a protocol mismatch"));
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                retire();
                return std::unexpected(std::format("the agent did not answer within {} ms", timeout.count()));
            }
            if (now >= next_heartbeat)
            {
                std::atomic_ref<std::uint32_t>(owner.region_->header.heartbeat)
                    .store(owner.heartbeat_.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                next_heartbeat = now + kHeartbeatInterval;
            }
            std::this_thread::sleep_for(kPollInterval);
        }
    }

}
