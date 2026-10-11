#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <latch>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "plugins/wine_proton/agent/slopkit_agent_protocol.h"
#include "plugins/wine_proton/agent_transport.hpp"

namespace
{
    using slopkit::plugins::wine_proton::AgentTransport;

    using namespace std::chrono_literals;

    std::expected<slopkit_agent_record, std::string> transact(AgentTransport&             transport,
                                                              const slopkit_agent_record& record,
                                                              std::chrono::milliseconds   timeout,
                                                              std::span<const std::byte>  request_payload  = {},
                                                              std::span<std::byte>        response_payload = {})
    {
        auto slot = transport.acquire(timeout);
        if (!slot)
        {
            return std::unexpected(slot.error());
        }
        return slot->transact(record, timeout, request_payload, response_payload);
    }

    class MockAgent
    {
    public:
        enum class Behaviour
        {
            Serve,
            Silent,
            SilentFirst,
            ProtocolMismatch,
        };

        MockAgent(const std::filesystem::path& directory, Behaviour behaviour, std::chrono::milliseconds hold = 0ms)
            : behaviour_(behaviour), hold_(hold)
        {
            const auto path = directory / SLOPKIT_AGENT_SHM_FILE;
            descriptor_     = ::open(path.c_str(), O_RDWR);
            REQUIRE(descriptor_ >= 0);

            void* mapped =
                ::mmap(nullptr, sizeof(slopkit_agent_region), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor_, 0);
            REQUIRE(mapped != MAP_FAILED);
            region_ = static_cast<slopkit_agent_region*>(mapped);

            if (behaviour_ == Behaviour::ProtocolMismatch)
            {
                std::atomic_ref<std::uint32_t>(region_->header.flags)
                    .fetch_or(SLOPKIT_AGENT_FLAG_PROTOCOL_MISMATCH, std::memory_order_release);
            }
            region_->header.slot_count   = SLOPKIT_AGENT_SLOT_COUNT;
            region_->header.worker_count = SLOPKIT_AGENT_MAX_WORKERS;

            target_memory_.resize(0x2000);
            for (std::size_t index = 0; index < target_memory_.size(); ++index)
            {
                target_memory_[index] = static_cast<std::byte>(index & 0xFF);
            }

            if (behaviour_ == Behaviour::Serve || behaviour_ == Behaviour::SilentFirst)
            {
                for (std::uint32_t index = 0; index < SLOPKIT_AGENT_SLOT_COUNT; ++index)
                {
                    workers_[index] = std::jthread(
                        [this, index]
                        {
                            serve_slot(index);
                        });
                }
            }
        }

        MockAgent(const MockAgent&)            = delete;
        MockAgent& operator=(const MockAgent&) = delete;

        ~MockAgent()
        {
            stop_.store(true);
            for (auto& worker : workers_)
            {
                if (worker.joinable())
                {
                    worker.join();
                }
            }
            if (region_ != nullptr)
            {
                ::munmap(region_, sizeof(slopkit_agent_region));
            }
            if (descriptor_ >= 0)
            {
                ::close(descriptor_);
            }
        }

        [[nodiscard]] const slopkit_agent_record& last_request() const
        {
            return last_request_;
        }

        [[nodiscard]] std::uint32_t served() const
        {
            return served_.load();
        }

        [[nodiscard]] int peak_in_flight() const
        {
            return peak_in_flight_.load();
        }

        [[nodiscard]] int in_flight() const
        {
            return in_flight_.load();
        }

        [[nodiscard]] std::vector<std::byte>& target_memory()
        {
            return target_memory_;
        }

        [[nodiscard]] static constexpr std::uint64_t target_base()
        {
            return 0x10000000ull;
        }

