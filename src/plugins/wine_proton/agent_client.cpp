#include "plugins/wine_proton/agent_client.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "plugins/wine_proton/agent_modules.hpp"
#include "process/types.hpp"

namespace slopkit::plugins::wine_proton
{
    namespace
    {
        const int kAgentDirectoryAnchor = 0;

        std::optional<std::filesystem::path> agent_binary_directory()
        {
            if (const char* override_dir = std::getenv("SLOPKIT_AGENT_DIR");
                override_dir != nullptr && *override_dir != '\0')
            {
                return std::filesystem::path(override_dir);
            }
            Dl_info info {};
            if (dladdr(&kAgentDirectoryAnchor, &info) == 0 || info.dli_fname == nullptr)
            {
                return std::nullopt;
            }
            return std::filesystem::path(info.dli_fname).parent_path();
        }

        struct SpawnOutcome
        {
            int         exit_code {0};
            bool        timed_out {false};
            std::string output;
        };

        std::expected<SpawnOutcome, std::string> run_process(const std::vector<std::string>& arguments,
                                                             const std::vector<std::string>& environment,
                                                             std::chrono::milliseconds       timeout)
        {
            if (arguments.empty())
            {
                return std::unexpected(std::string("no program to run"));
            }

            int pipes[2] = {-1, -1};
            if (::pipe2(pipes, O_CLOEXEC) != 0)
            {
                return std::unexpected(std::format("cannot create a pipe: {}", std::strerror(errno)));
            }

            std::vector<char*> argv;
            argv.reserve(arguments.size() + 1);
            for (const auto& argument : arguments)
            {
                argv.push_back(const_cast<char*>(argument.c_str()));
            }
            argv.push_back(nullptr);

            std::vector<std::string> environment_copy = environment;
            std::vector<char*>       envp;
            envp.reserve(environment_copy.size() + 1);
            for (auto& entry : environment_copy)
            {
                envp.push_back(entry.data());
            }
            envp.push_back(nullptr);

            const pid_t child = ::fork();
            if (child < 0)
            {
                const std::string reason = std::strerror(errno);
                ::close(pipes[0]);
                ::close(pipes[1]);
                return std::unexpected(std::format("cannot fork the injector: {}", reason));
            }
            if (child == 0)
            {
                ::dup2(pipes[1], STDOUT_FILENO);
                ::dup2(pipes[1], STDERR_FILENO);
                ::close(pipes[0]);
                ::close(pipes[1]);
                ::execve(argv[0], argv.data(), envp.data());
                ::_exit(127);
            }
            ::close(pipes[1]);

            std::string  output;
            std::jthread reader(
                [&output, read_fd = pipes[0]]
                {
                    char    buffer[512];
                    ssize_t count = 0;
                    while ((count = ::read(read_fd, buffer, sizeof(buffer))) > 0)
                    {
                        output.append(buffer, static_cast<std::size_t>(count));
                    }
                });

            int        status   = 0;
            bool       reaped   = false;
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                const pid_t done = ::waitpid(child, &status, WNOHANG);
                if (done == child)
                {
                    reaped = true;
                    break;
                }
                if (done < 0 && errno != EINTR)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            SpawnOutcome outcome;
            if (!reaped)
            {
                outcome.timed_out = true;
                ::kill(child, SIGKILL);
                ::waitpid(child, &status, 0);
            }
            reader.join();
            ::close(pipes[0]);

            outcome.output    = std::move(output);
            outcome.exit_code = reaped && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return outcome;
        }

    }

    std::string injector_failure_message(std::string_view output)
    {
        constexpr std::string_view kInjectorMark = "slopkit-injector:";
        std::string_view           last;
        std::string_view           from_injector;
        for (std::size_t offset = 0; offset <= output.size();)
        {
            const auto       end  = output.find('\n', offset);
            const auto       stop = end == std::string_view::npos ? output.size() : end;
            std::string_view line = output.substr(offset, stop - offset);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (!line.empty())
            {
                last = line;
                if (line.find(kInjectorMark) != std::string_view::npos)
                {
                    from_injector = line;
                }
            }
            if (end == std::string_view::npos)
            {
                break;
            }
            offset = end + 1;
        }
        const auto chosen = from_injector.empty() ? last : from_injector;
        return chosen.empty() ? std::string("no output") : std::string(chosen);
    }

    std::string AgentClient::dll_file_name(std::uint32_t pointer_size)
    {
        return std::format("slopkit-agent-{}.dll", pointer_size == 4 ? "i686" : "x86_64");
    }

