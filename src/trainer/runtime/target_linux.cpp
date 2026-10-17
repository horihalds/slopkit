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

#include "platform/linux/module_entry.hpp"
#include "platform/linux/procfs.hpp"
#include "platform/linux/wine.hpp"

namespace slopkit::trainer
{
    namespace
    {
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

        std::optional<std::uint32_t> parse_pid(std::string_view text)
        {
            std::uint32_t pid    = 0;
            const auto    parsed = std::from_chars(text.data(), text.data() + text.size(), pid);
            if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size())
            {
                return std::nullopt;
            }
            return pid;
        }

        // The recorded name is the one slopkit read from /proc/<pid>/comm; the
        // executable's basename is accepted as well, since the two differ for
        // interpreters and wrappers. A process whose guest name is longer than the
        // kernel's fifteen-character `comm` needs find_wine_process below.
        std::optional<std::uint32_t> find_process(std::string_view name)
        {
            const auto self = static_cast<std::uint32_t>(::getpid());

            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator {"/proc", error})
            {
                const auto pid = parse_pid(entry.path().filename().string());
                if (!pid || *pid == self)
                {
                    continue;
                }

                if (const auto comm = read_text_file(entry.path() / "comm"); comm && same_name(trim(*comm), name))
                {
                    return pid;
                }
                if (const auto executable = process_executable(*pid);
                    executable && same_name(executable->filename().string(), name))
                {
                    return pid;
                }
            }
            return std::nullopt;
        }

        // A cheap screen before the full Wine classification: every Wine process
        // runs a wine loader, and Wine renames it after the guest executable. A
        // process that is neither would only be rejected by reading and parsing
        // its whole memory map, which a scan of every pid cannot afford.
        bool wine_candidate(std::uint32_t pid, std::string_view wanted)
        {
            if (const auto executable = process_executable(pid);
                executable && lowercase(executable->filename().string()).starts_with("wine"))
            {
                return true;
            }

            const auto comm = read_text_file(proc_entry(pid, "comm"));
            if (!comm)
            {
                return false;
            }
            const auto name = lowercase(trim(*comm));
            return !name.empty() && lowercase(wanted).starts_with(name);
        }

        // A Wine target is named after its guest executable, and the guest's own
        // images are the only place that name exists: the kernel's `comm` is
        // truncated and `/proc/<pid>/exe` is the loader. A table made from a Wine
        // target therefore only matches here.
        std::optional<std::uint32_t> find_wine_process(std::string_view name)
        {
            if (!lowercase(name).ends_with(".exe"))
            {
                return std::nullopt;
            }

            const auto self = static_cast<std::uint32_t>(::getpid());

            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator {"/proc", error})
            {
                const auto pid = parse_pid(entry.path().filename().string());
                if (!pid || *pid == self || !wine_candidate(*pid, name)
                    || !platform::classify_wine_process(*pid).claimed())
                {
                    continue;
                }

                const auto guest = platform::guest_image_name(*pid);
                if (guest && same_name(*guest, name))
                {
                    return pid;
                }
            }
            return std::nullopt;
        }

        // The width of the image the target actually runs: the pointer chains an
        // expression resolves are the guest's, and a Wine process's loader is a
        // 64-bit ELF even when the game is 32-bit. Sixty-four bits when neither
        // image can be read, as before.
        std::uint32_t read_pointer_size(std::uint32_t pid)
        {
            std::uint32_t width = 0;
            if (platform::classify_wine_process(pid).claimed())
            {
                width = platform::guest_pointer_size(pid);
            }
            if (width == 0)
            {
                if (const auto executable = process_executable(pid))
                {
                    width = platform::read_elf_pointer_size(*executable).value_or(0);
                }
            }
            return width == 0 ? 8 : width;
        }

        std::vector<Module> to_modules(const std::vector<process::ModuleInfo>& infos)
        {
            std::vector<Module> modules;
            for (const auto& info : infos)
            {
                if (process::is_file_backed(info))
                {
                    modules.push_back(Module {info.name, info.base});
                }
            }
            std::ranges::sort(modules, {}, &Module::base);
            return modules;
        }

        // A region is named after the module that owns its address, as the app
        // names it; the mapped path only decides what a region outside every
        // module's span is called. A Wine/Proton PE image maps its code section
        // without a backing path, so the path alone would leave the very pages a
        // script scans anonymous.
        std::vector<Region> to_regions(const std::vector<platform::MappedRegion>& mapped,
                                       std::span<const process::ModuleInfo>       modules)
        {
            std::vector<Region> regions;
            regions.reserve(mapped.size());
            for (const auto& region : mapped)
            {
                std::string name = process::owning_module_name(region.start, modules);
                if (name.empty())
                {
                    name = platform::region_name(region);
                }
                regions.push_back(Region {region.start, region.end, region.readable, region.writable, std::move(name)});
            }
            return regions;
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

    }

    std::expected<std::unique_ptr<Target>, AttachError> attach(const AttachRequest& request)
    {
        std::uint32_t pid = request.pid;
        if (pid == 0)
        {
            const auto found = find_process(request.name);
            const auto guest = found ? std::nullopt : find_wine_process(request.name);
            if (!found && !guest)
            {
                return std::unexpected(AttachError {
                    std::format("no running process is named \"{}\"; pass --pid or --name", request.name), true});
            }
            pid = found ? *found : *guest;
        }

        const auto mapped = platform::read_maps(pid);
        if (mapped.empty())
        {
            return std::unexpected(
                AttachError {std::format("cannot read the memory map of pid {}; is it still running?", pid), true});
        }

        const auto infos = platform::modules_from_maps(mapped);
        return std::make_unique<LinuxTarget>(pid, to_modules(infos), to_regions(mapped, infos));
    }

    std::filesystem::path own_executable_path()
    {
        std::error_code error;
        auto            path = std::filesystem::read_symlink("/proc/self/exe", error);
        return error ? std::filesystem::path {} : path;
    }

    bool stdin_is_terminal()
    {
        return ::isatty(STDIN_FILENO) != 0;
    }

}