        [[nodiscard]] std::vector<slopkit_agent_region_entry> default_regions() const
        {
            std::vector<slopkit_agent_region_entry> regions(3);
            for (std::uint32_t index = 0; index < regions.size(); ++index)
            {
                regions[index].base       = 0x10000000ull + index * 0x1000ull;
                regions[index].size       = 0x1000;
                regions[index].state      = SLOPKIT_AGENT_MEM_COMMIT;
                regions[index].protection = SLOPKIT_AGENT_PAGE_READWRITE;
                regions[index].type       = SLOPKIT_AGENT_MEM_PRIVATE;
            }
            return regions;
        }

    private:
        [[nodiscard]] bool owned(std::uint64_t address) const
        {
            for (const auto base : owned_)
            {
                if (base == address)
                {
                    return true;
                }
            }
            return false;
        }

        slopkit_agent_record answer(const slopkit_agent_record& request, std::uint8_t* payload)
        {
            slopkit_agent_record response {};
            response.op         = request.op;
            response.request_id = request.request_id;
            response.status     = SLOPKIT_AGENT_STATUS_OK;

            switch (request.op)
            {
            case SLOPKIT_AGENT_OP_HELLO:
                response.protocol_version = SLOPKIT_AGENT_PROTOCOL_VERSION;
                response.record_size      = static_cast<std::uint32_t>(sizeof(slopkit_agent_record));
                response.pointer_size     = static_cast<std::uint32_t>(sizeof(void*));
                response.windows_pid      = 4242;
                response.granularity      = 0x10000;
                response.agent_base       = 0x7fff00000000ull;
                response.slot_count       = SLOPKIT_AGENT_SLOT_COUNT;
                response.worker_count     = SLOPKIT_AGENT_MAX_WORKERS;
                response.reserved0        = SLOPKIT_AGENT_PAYLOAD_SIZE;
                break;
            case SLOPKIT_AGENT_OP_ALLOC:
                if (request.size == 0)
                {
                    response.status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
                    break;
                }
                response.address = next_base_;
                response.size    = request.size;
                owned_.push_back(next_base_);
                next_base_ += 0x10000;
                break;
            case SLOPKIT_AGENT_OP_ALLOC_NEAR:
                if (request.size == 0 || request.hint == 0)
                {
                    response.status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
                    break;
                }
                response.address = request.hint & ~0xffffull;
                response.size    = request.size;
                owned_.push_back(response.address);
                break;
            case SLOPKIT_AGENT_OP_FREE:
                if (!owned(request.address))
                {
                    response.status      = SLOPKIT_AGENT_STATUS_NOT_OWNED;
                    response.win32_error = 0x1E7;
                    break;
                }
                std::erase(owned_, request.address);
                break;
            case SLOPKIT_AGENT_OP_PROTECT:
                if (request.size == 0)
                {
                    response.status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
                    break;
                }
                if (!owned(request.address))
                {
                    response.status      = SLOPKIT_AGENT_STATUS_NOT_OWNED;
                    response.win32_error = 0x1E7;
                    break;
                }
                response.protection = SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE;
                break;
            case SLOPKIT_AGENT_OP_QUERY:
                if (request.address == 0)
                {
                    response.status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
                    break;
                }
                response.region_base    = request.address & ~0xfffull;
                response.region_size    = 0x1000;
                response.region_state   = SLOPKIT_AGENT_MEM_COMMIT;
                response.region_protect = SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE;
                break;
            case SLOPKIT_AGENT_OP_ENUM:
            {
                const auto capacity =
                    static_cast<std::uint32_t>(SLOPKIT_AGENT_PAYLOAD_SIZE / sizeof(slopkit_agent_region_entry));
                if (request.size > capacity)
                {
                    response.status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
                    break;
                }
                const auto  regions = default_regions();
                const auto  want    = request.size == 0 ? capacity : static_cast<std::uint32_t>(request.size);
                auto*       entries = reinterpret_cast<slopkit_agent_region_entry*>(payload);
                std::size_t index   = 0;
                while (index < regions.size() && regions[index].base < request.address)
                {
                    ++index;
                }
                std::uint32_t written = 0;
                while (index < regions.size() && written < want)
                {
                    entries[written++] = regions[index++];
                }
                response.size    = written;
                response.address = index < regions.size() ? regions[index].base : 0;
                break;
            }
            case SLOPKIT_AGENT_OP_READ:
            {
                if (request.size == 0 || request.size > SLOPKIT_AGENT_PAYLOAD_SIZE)
                {
                    response.status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
                    break;
                }
                if (request.address < target_base() || request.address >= target_base() + target_memory_.size())
                {
                    response.status      = SLOPKIT_AGENT_STATUS_ERROR;
                    response.win32_error = 0x1E7;
                    break;
                }
                const auto offset    = static_cast<std::size_t>(request.address - target_base());
                const auto available = target_memory_.size() - offset;
                const auto span      = std::min<std::size_t>({static_cast<std::size_t>(request.size), available});
                std::memcpy(payload, target_memory_.data() + offset, span);
                response.size = span;
                break;
            }
            case SLOPKIT_AGENT_OP_WRITE:
            {
                if (request.size == 0 || request.size > SLOPKIT_AGENT_PAYLOAD_SIZE)
                {
                    response.status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
                    break;
                }
                if (request.address < target_base() || request.address >= target_base() + target_memory_.size())
                {
                    response.status      = SLOPKIT_AGENT_STATUS_ERROR;
                    response.win32_error = 0x1E7;
                    break;
                }
                const auto offset    = static_cast<std::size_t>(request.address - target_base());
                const auto available = target_memory_.size() - offset;
                const auto span      = std::min<std::size_t>({static_cast<std::size_t>(request.size), available});
                std::memcpy(target_memory_.data() + offset, payload, span);
                response.size = span;
                break;
            }
            case SLOPKIT_AGENT_OP_SHUTDOWN:
                owned_.clear();
                break;
            default:
                response.status = SLOPKIT_AGENT_STATUS_UNKNOWN_OP;
                break;
            }
            return response;
        }