    std::string AgentClient::injector_file_name(std::uint32_t pointer_size)
    {
        return std::format("slopkit-injector-{}.exe", pointer_size == 4 ? "i686" : "x86_64");
    }

    std::expected<AgentClient, std::string> AgentClient::launch(const WineTarget&         target,
                                                                std::chrono::milliseconds timeout)
    {
        const auto source_directory = agent_binary_directory();
        if (!source_directory)
        {
            return std::unexpected(std::string("cannot locate the directory the agent binaries ship in"));
        }
        const auto      dll_source      = *source_directory / dll_file_name(target.pointer_size);
        const auto      injector_source = *source_directory / injector_file_name(target.pointer_size);
        std::error_code error;
        if (!std::filesystem::exists(dll_source, error) || !std::filesystem::exists(injector_source, error))
        {
            return std::unexpected(std::format("the {}-bit agent binaries are missing from {}",
                                               target.pointer_size == 4 ? 32 : 64,
                                               source_directory->string()));
        }

        const auto staging = make_staging_directory(target.pid);
        if (!staging)
        {
            return std::unexpected(staging.error());
        }
        const auto dll_target      = *staging / SLOPKIT_AGENT_DLL_FILE;
        const auto injector_target = *staging / SLOPKIT_AGENT_INJECTOR_FILE;
        std::filesystem::copy_file(dll_source, dll_target, std::filesystem::copy_options::overwrite_existing, error);
        if (error)
        {
            return std::unexpected(
                std::format("cannot stage the agent DLL in {}: {}", staging->string(), error.message()));
        }
        std::filesystem::copy_file(
            injector_source, injector_target, std::filesystem::copy_options::overwrite_existing, error);
        if (error)
        {
            return std::unexpected(
                std::format("cannot stage the injector in {}: {}", staging->string(), error.message()));
        }

        auto transport = AgentTransport::create(*staging);
        if (!transport)
        {
            return std::unexpected(transport.error());
        }

        const std::vector<std::string> arguments {
            target.wine_binary.string(),
            injector_target.string(),
            "--image",
            target.image_path,
            "--dll",
            linux_path_to_windows(dll_target.string()),
        };
        const auto spawned = run_process(arguments, target.environment, timeout);
        if (!spawned)
        {
            return std::unexpected(spawned.error());
        }
        if (spawned->timed_out)
        {
            return std::unexpected(std::format("the injector did not finish within {} ms", timeout.count()));
        }
        if (spawned->exit_code != 0)
        {
            return std::unexpected(std::format(
                "the injector exited with {}: {}", spawned->exit_code, injector_failure_message(spawned->output)));
        }

        const auto exchange =
            [&transport](const slopkit_agent_record& record,
                         std::chrono::milliseconds   wait) -> std::expected<slopkit_agent_record, std::string>
        {
            auto slot = transport->acquire(wait);
            if (!slot)
            {
                return std::unexpected(slot.error());
            }
            return slot->transact(record, wait);
        };

        slopkit_agent_record hello {};
        hello.op          = SLOPKIT_AGENT_OP_HELLO;
        const auto answer = exchange(hello, timeout);
        const auto refuse = [&](const std::string& reason)
        {
            slopkit_agent_record shutdown {};
            shutdown.op = SLOPKIT_AGENT_OP_SHUTDOWN;
            (void)exchange(shutdown, std::chrono::milliseconds {200});
            return std::unexpected(reason);
        };
        if (!answer)
        {
            return refuse(std::format("the agent did not handshake: {}", answer.error()));
        }
        if (answer->status != SLOPKIT_AGENT_STATUS_OK)
        {
            return refuse(std::format(
                "the agent refused HELLO: status {} (win32 error {})", answer->status, answer->win32_error));
        }
        if (answer->protocol_version != SLOPKIT_AGENT_PROTOCOL_VERSION
            || answer->record_size != sizeof(slopkit_agent_record))
        {
            return refuse(std::format("the agent speaks protocol {} with a {}-byte record; this plugin speaks {} "
                                      "with {} bytes",
                                      answer->protocol_version,
                                      answer->record_size,
                                      SLOPKIT_AGENT_PROTOCOL_VERSION,
                                      sizeof(slopkit_agent_record)));
        }
        if (answer->reserved0 != SLOPKIT_AGENT_PAYLOAD_SIZE)
        {
            return refuse(std::format("the agent's shared region carries a {}-byte payload; this plugin sends {}",
                                      answer->reserved0,
                                      SLOPKIT_AGENT_PAYLOAD_SIZE));
        }
        if (answer->pointer_size != target.pointer_size)
        {
            return refuse(std::format("the agent is {}-bit but the target is {}-bit",
                                      answer->pointer_size == 4 ? 32 : 64,
                                      target.pointer_size == 4 ? 32 : 64));
        }

        transport->advertise_capacity(answer->slot_count);

        AgentClient client(std::make_shared<AgentTransport>(std::move(*transport)), std::move(spawned->output));
        client.windows_pid_  = answer->windows_pid;
        client.pointer_size_ = answer->pointer_size;
        client.granularity_  = answer->granularity;
        client.agent_base_   = answer->agent_base;
        return client;
    }

