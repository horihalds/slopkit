#include "trainer/runtime/target.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/uio.h>
#include <unistd.h>

namespace slopkit::trainer
{
    namespace
    {
        constexpr std::uint8_t kElfClass64 {2};

        std::string trim(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n'))
            {
                text.remove_prefix(1);
            }
            while (!text.empty()
                   && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r'))
            {
                text.remove_suffix(1);
            }
            return std::string {text};
        }

        std::string lowercase(std::string_view text)
        {
            std::string result {text};
            for (char& character : result)
            {
                if (character >= 'A' && character <= 'Z')
                {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            return result;
        }

        std::optional<std::string> read_text_file(const std::filesystem::path& path)
        {
            std::ifstream stream {path, std::ios::binary};
            if (!stream)
            {
                return std::nullopt;
            }
            return std::string {std::istreambuf_iterator<char> {stream}, std::istreambuf_iterator<char> {}};
        }

        std::filesystem::path proc_entry(std::uint32_t pid, std::string_view name)
        {
            return std::filesystem::path {"/proc"} / std::to_string(pid) / name;
        }

        std::optional<std::filesystem::path> process_executable(std::uint32_t pid)
        {
            std::error_code error;
            auto            path = std::filesystem::read_symlink(proc_entry(pid, "exe"), error);
            if (error)
            {
                return std::nullopt;
            }
            return path;
        }

        bool same_name(std::string_view candidate, std::string_view wanted)
        {
            return lowercase(candidate) == lowercase(wanted);
        }

        // The recorded name is the one slopkit read from /proc/<pid>/comm; the
        // executable's basename is accepted as well, since the two differ for
        // interpreters and wrappers.
        std::optional<std::uint32_t> find_process(std::string_view name)
        {
            const auto self = static_cast<std::uint32_t>(::getpid());

            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator {"/proc", error})
            {
                const auto    pid_text = entry.path().filename().string();
                std::uint32_t pid      = 0;
                const auto    parsed   = std::from_chars(pid_text.data(), pid_text.data() + pid_text.size(), pid);
                if (parsed.ec != std::errc {} || parsed.ptr != pid_text.data() + pid_text.size() || pid == self)
                {
                    continue;
                }

                if (const auto comm = read_text_file(entry.path() / "comm"); comm && same_name(trim(*comm), name))
                {
                    return pid;
                }
                if (const auto executable = process_executable(pid);
                    executable && same_name(executable->filename().string(), name))
                {
                    return pid;
                }
            }
            return std::nullopt;
        }

        std::uint32_t read_pointer_size(std::uint32_t pid)
        {
            const auto executable = process_executable(pid);
            if (!executable)
            {
                return 8;
            }

            std::ifstream stream {*executable, std::ios::binary};
            unsigned char identity[5] {};
            if (!stream.read(reinterpret_cast<char*>(identity), sizeof(identity)))
            {
                return 8;
            }
            return identity[4] == kElfClass64 ? 8 : 4;
        }

        std::string region_name(std::string_view path)
        {
            if (path.empty() || path.front() == '[')
            {
                return "[anon]";
            }
            return std::filesystem::path {path}.filename().string();
        }

        bool parse_hex(std::string_view text, std::uint64_t& value)
        {
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
            return parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size();
        }

        class LinuxTarget : public Target
        {
        public:
            LinuxTarget(std::uint32_t pid, std::vector<Module> modules, std::vector<Region> regions)
                : pid_ {pid}, pointer_size_ {read_pointer_size(pid)}, modules_ {std::move(modules)},
                  regions_ {std::move(regions)}
            {
            }

            [[nodiscard]] std::uint32_t pid() const override
            {
                return pid_;
            }

            [[nodiscard]] std::uint32_t pointer_size() const override
            {
                return pointer_size_;
            }

            [[nodiscard]] const std::vector<Module>& modules() const override
            {
                return modules_;
            }

            [[nodiscard]] const std::vector<Region>& regions() const override
            {
                return regions_;
            }

            [[nodiscard]] std::expected<std::vector<std::byte>, std::string> read(std::uint64_t address,
                                                                                  std::size_t   size) const override
            {
                std::vector<std::byte> bytes(size);
                if (size == 0)
                {
                    return bytes;
                }

                iovec      local {bytes.data(), size};
                iovec      remote {reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), size};
                const auto count = ::process_vm_readv(static_cast<pid_t>(pid_), &local, 1, &remote, 1, 0);
                if (count != static_cast<ssize_t>(size))
                {
                    if (const auto message = read_mem(address, bytes))
                    {
                        return std::unexpected(*message);
                    }
                }
                return bytes;
            }

            [[nodiscard]] std::expected<void, std::string> write(std::uint64_t              address,
                                                                 std::span<const std::byte> bytes) const override
            {
                if (bytes.empty())
                {
                    return {};
                }

                iovec      local {const_cast<std::byte*>(bytes.data()), bytes.size()};
                iovec      remote {reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), bytes.size()};
                const auto count = ::process_vm_writev(static_cast<pid_t>(pid_), &local, 1, &remote, 1, 0);
                if (count == static_cast<ssize_t>(bytes.size()))
                {
                    return {};
                }

                if (const auto message = write_mem(address, bytes))
                {
                    return std::unexpected(*message);
                }
                return {};
            }

        private:
            // /proc/<pid>/mem needs no attach step of its own and is the fallback
            // wherever the v-v syscalls are refused.
            [[nodiscard]] std::optional<std::string> read_mem(std::uint64_t           address,
                                                              std::vector<std::byte>& bytes) const
            {
                const int fd = ::open(proc_entry(pid_, "mem").c_str(), O_RDONLY);
                if (fd < 0)
                {
                    return describe_failure(address, errno);
                }

                const auto count   = ::pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(address));
                const int  failure = errno;
                ::close(fd);

                if (count != static_cast<ssize_t>(bytes.size()))
                {
                    return describe_failure(address, count < 0 ? failure : EIO);
                }
                return std::nullopt;
            }