        void serve_slot(std::uint32_t index)
        {
            while (!stop_.load())
            {
                auto& state = region_->slots[index].state;
                if (std::atomic_ref<std::uint32_t>(state).load(std::memory_order_acquire) != SLOPKIT_AGENT_SLOT_REQUEST)
                {
                    std::this_thread::sleep_for(50us);
                    continue;
                }

                slopkit_agent_record request {};
                bool                 skip = false;
                {
                    const std::lock_guard lock(serve_mutex_);
                    request       = region_->slots[index].request;
                    last_request_ = request;
                    // SilentFirst abandons exactly its first request and leaves
                    // the slot in the request state; the host times out on it
                    // and must never read a later request's answer out of it.
                    if (behaviour_ == Behaviour::SilentFirst)
                    {
                        if (!ignored_seen_)
                        {
                            ignored_seen_       = true;
                            ignored_request_id_ = request.request_id;
                        }
                        skip = request.request_id == ignored_request_id_;
                    }
                }
                if (skip)
                {
                    std::this_thread::sleep_for(50us);
                    continue;
                }

                const int now      = in_flight_.fetch_add(1) + 1;
                int       observed = peak_in_flight_.load();
                while (now > observed && !peak_in_flight_.compare_exchange_weak(observed, now))
                {
                }

                if (hold_ > 0ms)
                {
                    std::this_thread::sleep_for(hold_);
                }

                slopkit_agent_record response;
                {
                    const std::lock_guard lock(serve_mutex_);
                    response = answer(request, region_->slots[index].payload);
                }

                const bool stopping = request.op == SLOPKIT_AGENT_OP_SHUTDOWN;
                if (stopping)
                {
                    std::atomic_ref<std::uint32_t>(region_->header.flags)
                        .fetch_or(SLOPKIT_AGENT_FLAG_STOPPED, std::memory_order_release);
                }
                region_->slots[index].response = response;
                served_.fetch_add(1);
                std::atomic_ref<std::uint32_t>(state).store(SLOPKIT_AGENT_SLOT_RESPONSE, std::memory_order_release);
                in_flight_.fetch_sub(1);
                if (stopping)
                {
                    stop_.store(true);
                }
            }
        }

