#include <catch2/catch.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
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

    // The mock stands in for the injected DLL: it maps the same region file the
    // transport created and serves the protocol by hand. Every field the
    // transport put on the wire is therefore checked for real, and the state
    // word is the same one the DLL uses.
    class MockAgent
    {
    public:
        enum class Behaviour
        {
            Serve,
            Silent,
            ProtocolMismatch,
        };

        MockAgent(const std::filesystem::path& directory, Behaviour behaviour) : behaviour_(behaviour)
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
            if (behaviour_ == Behaviour::Serve)
            {
                thread_ = std::jthread(
                    [this]
                    {
                        serve();
                    });
            }
        }

        MockAgent(const MockAgent&)            = delete;
        MockAgent& operator=(const MockAgent&) = delete;

        ~MockAgent()
        {
            stop_.store(true);
            if (thread_.joinable())
            {
                thread_.join();
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

        // The last request the mock picked up off the wire, so a test can assert
        // the exact encoding the transport produced.
        [[nodiscard]] const slopkit_agent_record& last_request() const
        {
            return last_request_;
        }

        [[nodiscard]] std::uint32_t served() const
        {
            return served_.load();
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

        slopkit_agent_record answer(const slopkit_agent_record& request)
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
                    response.win32_error = 0x1E7; // ERROR_INVALID_ADDRESS
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
            case SLOPKIT_AGENT_OP_SHUTDOWN:
                owned_.clear();
                stop_.store(true);
                break;
            default:
                response.status = SLOPKIT_AGENT_STATUS_UNKNOWN_OP;
                break;
            }
            return response;
        }

        void serve()
        {
            while (!stop_.load())
            {
                auto& state = region_->header.state;
                if (std::atomic_ref<std::uint32_t>(state).load(std::memory_order_acquire)
                    != SLOPKIT_AGENT_STATE_REQUEST)
                {
                    std::this_thread::sleep_for(50us);
                    continue;
                }

                last_request_                       = region_->request;
                const slopkit_agent_record response = answer(last_request_);
                // The stopped flag is published before the response, so a host
                // that observes the answer also observes the flag.
                const bool                 stopping = last_request_.op == SLOPKIT_AGENT_OP_SHUTDOWN;
                if (stopping)
                {
                    std::atomic_ref<std::uint32_t>(region_->header.flags)
                        .fetch_or(SLOPKIT_AGENT_FLAG_STOPPED, std::memory_order_release);
                }
                region_->response = response;
                served_.fetch_add(1);
                std::atomic_ref<std::uint32_t>(state).store(SLOPKIT_AGENT_STATE_RESPONSE, std::memory_order_release);
                if (stopping)
                {
                    stop_.store(true);
                }
            }
        }

        Behaviour                  behaviour_ {Behaviour::Serve};
        int                        descriptor_ {-1};
        slopkit_agent_region*      region_ {nullptr};
        std::jthread               thread_;
        std::atomic<bool>          stop_ {false};
        std::atomic<std::uint32_t> served_ {0};
        slopkit_agent_record       last_request_ {};
        std::vector<std::uint64_t> owned_;
        std::uint64_t              next_base_ {0x140000000ull};
    };

    // A fresh per-test directory under the project's tmp/; the transport removes
    // it when it is destroyed, so nothing is left behind.
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
} // namespace