    AgentClient::AgentClient(std::shared_ptr<AgentTransport> transport, std::string injector_output)
        : transport_(std::move(transport)), injector_output_(std::move(injector_output))
    {
    }

    AgentClient::AgentClient(AgentClient&& other) noexcept
        : transport_(std::move(other.transport_)), allocations_(std::move(other.allocations_)),
          injector_output_(std::move(other.injector_output_)), windows_pid_(other.windows_pid_),
          pointer_size_(other.pointer_size_), granularity_(other.granularity_), agent_base_(other.agent_base_)
    {
    }

    AgentClient& AgentClient::operator=(AgentClient&& other) noexcept
    {
        if (this != &other)
        {
            transport_       = std::move(other.transport_);
            allocations_     = std::move(other.allocations_);
            injector_output_ = std::move(other.injector_output_);
            windows_pid_     = other.windows_pid_;
            pointer_size_    = other.pointer_size_;
            granularity_     = other.granularity_;
            agent_base_      = other.agent_base_;
        }
        return *this;
    }

    AgentClient::~AgentClient()
    {
        shutdown();
    }

    std::uint32_t AgentClient::windows_pid() const noexcept
    {
        return windows_pid_;
    }

    std::uint32_t AgentClient::granularity() const noexcept
    {
        return granularity_;
    }

    std::uint64_t AgentClient::agent_base() const noexcept
    {
        return agent_base_;
    }

    const std::string& AgentClient::injector_output() const noexcept
    {
        return injector_output_;
    }

    bool AgentClient::owns(std::uint64_t address) const noexcept
    {
        const std::lock_guard lock(allocations_mutex_);
        return allocations_.contains(address);
    }

    const std::filesystem::path& AgentClient::directory() const noexcept
    {
        return transport_->directory();
    }

    std::uint32_t AgentClient::slot_count() const
    {
        return transport_ != nullptr ? transport_->slot_count() : 0;
    }

    bool AgentClient::slots_exhausted() const
    {
        return transport_ != nullptr && transport_->exhausted();
    }

    std::expected<slopkit_agent_record, std::string> AgentClient::transact(const slopkit_agent_record& record,
                                                                           std::chrono::milliseconds   timeout,
                                                                           std::span<const std::byte>  request_payload,
                                                                           std::span<std::byte>        response_payload)
    {
        auto slot = transport_->acquire(timeout);
        if (!slot)
        {
            return std::unexpected(slot.error());
        }
        return slot->transact(record, timeout, request_payload, response_payload);
    }

    std::expected<slopkit_agent_record, std::string> AgentClient::request(const slopkit_agent_record& record,
                                                                          std::chrono::milliseconds   timeout)
    {
        auto answer = transact(record, timeout);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        if (answer->status == SLOPKIT_AGENT_STATUS_NOT_OWNED)
        {
            return std::unexpected(std::format("the agent does not own {:#x}", record.address));
        }
        if (answer->status == SLOPKIT_AGENT_STATUS_UNSUPPORTED)
        {
            return std::unexpected(std::format("the agent found no address near {:#x}", record.hint));
        }
        if (answer->status != SLOPKIT_AGENT_STATUS_OK)
        {
            return std::unexpected(std::format("the agent refused op {} with status {} (win32 error {})",
                                               record.op,
                                               answer->status,
                                               answer->win32_error));
        }
        return *answer;
    }

    std::expected<std::uint64_t, std::string>
    AgentClient::allocate(std::uint64_t size, std::uint64_t hint, std::chrono::milliseconds timeout)
    {
        slopkit_agent_record record {};
        record.op   = SLOPKIT_AGENT_OP_ALLOC;
        record.size = size;
        record.hint = hint;
        auto answer = request(record, timeout);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        if (answer->address == 0)
        {
            return std::unexpected(std::string("the agent returned no address"));
        }
        {
            const std::lock_guard lock(allocations_mutex_);
            allocations_.insert(answer->address);
        }
        return answer->address;
    }