        Behaviour                                          behaviour_ {Behaviour::Serve};
        std::chrono::milliseconds                          hold_ {0};
        int                                                descriptor_ {-1};
        slopkit_agent_region*                              region_ {nullptr};
        std::array<std::jthread, SLOPKIT_AGENT_SLOT_COUNT> workers_;
        std::mutex                                         serve_mutex_;
        std::atomic<bool>                                  stop_ {false};
        std::atomic<std::uint32_t>                         served_ {0};
        std::atomic<int>                                   in_flight_ {0};
        std::atomic<int>                                   peak_in_flight_ {0};
        slopkit_agent_record                               last_request_ {};
        std::vector<std::uint64_t>                         owned_;
        std::uint64_t                                      next_base_ {0x140000000ull};
        std::vector<std::byte>                             target_memory_;
        bool                                               ignored_seen_ {false};
        std::uint32_t                                      ignored_request_id_ {0};
    };

    std::filesystem::path scratch_directory(std::string_view name)
    {
        const auto      directory = std::filesystem::path(SLOPKIT_TMP_DIR) / std::string(name);
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return directory;
    }

    slopkit_agent_record request(std::uint32_t op)
    {
        slopkit_agent_record record {};
        record.op = op;
        return record;
    }
}

TEST_CASE("the agent transport handshakes and echoes the request id", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-handshake"));
    REQUIRE(transport.has_value());

    MockAgent agent(transport->directory(), MockAgent::Behaviour::Serve);

    const auto hello = transact(*transport, request(SLOPKIT_AGENT_OP_HELLO), 2s);
    REQUIRE(hello.has_value());
    CHECK(hello->status == SLOPKIT_AGENT_STATUS_OK);
    CHECK(hello->op == SLOPKIT_AGENT_OP_HELLO);
    CHECK(hello->request_id == 1u);
    CHECK(hello->protocol_version == SLOPKIT_AGENT_PROTOCOL_VERSION);
    CHECK(hello->record_size == sizeof(slopkit_agent_record));
    CHECK(hello->pointer_size == sizeof(void*));
    CHECK(hello->windows_pid == 4242u);
    CHECK(hello->granularity == 0x10000u);
    CHECK(hello->agent_base == 0x7fff00000000ull);
    CHECK(hello->slot_count == SLOPKIT_AGENT_SLOT_COUNT);
    CHECK(hello->worker_count == SLOPKIT_AGENT_MAX_WORKERS);

    CHECK(transport->load_slot_state(0) == SLOPKIT_AGENT_SLOT_IDLE);
    const auto again = transact(*transport, request(SLOPKIT_AGENT_OP_HELLO), 2s);
    REQUIRE(again.has_value());
    CHECK(again->request_id == 2u);
}

TEST_CASE("the slot pool is sized by the advertised capacity", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-capacity"));
    REQUIRE(transport.has_value());

    CHECK(transport->slot_count() == 1u);
    CHECK(transport->available_slots() == 1u);

    MockAgent agent(transport->directory(), MockAgent::Behaviour::Serve);

    const auto hello = transact(*transport, request(SLOPKIT_AGENT_OP_HELLO), 2s);
    REQUIRE(hello.has_value());
    transport->advertise_capacity(hello->slot_count);
    CHECK(transport->slot_count() == SLOPKIT_AGENT_SLOT_COUNT);
    CHECK(transport->available_slots() == SLOPKIT_AGENT_SLOT_COUNT);
    CHECK_FALSE(transport->exhausted());

    auto quiet = AgentTransport::create(scratch_directory("agent-transport-capacity-zero"));
    REQUIRE(quiet.has_value());
    quiet->advertise_capacity(0);
    CHECK(quiet->slot_count() == 1u);
    CHECK(quiet->available_slots() == 1u);
}