            [[nodiscard]] std::optional<std::string> write_mem(std::uint64_t              address,
                                                               std::span<const std::byte> bytes) const
            {
                const int fd = ::open(proc_entry(pid_, "mem").c_str(), O_RDWR);
                if (fd < 0)
                {
                    return describe_failure(address, errno);
                }

                const auto count   = ::pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(address));
                const int  failure = errno;
                ::close(fd);

                if (count != static_cast<ssize_t>(bytes.size()))
                {
                    return describe_failure(address, count < 0 ? failure : EIO);
                }
                return std::nullopt;
            }

            [[nodiscard]] std::string describe_failure(std::uint64_t address, int error) const
            {
                return std::format("cannot access 0x{:x} in pid {}: {}", address, pid_, std::strerror(error));
            }

            std::uint32_t       pid_ {};
            std::uint32_t       pointer_size_ {8};
            std::vector<Module> modules_;
            std::vector<Region> regions_;
        };

        void parse_maps(std::string_view maps, std::vector<Module>& modules, std::vector<Region>& regions)
        {
            std::map<std::string, std::size_t> module_by_path;

            std::size_t offset = 0;
            while (offset < maps.size())
            {
                const auto end  = maps.find('\n', offset);
                const auto line = maps.substr(offset, end - offset);
                offset          = end == std::string_view::npos ? maps.size() : end + 1;
                if (line.empty())
                {
                    continue;
                }

                const auto dash = line.find('-');
                const auto gap  = line.find(' ');
                if (dash == std::string_view::npos || gap == std::string_view::npos)
                {
                    continue;
                }

                std::uint64_t start = 0;
                std::uint64_t stop  = 0;
                if (!parse_hex(line.substr(0, dash), start) || !parse_hex(line.substr(dash + 1, gap - dash - 1), stop))
                {
                    continue;
                }

                const auto path_start = line.find('/');
                const auto path_text =
                    path_start == std::string_view::npos ? std::string_view {} : line.substr(path_start);

                Region region;
                region.start    = start;
                region.end      = stop;
                region.readable = line.size() > gap + 1 && line[gap + 1] == 'r';
                region.writable = line.size() > gap + 2 && line[gap + 2] == 'w';
                region.name     = region_name(path_text);

                if (!path_text.empty())
                {
                    const auto found = module_by_path.find(std::string {path_text});
                    if (found == module_by_path.end())
                    {
                        module_by_path.emplace(std::string {path_text}, modules.size());
                        modules.push_back(Module {region.name, start});
                    }
                    else
                    {
                        auto& module = modules[found->second];
                        module.base  = std::min(module.base, start);
                    }
                }
                regions.push_back(std::move(region));
            }

            std::ranges::sort(modules, {}, &Module::base);
        }
    }

    std::expected<std::unique_ptr<Target>, AttachError> attach(const AttachRequest& request)
    {
        std::uint32_t pid = request.pid;
        if (pid == 0)
        {
            const auto found = find_process(request.name);
            if (!found)
            {
                return std::unexpected(AttachError {
                    std::format("no running process is named \"{}\"; pass --pid or --name", request.name), true});
            }
            pid = *found;
        }

        const auto maps = read_text_file(proc_entry(pid, "maps"));
        if (!maps)
        {
            return std::unexpected(
                AttachError {std::format("cannot read the memory map of pid {}; is it still running?", pid), true});
        }

        std::vector<Module> modules;
        std::vector<Region> regions;
        parse_maps(*maps, modules, regions);

        return std::make_unique<LinuxTarget>(pid, std::move(modules), std::move(regions));
    }

    std::filesystem::path own_executable_path()
    {
        std::error_code error;
        auto            path = std::filesystem::read_symlink("/proc/self/exe", error);
        return error ? std::filesystem::path {} : path;
    }

    InputResult read_line(std::uint32_t timeout_ms)
    {
        static std::string pending;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds {timeout_ms};
        while (true)
        {
            if (const auto newline = pending.find('\n'); newline != std::string::npos)
            {
                auto line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                return InputResult {InputStatus::line, std::move(line)};
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                return InputResult {InputStatus::idle, {}};
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();

            pollfd     descriptor {STDIN_FILENO, POLLIN, 0};
            const auto ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
            if (ready == 0)
            {
                return InputResult {InputStatus::idle, {}};
            }
            if (ready < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return InputResult {InputStatus::closed, {}};
            }

            char       buffer[256];
            const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count == 0)
            {
                return InputResult {InputStatus::closed, {}};
            }
            if (count < 0)
            {
                if (errno == EINTR || errno == EAGAIN)
                {
                    continue;
                }
                return InputResult {InputStatus::closed, {}};
            }
            pending.append(buffer, static_cast<std::size_t>(count));
        }
    }

    bool stdin_is_terminal()
    {
        return ::isatty(STDIN_FILENO) != 0;
    }

    void clear_screen()
    {
        std::fputs("\x1b[2J\x1b[H", stdout);
        std::fflush(stdout);
    }

}