    std::expected<std::uint64_t, std::string>
    AgentClient::allocate_near(std::uint64_t size, std::uint64_t near_address, std::chrono::milliseconds timeout)
    {
        slopkit_agent_record record {};
        record.op         = SLOPKIT_AGENT_OP_ALLOC_NEAR;
        record.size       = size;
        record.hint       = near_address;
        const auto answer = transact(record, timeout);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        if (answer->status == SLOPKIT_AGENT_STATUS_UNSUPPORTED)
        {
            return allocate(size, 0, timeout);
        }
        if (answer->status != SLOPKIT_AGENT_STATUS_OK || answer->address == 0)
        {
            return std::unexpected(std::format("the agent could not place {:#x} bytes near {:#x}: status {} (win32 "
                                               "error {})",
                                               size,
                                               near_address,
                                               answer->status,
                                               answer->win32_error));
        }
        {
            const std::lock_guard lock(allocations_mutex_);
            allocations_.insert(answer->address);
        }
        return answer->address;
    }

    std::expected<void, std::string> AgentClient::free(std::uint64_t address, std::chrono::milliseconds timeout)
    {
        slopkit_agent_record record {};
        record.op      = SLOPKIT_AGENT_OP_FREE;
        record.address = address;
        auto answer    = request(record, timeout);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        {
            const std::lock_guard lock(allocations_mutex_);
            allocations_.erase(address);
        }
        return {};
    }

    std::expected<std::uint32_t, std::string> AgentClient::protect(std::uint64_t             address,
                                                                   std::uint64_t             size,
                                                                   std::uint32_t             protection,
                                                                   std::chrono::milliseconds timeout)
    {
        slopkit_agent_record record {};
        record.op         = SLOPKIT_AGENT_OP_PROTECT;
        record.address    = address;
        record.size       = size;
        record.protection = protection;
        auto answer       = request(record, timeout);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        return answer->protection;
    }

    std::expected<AgentRegionInfo, std::string> AgentClient::query(std::uint64_t             address,
                                                                   std::chrono::milliseconds timeout)
    {
        slopkit_agent_record record {};
        record.op      = SLOPKIT_AGENT_OP_QUERY;
        record.address = address;
        auto answer    = request(record, timeout);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        return AgentRegionInfo {answer->region_base, answer->region_size, answer->region_state, answer->region_protect};
    }

    std::expected<AgentRegionBatch, std::string>
    AgentClient::enumerate(std::uint64_t cursor, std::uint32_t capacity, std::chrono::milliseconds timeout)
    {
        const auto record_capacity =
            static_cast<std::uint32_t>(SLOPKIT_AGENT_PAYLOAD_SIZE / sizeof(slopkit_agent_region_entry));
        slopkit_agent_record record {};
        record.op      = SLOPKIT_AGENT_OP_ENUM;
        record.address = cursor;
        record.size    = capacity == 0 ? record_capacity : capacity;

        std::vector<std::byte> payload(SLOPKIT_AGENT_PAYLOAD_SIZE);
        auto                   answer = transact(record, timeout, {}, payload);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        if (answer->status != SLOPKIT_AGENT_STATUS_OK)
        {
            return std::unexpected(std::format("the agent refused an ENUM at {:#x} with status {} (win32 error {})",
                                               cursor,
                                               answer->status,
                                               answer->win32_error));
        }
        if (answer->size > record_capacity)
        {
            return std::unexpected(std::format(
                "the agent reported {} region records; the payload holds {}", answer->size, record_capacity));
        }

        AgentRegionBatch batch;
        batch.next_cursor = answer->address;
        batch.regions.reserve(answer->size);
        const auto* entries = reinterpret_cast<const slopkit_agent_region_entry*>(payload.data());
        for (std::uint32_t index = 0; index < answer->size; ++index)
        {
            batch.regions.push_back(AgentRegionEntry {entries[index].base,
                                                      entries[index].size,
                                                      entries[index].state,
                                                      entries[index].protection,
                                                      entries[index].type});
        }
        return batch;
    }