TEST_CASE("the agent transport carries every operation", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-ops"));
    REQUIRE(transport.has_value());

    MockAgent agent(transport->directory(), MockAgent::Behaviour::Serve);

    SECTION("alloc puts size and hint on the wire and returns the base")
    {
        auto alloc           = request(SLOPKIT_AGENT_OP_ALLOC);
        alloc.size           = 0x4321;
        alloc.hint           = 0x100000000ull;
        const auto allocated = transact(*transport, alloc, 2s);
        REQUIRE(allocated.has_value());
        CHECK(allocated->status == SLOPKIT_AGENT_STATUS_OK);
        CHECK(allocated->address != 0);
        CHECK(allocated->size == 0x4321u);
        CHECK(agent.last_request().op == SLOPKIT_AGENT_OP_ALLOC);
        CHECK(agent.last_request().size == 0x4321u);
        CHECK(agent.last_request().hint == 0x100000000ull);
    }

    SECTION("alloc_near answers inside the hint's 64 KB page")
    {
        auto near         = request(SLOPKIT_AGENT_OP_ALLOC_NEAR);
        near.size         = 0x2000;
        near.hint         = 0x7f1234567890ull;
        const auto placed = transact(*transport, near, 2s);
        REQUIRE(placed.has_value());
        CHECK(placed->address == 0x7f1234560000ull);
        CHECK(agent.last_request().hint == 0x7f1234567890ull);
    }

    SECTION("free releases an owned address and refuses a foreign one")
    {
        auto alloc           = request(SLOPKIT_AGENT_OP_ALLOC);
        alloc.size           = 0x1000;
        const auto allocated = transact(*transport, alloc, 2s);
        REQUIRE(allocated.has_value());

        auto release     = request(SLOPKIT_AGENT_OP_FREE);
        release.address  = allocated->address;
        const auto freed = transact(*transport, release, 2s);
        REQUIRE(freed.has_value());
        CHECK(freed->status == SLOPKIT_AGENT_STATUS_OK);

        release.address  = 0xdead0000ull;
        const auto stray = transact(*transport, release, 2s);
        REQUIRE(stray.has_value());
        CHECK(stray->status == SLOPKIT_AGENT_STATUS_NOT_OWNED);
        CHECK(stray->win32_error == 0x1E7u);
    }

    SECTION("protect returns the previous protection")
    {
        auto alloc           = request(SLOPKIT_AGENT_OP_ALLOC);
        alloc.size           = 0x1000;
        const auto allocated = transact(*transport, alloc, 2s);
        REQUIRE(allocated.has_value());

        auto protect       = request(SLOPKIT_AGENT_OP_PROTECT);
        protect.address    = allocated->address;
        protect.size       = 0x1000;
        protect.protection = SLOPKIT_AGENT_PAGE_READONLY;
        const auto changed = transact(*transport, protect, 2s);
        REQUIRE(changed.has_value());
        CHECK(changed->protection == SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE);
        CHECK(agent.last_request().protection == SLOPKIT_AGENT_PAGE_READONLY);
    }

    SECTION("query reports the target's view of a region")
    {
        auto query        = request(SLOPKIT_AGENT_OP_QUERY);
        query.address     = 0x7f00abcdull;
        const auto region = transact(*transport, query, 2s);
        REQUIRE(region.has_value());
        CHECK(region->region_base == 0x7f00a000ull);
        CHECK(region->region_size == 0x1000u);
        CHECK(region->region_state == SLOPKIT_AGENT_MEM_COMMIT);
        CHECK(region->region_protect == SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE);
    }

    SECTION("shutdown answers and marks the agent stopped")
    {
        const auto done = transact(*transport, request(SLOPKIT_AGENT_OP_SHUTDOWN), 2s);
        REQUIRE(done.has_value());
        CHECK(done->status == SLOPKIT_AGENT_STATUS_OK);
        CHECK((transport->header().flags & SLOPKIT_AGENT_FLAG_STOPPED) != 0);
    }
}