TEST_CASE("the agent transport handshakes and echoes the request id", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-handshake"));
    REQUIRE(transport.has_value());

    MockAgent agent(transport->directory(), MockAgent::Behaviour::Serve);

    const auto hello = transport->transact(request(SLOPKIT_AGENT_OP_HELLO), 2s);
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

    // The response is consumed and the state returns to idle, so the next
    // exchange starts cleanly with the next request id.
    CHECK(transport->load_state() == SLOPKIT_AGENT_STATE_IDLE);
    const auto again = transport->transact(request(SLOPKIT_AGENT_OP_HELLO), 2s);
    REQUIRE(again.has_value());
    CHECK(again->request_id == 2u);
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
        const auto allocated = transport->transact(alloc, 2s);
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
        const auto placed = transport->transact(near, 2s);
        REQUIRE(placed.has_value());
        CHECK(placed->address == 0x7f1234560000ull);
        CHECK(agent.last_request().hint == 0x7f1234567890ull);
    }

    SECTION("free releases an owned address and refuses a foreign one")
    {
        auto alloc           = request(SLOPKIT_AGENT_OP_ALLOC);
        alloc.size           = 0x1000;
        const auto allocated = transport->transact(alloc, 2s);
        REQUIRE(allocated.has_value());

        auto release     = request(SLOPKIT_AGENT_OP_FREE);
        release.address  = allocated->address;
        const auto freed = transport->transact(release, 2s);
        REQUIRE(freed.has_value());
        CHECK(freed->status == SLOPKIT_AGENT_STATUS_OK);

        release.address  = 0xdead0000ull;
        const auto stray = transport->transact(release, 2s);
        REQUIRE(stray.has_value());
        CHECK(stray->status == SLOPKIT_AGENT_STATUS_NOT_OWNED);
        CHECK(stray->win32_error == 0x1E7u);
    }

    SECTION("protect returns the previous protection")
    {
        auto alloc           = request(SLOPKIT_AGENT_OP_ALLOC);
        alloc.size           = 0x1000;
        const auto allocated = transport->transact(alloc, 2s);
        REQUIRE(allocated.has_value());

        auto protect       = request(SLOPKIT_AGENT_OP_PROTECT);
        protect.address    = allocated->address;
        protect.size       = 0x1000;
        protect.protection = SLOPKIT_AGENT_PAGE_READONLY;
        const auto changed = transport->transact(protect, 2s);
        REQUIRE(changed.has_value());
        CHECK(changed->protection == SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE);
        CHECK(agent.last_request().protection == SLOPKIT_AGENT_PAGE_READONLY);
    }

    SECTION("query reports the target's view of a region")
    {
        auto query        = request(SLOPKIT_AGENT_OP_QUERY);
        query.address     = 0x7f00abcdull;
        const auto region = transport->transact(query, 2s);
        REQUIRE(region.has_value());
        CHECK(region->region_base == 0x7f00a000ull);
        CHECK(region->region_size == 0x1000u);
        CHECK(region->region_state == SLOPKIT_AGENT_MEM_COMMIT);
        CHECK(region->region_protect == SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE);
    }

    SECTION("shutdown answers and marks the agent stopped")
    {
        const auto done = transport->transact(request(SLOPKIT_AGENT_OP_SHUTDOWN), 2s);
        REQUIRE(done.has_value());
        CHECK(done->status == SLOPKIT_AGENT_STATUS_OK);
        CHECK((transport->header().flags & SLOPKIT_AGENT_FLAG_STOPPED) != 0);
    }
}

TEST_CASE("the agent transport reports a refused request", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-refusal"));
    REQUIRE(transport.has_value());

    MockAgent agent(transport->directory(), MockAgent::Behaviour::Serve);

    SECTION("an unknown opcode is refused, not ignored")
    {
        const auto answer = transport->transact(request(9999), 2s);
        REQUIRE(answer.has_value());
        CHECK(answer->status == SLOPKIT_AGENT_STATUS_UNKNOWN_OP);
    }

    SECTION("a bad argument is refused with a status")
    {
        auto alloc        = request(SLOPKIT_AGENT_OP_ALLOC);
        alloc.size        = 0;
        const auto answer = transport->transact(alloc, 2s);
        REQUIRE(answer.has_value());
        CHECK(answer->status == SLOPKIT_AGENT_STATUS_BAD_REQUEST);
    }
}

TEST_CASE("the agent transport times out instead of hanging", "[wine-proton][agent]")
{
    auto transport = AgentTransport::create(scratch_directory("agent-transport-timeout"));
    REQUIRE(transport.has_value());

    // A mapped but silent agent: nothing answers, which is also what a dead or
    // wedged one looks like.
    MockAgent agent(transport->directory(), MockAgent::Behaviour::Silent);

    const auto start  = std::chrono::steady_clock::now();
    const auto answer = transport->transact(request(SLOPKIT_AGENT_OP_HELLO), 200ms);
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
    const auto answer = transport->transact(request(SLOPKIT_AGENT_OP_HELLO), 5s);
    const auto spent  = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().find("protocol mismatch") != std::string::npos);
    CHECK(spent < 1s);
}