    std::expected<AgentModuleBatch, std::string>
    AgentClient::modules(std::uint64_t start_index, std::uint32_t capacity, std::chrono::milliseconds timeout)
    {
        const auto record_capacity =
            static_cast<std::uint32_t>(SLOPKIT_AGENT_PAYLOAD_SIZE / sizeof(slopkit_agent_module_entry));
        slopkit_agent_record record {};
        record.op      = SLOPKIT_AGENT_OP_MODULES;
        record.address = start_index;
        record.size    = capacity == 0 ? record_capacity : capacity;

        std::vector<std::byte> payload(SLOPKIT_AGENT_PAYLOAD_SIZE);
        auto                   answer = transact(record, timeout, {}, payload);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        if (answer->status != SLOPKIT_AGENT_STATUS_OK)
        {
            return std::unexpected(
                std::format("the agent refused a MODULES at index {} with status {} (win32 error {})",
                            start_index,
                            answer->status,
                            answer->win32_error));
        }
        if (answer->size > record_capacity)
        {
            return std::unexpected(std::format(
                "the agent reported {} module records; the payload holds {}", answer->size, record_capacity));
        }

        AgentModuleBatch batch;
        batch.next_index = answer->address;
        batch.modules.reserve(answer->size);
        const auto* entries = reinterpret_cast<const slopkit_agent_module_entry*>(payload.data());
        for (std::uint32_t index = 0; index < answer->size; ++index)
        {
            const auto& entry = entries[index];
            if ((entry.name_offset % 2u) != 0 || (entry.name_length % 2u) != 0
                || entry.name_offset > SLOPKIT_AGENT_PAYLOAD_SIZE
                || entry.name_length > SLOPKIT_AGENT_PAYLOAD_SIZE - entry.name_offset)
            {
                return std::unexpected(std::format("the agent's module {} names bytes [{}, {}) outside the payload",
                                                   index,
                                                   entry.name_offset,
                                                   entry.name_offset + entry.name_length));
            }

            AgentModuleEntry module;
            module.base  = entry.base;
            module.size  = entry.size;
            module.entry = entry.entry;
            module.main  = (entry.flags & SLOPKIT_AGENT_MODULE_FLAG_MAIN) != 0;
            if (entry.name_length != 0)
            {
                const auto* characters = reinterpret_cast<const char16_t*>(payload.data() + entry.name_offset);
                decode_utf16(std::span<const char16_t>(characters, entry.name_length / 2), module.name);
            }
            batch.modules.push_back(std::move(module));
        }
        return batch;
    }

    slopkit_agent_debug* AgentClient::debug_at(std::uint32_t offset) const noexcept
    {
        if (transport_ == nullptr || transport_->region() == nullptr)
        {
            return nullptr;
        }
        auto* const base = reinterpret_cast<std::uint8_t*>(transport_->region());
        return reinterpret_cast<slopkit_agent_debug*>(base + offset);
    }

    std::expected<void, std::string> AgentClient::debug_attach(std::chrono::milliseconds timeout)
    {
        slopkit_agent_record record {};
        record.op   = SLOPKIT_AGENT_OP_DEBUG_ATTACH;
        auto answer = transact(record, timeout);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        if (answer->status != SLOPKIT_AGENT_STATUS_OK)
        {
            return std::unexpected(std::format(
                "the agent refused a debug attach: status {} (win32 error {})", answer->status, answer->win32_error));
        }
        return {};
    }

    std::expected<void, std::string> AgentClient::debug_detach(std::chrono::milliseconds timeout)
    {
        slopkit_agent_record record {};
        record.op   = SLOPKIT_AGENT_OP_DEBUG_DETACH;
        auto answer = transact(record, timeout);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        if (answer->status != SLOPKIT_AGENT_STATUS_OK)
        {
            return std::unexpected(std::format(
                "the agent refused a debug detach: status {} (win32 error {})", answer->status, answer->win32_error));
        }
        return {};
    }

    bool AgentClient::wait_for_answer(std::uint32_t sequence, std::chrono::milliseconds timeout) const
    {
        slopkit_agent_debug* const block = debug_at(static_cast<std::uint32_t>(offsetof(slopkit_agent_region, debug)));
        if (block == nullptr)
        {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true)
        {
            if (std::atomic_ref<std::uint32_t>(block->answer).load(std::memory_order_acquire) == sequence)
            {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
    }

    bool AgentClient::wait_for_stop(std::chrono::milliseconds timeout) const
    {
        slopkit_agent_debug* const block = debug_at(static_cast<std::uint32_t>(offsetof(slopkit_agent_region, debug)));
        if (block == nullptr)
        {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true)
        {
            if (std::atomic_ref<std::uint32_t>(block->flag).load(std::memory_order_acquire) != 0)
            {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
    }

    void AgentClient::shutdown(std::chrono::milliseconds timeout) noexcept
    {
        if (transport_ == nullptr)
        {
            return;
        }
        {
            const std::lock_guard lock(shutdown_mutex_);
            if (shutdown_sent_)
            {
                return;
            }
            shutdown_sent_ = true;
        }
        slopkit_agent_record record {};
        record.op = SLOPKIT_AGENT_OP_SHUTDOWN;
        (void)transact(record, timeout);
        {
            const std::lock_guard lock(allocations_mutex_);
            allocations_.clear();
        }
    }

}