TEST_CASE("the agent transport carries a payload in both directions", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-payload"));
    REQUIRE(transport.has_value());

    MockAgent agent(transport->directory(), MockAgent::Behaviour::Serve);

    SECTION("enum returns a batch and the next cursor, and the walk ends at 0")
    {
        auto first    = request(SLOPKIT_AGENT_OP_ENUM);
        first.address = 0;
        first.size    = 2;
        std::vector<std::byte> payload(SLOPKIT_AGENT_PAYLOAD_SIZE);
        const auto             batch = transact(*transport, first, 2s, {}, payload);
        REQUIRE(batch.has_value());
        CHECK(batch->status == SLOPKIT_AGENT_STATUS_OK);
        CHECK(batch->size == 2u);
        CHECK(batch->address == 0x10002000ull);

        const auto* entries = reinterpret_cast<const slopkit_agent_region_entry*>(payload.data());
        CHECK(entries[0].base == 0x10000000ull);
        CHECK(entries[0].size == 0x1000u);
        CHECK(entries[0].state == SLOPKIT_AGENT_MEM_COMMIT);
        CHECK(entries[0].type == SLOPKIT_AGENT_MEM_PRIVATE);

        auto second     = request(SLOPKIT_AGENT_OP_ENUM);
        second.address  = batch->address;
        second.size     = 2;
        const auto tail = transact(*transport, second, 2s, {}, payload);
        REQUIRE(tail.has_value());
        CHECK(tail->size == 1u);
        CHECK(tail->address == 0ull);
    }

    SECTION("read returns the target's bytes and a short read is a success")
    {
        auto record    = request(SLOPKIT_AGENT_OP_READ);
        record.address = MockAgent::target_base() + 0x10;
        record.size    = 0x20;
        std::vector<std::byte> buffer(0x20);
        const auto             read = transact(*transport, record, 2s, {}, buffer);
        REQUIRE(read.has_value());
        CHECK(read->status == SLOPKIT_AGENT_STATUS_OK);
        CHECK(read->size == 0x20u);
        for (std::size_t index = 0; index < buffer.size(); ++index)
        {
            CHECK(buffer[index] == agent.target_memory()[0x10 + index]);
        }

        record.address = MockAgent::target_base() + agent.target_memory().size() - 0x10;
        record.size    = 0x100;
        std::vector<std::byte> short_buffer(0x100);
        const auto             short_read = transact(*transport, record, 2s, {}, short_buffer);
        REQUIRE(short_read.has_value());
        CHECK(short_read->status == SLOPKIT_AGENT_STATUS_OK);
        CHECK(short_read->size == 0x10u);
    }

    SECTION("write sends the payload into the target")
    {
        std::vector<std::byte> data(0x40);
        for (std::size_t index = 0; index < data.size(); ++index)
        {
            data[index] = static_cast<std::byte>(0xA0 + (index & 0xF));
        }
        auto record      = request(SLOPKIT_AGENT_OP_WRITE);
        record.address   = MockAgent::target_base() + 0x100;
        record.size      = data.size();
        const auto write = transact(*transport, record, 2s, data, {});
        REQUIRE(write.has_value());
        CHECK(write->status == SLOPKIT_AGENT_STATUS_OK);
        CHECK(write->size == data.size());
        CHECK(std::equal(data.begin(), data.end(), agent.target_memory().begin() + 0x100));
    }

    SECTION("an oversized size is a bad request")
    {
        auto enum_request = request(SLOPKIT_AGENT_OP_ENUM);
        enum_request.size = 99999;
        std::vector<std::byte> payload(SLOPKIT_AGENT_PAYLOAD_SIZE);
        const auto             refused = transact(*transport, enum_request, 2s, {}, payload);
        REQUIRE(refused.has_value());
        CHECK(refused->status == SLOPKIT_AGENT_STATUS_BAD_REQUEST);

        auto read_request    = request(SLOPKIT_AGENT_OP_READ);
        read_request.address = MockAgent::target_base();
        read_request.size    = SLOPKIT_AGENT_PAYLOAD_SIZE + 1;
        std::vector<std::byte> buffer(1);
        const auto             bad = transact(*transport, read_request, 2s, {}, buffer);
        REQUIRE(bad.has_value());
        CHECK(bad->status == SLOPKIT_AGENT_STATUS_BAD_REQUEST);
    }
}

