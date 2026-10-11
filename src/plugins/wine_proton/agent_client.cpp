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

#include "process/types.hpp"

namespace slopkit::plugins::wine_proton
{
    namespace
    {
        // dladdr resolves this anchor to the plugin's own library, which is how
        // the agent binaries that ship beside it are found without a search
        // path. SLOPKIT_AGENT_DIR overrides it for tests.
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

        // A per-run, per-target directory the target can reach through Wine's
        // default Z: drive. It holds the region, the DLL and the injector, and
        // the transport removes it on teardown.
        std::filesystem::path make_session_directory(process::ProcessId pid)
        {
            std::filesystem::path base;
            if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime != '\0')
            {
                base = runtime;
            }
            else
            {
                std::error_code error;
                base = std::filesystem::temp_directory_path(error);
                if (error)
                {
                    base = "/tmp";
                }
            }
            return base / std::format("slopkit-agent-{}-{}", pid, static_cast<long>(::getpid()));
        }

        struct SpawnOutcome
        {
            int         exit_code {0};
            bool        timed_out {false};
            std::string output;
        };

        // Runs `arguments` with `environment` (the target's own), combining
        // stdout and stderr into one bounded capture. The child is killed and
        // reported as timed out if it outlives `timeout`, so the tool can never
        // hang on a wedged wine.
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

        std::string first_line(const std::string& text)
        {
            const auto end = text.find('\n');
            return end == std::string::npos ? text : text.substr(0, end);
        }
    } // namespace

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

        const auto session_directory = make_session_directory(target.pid);
        const auto dll_target        = session_directory / SLOPKIT_AGENT_DLL_FILE;
        const auto injector_target   = session_directory / SLOPKIT_AGENT_INJECTOR_FILE;
        std::filesystem::create_directories(session_directory, error);
        std::filesystem::copy_file(dll_source, dll_target, std::filesystem::copy_options::overwrite_existing, error);
        if (error)
        {
            return std::unexpected(
                std::format("cannot stage the agent DLL in {}: {}", session_directory.string(), error.message()));
        }
        std::filesystem::copy_file(
            injector_source, injector_target, std::filesystem::copy_options::overwrite_existing, error);
        if (error)
        {
            return std::unexpected(
                std::format("cannot stage the injector in {}: {}", session_directory.string(), error.message()));
        }

        auto transport = AgentTransport::create(session_directory);
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
            return std::unexpected(
                std::format("the injector exited with {}: {}", spawned->exit_code, first_line(spawned->output)));
        }

        slopkit_agent_record hello {};
        hello.op          = SLOPKIT_AGENT_OP_HELLO;
        const auto answer = transport->transact(hello, timeout);
        const auto refuse = [&](const std::string& reason)
        {
            // The DLL is in the target now; ask it to leave before the region
            // goes away so no service thread keeps polling an unlinked file.
            slopkit_agent_record shutdown {};
            shutdown.op = SLOPKIT_AGENT_OP_SHUTDOWN;
            (void)transport->transact(shutdown, std::chrono::milliseconds {200});
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
        if (answer->pointer_size != target.pointer_size)
        {
            return refuse(std::format("the agent is {}-bit but the target is {}-bit",
                                      answer->pointer_size == 4 ? 32 : 64,
                                      target.pointer_size == 4 ? 32 : 64));
        }

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
        return allocations_.contains(address);
    }

    const std::filesystem::path& AgentClient::directory() const noexcept
    {
        return transport_->directory();
    }

    std::expected<slopkit_agent_record, std::string> AgentClient::request(const slopkit_agent_record& record,
                                                                          std::chrono::milliseconds   timeout)
    {
        auto answer = transport_->transact(record, timeout);
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
        allocations_.insert(answer->address);
        return answer->address;
    }

    std::expected<std::uint64_t, std::string>
    AgentClient::allocate_near(std::uint64_t size, std::uint64_t near_address, std::chrono::milliseconds timeout)
    {
        slopkit_agent_record record {};
        record.op         = SLOPKIT_AGENT_OP_ALLOC_NEAR;
        record.size       = size;
        record.hint       = near_address;
        const auto answer = transport_->transact(record, timeout);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        if (answer->status == SLOPKIT_AGENT_STATUS_UNSUPPORTED)
        {
            // No gap within ±2 GB. The hint is best effort, so place it
            // anywhere rather than failing the call.
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
        allocations_.insert(answer->address);
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
        allocations_.erase(address);
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

    void AgentClient::shutdown(std::chrono::milliseconds timeout) noexcept
    {
        if (transport_ == nullptr)
        {
            return;
        }
        if (!shutdown_sent_)
        {
            slopkit_agent_record record {};
            record.op      = SLOPKIT_AGENT_OP_SHUTDOWN;
            shutdown_sent_ = true;
            (void)transport_->transact(record, timeout);
        }
        allocations_.clear();
    }

} // namespace slopkit::plugins::wine_proton