TEST_CASE("several host threads transact on their own slots at once", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-concurrent"));
    REQUIRE(transport.has_value());
    transport->advertise_capacity(SLOPKIT_AGENT_SLOT_COUNT);

    MockAgent agent(transport->directory(), MockAgent::Behaviour::Serve, 60ms);

    constexpr std::size_t                        kReaders = 2;
    std::array<std::thread, kReaders>            readers;
    std::array<std::uint32_t, kReaders>          answered_ids {};
    std::array<std::vector<std::byte>, kReaders> results;
    std::latch                                   start(static_cast<std::ptrdiff_t>(kReaders) + 1);

    for (std::size_t index = 0; index < kReaders; ++index)
    {
        readers[index] = std::thread(
            [&, index]
            {
                results[index].assign(0x20, std::byte {0});
                auto record    = request(SLOPKIT_AGENT_OP_READ);
                record.address = MockAgent::target_base() + index * 0x40;
                record.size    = results[index].size();
                start.arrive_and_wait();
                const auto answer = transact(*transport, record, 5s, {}, results[index]);
                if (answer.has_value())
                {
                    answered_ids[index] = answer->request_id;
                }
            });
    }
    start.arrive_and_wait();
    for (auto& reader : readers)
    {
        reader.join();
    }

    CHECK(answered_ids[0] != 0u);
    CHECK(answered_ids[1] != 0u);
    CHECK(answered_ids[0] != answered_ids[1]);
    CHECK(results[0] == std::vector<std::byte>(agent.target_memory().begin(), agent.target_memory().begin() + 0x20));
    CHECK(results[1]
          == std::vector<std::byte>(agent.target_memory().begin() + 0x40, agent.target_memory().begin() + 0x60));

    CHECK(agent.peak_in_flight() > 1);
    CHECK(transport->available_slots() == SLOPKIT_AGENT_SLOT_COUNT);
}

TEST_CASE("a timed-out exchange retires its slot", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-retire"));
    REQUIRE(transport.has_value());

    MockAgent agent(transport->directory(), MockAgent::Behaviour::Silent);

    auto slot = transport->acquire(1s);
    REQUIRE(slot.has_value());
    const auto index = slot->index();

    const auto timed_out = slot->transact(request(SLOPKIT_AGENT_OP_HELLO), 150ms);
    REQUIRE_FALSE(timed_out.has_value());
    CHECK(timed_out.error().find("did not answer") != std::string::npos);

    CHECK(transport->retired_slots() == 1u);
    CHECK(transport->exhausted());
    CHECK(transport->load_slot_state(index) == SLOPKIT_AGENT_SLOT_REQUEST);
    const auto again = transport->acquire(100ms);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().find("no agent slot") != std::string::npos);
}

TEST_CASE("a retired slot does not take the surviving pool with it", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-retire-one"));
    REQUIRE(transport.has_value());
    transport->advertise_capacity(SLOPKIT_AGENT_SLOT_COUNT);

    MockAgent agent(transport->directory(), MockAgent::Behaviour::SilentFirst);

    const auto timed_out = transact(*transport, request(SLOPKIT_AGENT_OP_HELLO), 200ms);
    REQUIRE_FALSE(timed_out.has_value());
    CHECK(transport->retired_slots() == 1u);
    CHECK_FALSE(transport->exhausted());

    const auto recovered = transact(*transport, request(SLOPKIT_AGENT_OP_HELLO), 2s);
    REQUIRE(recovered.has_value());
    CHECK(recovered->status == SLOPKIT_AGENT_STATUS_OK);
    CHECK(recovered->reserved0 == SLOPKIT_AGENT_PAYLOAD_SIZE);
}

TEST_CASE("shutdown is answered while two slots are mid-exchange", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-shutdown-drain"));
    REQUIRE(transport.has_value());
    transport->advertise_capacity(SLOPKIT_AGENT_SLOT_COUNT);

    MockAgent agent(transport->directory(), MockAgent::Behaviour::Serve, 40ms);

    auto first  = transport->acquire(1s);
    auto second = transport->acquire(1s);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    std::atomic<bool> first_ok {false};
    std::atomic<bool> second_ok {false};
    std::jthread      one(
        [&]
        {
            first_ok = first->transact(request(SLOPKIT_AGENT_OP_HELLO), 5s).has_value();
        });
    std::jthread two(
        [&]
        {
            second_ok = second->transact(request(SLOPKIT_AGENT_OP_HELLO), 5s).has_value();
        });

    // Wait until both HELLOs are being served, then shut the agent down. The
    // drain has to let both finish before the region goes away.
    for (int waited = 0; waited < 200 && agent.in_flight() < 2; ++waited)
    {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(agent.in_flight() >= 2);

    const auto done = transact(*transport, request(SLOPKIT_AGENT_OP_SHUTDOWN), 5s);
    REQUIRE(done.has_value());
    CHECK(done->status == SLOPKIT_AGENT_STATUS_OK);

    one.join();
    two.join();
    CHECK(first_ok);
    CHECK(second_ok);
    CHECK((transport->header().flags & SLOPKIT_AGENT_FLAG_STOPPED) != 0);
    CHECK(agent.served() >= 2u);
}

TEST_CASE("the agent transport reports a refused request", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-refusal"));
    REQUIRE(transport.has_value());

    MockAgent agent(transport->directory(), MockAgent::Behaviour::Serve);

    SECTION("an unknown opcode is refused, not ignored")
    {
        const auto answer = transact(*transport, request(9999), 2s);
        REQUIRE(answer.has_value());
        CHECK(answer->status == SLOPKIT_AGENT_STATUS_UNKNOWN_OP);
    }

    SECTION("a bad argument is refused with a status")
    {
        auto alloc        = request(SLOPKIT_AGENT_OP_ALLOC);
        alloc.size        = 0;
        const auto answer = transact(*transport, alloc, 2s);
        REQUIRE(answer.has_value());
        CHECK(answer->status == SLOPKIT_AGENT_STATUS_BAD_REQUEST);
    }
}

TEST_CASE("the agent transport times out instead of hanging", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-timeout"));
    REQUIRE(transport.has_value());

    MockAgent agent(transport->directory(), MockAgent::Behaviour::Silent);

    const auto start  = std::chrono::steady_clock::now();
    const auto answer = transact(*transport, request(SLOPKIT_AGENT_OP_HELLO), 200ms);
    const auto spent  = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().find("did not answer") != std::string::npos);
    CHECK(spent >= 200ms);
    CHECK(spent < 5s);
}

TEST_CASE("the agent transport refuses a protocol mismatch promptly", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-mismatch"));
    REQUIRE(transport.has_value());

    MockAgent agent(transport->directory(), MockAgent::Behaviour::ProtocolMismatch);

    const auto start  = std::chrono::steady_clock::now();
    const auto answer = transact(*transport, request(SLOPKIT_AGENT_OP_HELLO), 5s);
    const auto spent  = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().find("protocol mismatch") != std::string::npos);
    CHECK(spent < 1s);
}
