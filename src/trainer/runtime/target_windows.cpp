#include "trainer/runtime/target.hpp"

#include <algorithm>
#include <chrono>
#include <conio.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// windows.h comes first: psapi.h and tlhelp32.h need the types it defines.
#include <windows.h>

#include <io.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace slopkit::trainer
{
    namespace
    {
        // The user address space ends here; the walk stops at the first region
        // VirtualQueryEx cannot describe.
        constexpr std::uintptr_t kMaxAddress {0x7FFF'FFFF'FFFFULL};

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

        std::string to_utf8(std::wstring_view text)
        {
            if (text.empty())
            {
                return {};
            }

            const auto length = static_cast<int>(text.size());
            const auto size   = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
            if (size <= 0)
            {
                return {};
            }

            std::string result(static_cast<std::size_t>(size), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), size, nullptr, nullptr);
            return result;
        }

        bool readable_protection(DWORD protect)
        {
            switch (protect & 0xFFu)
            {
            case PAGE_READONLY:
            case PAGE_READWRITE:
            case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READ:
            case PAGE_EXECUTE_READWRITE:
            case PAGE_EXECUTE_WRITECOPY:
                return true;
            default:
                return false;
            }
        }

        bool writable_protection(DWORD protect)
        {
            switch (protect & 0xFFu)
            {
            case PAGE_READWRITE:
            case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READWRITE:
            case PAGE_EXECUTE_WRITECOPY:
                return true;
            default:
                return false;
            }
        }

        bool executable_protection(DWORD protect)
        {
            switch (protect & 0xFFu)
            {
            case PAGE_EXECUTE:
            case PAGE_EXECUTE_READ:
            case PAGE_EXECUTE_READWRITE:
            case PAGE_EXECUTE_WRITECOPY:
                return true;
            default:
                return false;
            }
        }

        std::optional<std::uint32_t> find_process(std::string_view name)
        {
            const auto wanted     = lowercase(name);
            const auto wanted_exe = wanted.ends_with(".exe") ? wanted : wanted + ".exe";

            const auto snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
            {
                return std::nullopt;
            }

            PROCESSENTRY32W entry {};
            entry.dwSize = sizeof(entry);

            std::optional<std::uint32_t> found;
            if (::Process32FirstW(snapshot, &entry) != 0)
            {
                do
                {
                    const auto candidate = lowercase(to_utf8(entry.szExeFile));
                    if (candidate == wanted || candidate == wanted_exe)
                    {
                        found = entry.th32ProcessID;
                        break;
                    }
                }
                while (::Process32NextW(snapshot, &entry) != 0);
            }

            ::CloseHandle(snapshot);
            return found;
        }

        std::vector<Module> query_modules(HANDLE process)
        {
            std::vector<Module>  modules;
            std::vector<HMODULE> handles(256);
            while (true)
            {
                DWORD needed = 0;
                if (::EnumProcessModulesEx(process,
                                           handles.data(),
                                           static_cast<DWORD>(handles.size() * sizeof(HMODULE)),
                                           &needed,
                                           LIST_MODULES_ALL)
                    == 0)
                {
                    return modules;
                }
                if (needed <= handles.size() * sizeof(HMODULE))
                {
                    handles.resize(needed / sizeof(HMODULE));
                    break;
                }
                handles.resize(needed / sizeof(HMODULE));
            }

            for (const auto module : handles)
            {
                wchar_t name[MAX_PATH] {};
                if (::GetModuleBaseNameW(process, module, name, MAX_PATH) == 0)
                {
                    continue;
                }
                modules.push_back(Module {to_utf8(name), reinterpret_cast<std::uintptr_t>(module)});
            }

            std::ranges::sort(modules, {}, &Module::base);
            return modules;
        }

        // The pages of an image report the image's own base as their allocation
        // base, and that base is the module handle, so the owning module can be
        // asked for the name directly instead of being looked up in the module
        // list the attach captured: a module loaded after that snapshot is still
        // recognised. A module-scoped `aobscan` filters on this name, and the
        // protection alone says nothing about which image a page belongs to.
        std::string image_module_name(HANDLE process, const MEMORY_BASIC_INFORMATION& information)
        {
            if (information.Type != MEM_IMAGE || information.AllocationBase == nullptr)
            {
                return {};
            }

            wchar_t    name[MAX_PATH] {};
            const auto module = static_cast<HMODULE>(information.AllocationBase);
            if (::GetModuleBaseNameW(process, module, name, MAX_PATH) == 0)
            {
                return {};
            }
            return to_utf8(name);
        }

        std::vector<Region> query_regions(HANDLE process)
        {
            std::vector<Region> regions;

            std::uintptr_t address = 0;
            while (address < kMaxAddress)
            {
                MEMORY_BASIC_INFORMATION information {};
                if (::VirtualQueryEx(process, reinterpret_cast<const void*>(address), &information, sizeof(information))
                    == 0)
                {
                    break;
                }

                const auto start = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
                const auto end   = start + information.RegionSize;
                if (information.State == MEM_COMMIT && (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0)
                {
                    Region region;
                    region.start      = start;
                    region.end        = end;
                    region.readable   = readable_protection(information.Protect);
                    region.writable   = writable_protection(information.Protect);
                    region.executable = executable_protection(information.Protect);
                    region.name       = image_module_name(process, information);
                    regions.push_back(std::move(region));
                }

                if (end <= address)
                {
                    break;
                }
                address = end;
            }

            return regions;
        }

        // How far a near jump's signed displacement — and an instruction that names
        // an absolute memory operand — can reach from the instruction.
        constexpr std::uint64_t kReach {0x7FFF'0000};

        struct AddressSpace
        {
            std::uint64_t lowest {};
            std::uint64_t highest {};
            std::uint64_t granularity {};
        };

        AddressSpace address_space()
        {
            SYSTEM_INFO information {};
            ::GetSystemInfo(&information);
            return AddressSpace {
                static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(information.lpMinimumApplicationAddress)),
                static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(information.lpMaximumApplicationAddress)),
                information.dwAllocationGranularity == 0 ? 0x10000u : information.dwAllocationGranularity};
        }

        void* map_at(HANDLE process, std::uint64_t address, std::uint64_t span, DWORD protection)
        {
            return ::VirtualAllocEx(process,
                                    reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)),
                                    static_cast<SIZE_T>(span),
                                    MEM_COMMIT | MEM_RESERVE,
                                    protection);
        }

        // The nearest free base at or below `from`. `VirtualAllocEx` only takes an
        // address it can honour exactly, so a hint inside taken memory fails; walking
        // the target's regions to the top of the closest free one finds a base it
        // accepts.
        std::uint64_t map_down(HANDLE        process,
                               std::uint64_t from,
                               std::uint64_t span,
                               std::uint64_t lowest,
                               std::uint64_t granularity,
                               DWORD         protection)
        {
            std::uint64_t cursor = from & ~(granularity - 1);
            while (cursor >= lowest && cursor != 0)
            {
                MEMORY_BASIC_INFORMATION information {};
                if (::VirtualQueryEx(process,
                                     reinterpret_cast<const void*>(static_cast<std::uintptr_t>(cursor)),
                                     &information,
                                     sizeof(information))
                    == 0)
                {
                    break;
                }

                const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(information.BaseAddress));
                const auto end  = base + static_cast<std::uint64_t>(information.RegionSize);
                if (information.State == MEM_FREE && end > base)
                {
                    std::uint64_t candidate = (end >= span ? end - span : 0) & ~(granularity - 1);
                    if (candidate > cursor)
                    {
                        candidate = cursor;
                    }
                    if (candidate >= base && candidate >= lowest && candidate != 0)
                    {
                        if (auto* mapped = map_at(process, candidate, span, protection); mapped != nullptr)
                        {
                            return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mapped));
                        }
                    }
                }

                if (base <= lowest || end <= base)
                {
                    break;
                }
                cursor = (base - 1) & ~(granularity - 1);
            }
            return 0;
        }

        // The nearest free base at or above `from`, found by the same walk upward.
        std::uint64_t map_up(HANDLE        process,
                             std::uint64_t from,
                             std::uint64_t span,
                             std::uint64_t highest,
                             std::uint64_t granularity,
                             DWORD         protection)
        {
            std::uint64_t cursor = from & ~(granularity - 1);
            while (cursor <= highest && cursor != 0)
            {
                MEMORY_BASIC_INFORMATION information {};
                if (::VirtualQueryEx(process,
                                     reinterpret_cast<const void*>(static_cast<std::uintptr_t>(cursor)),
                                     &information,
                                     sizeof(information))
                    == 0)
                {
                    break;
                }

                const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(information.BaseAddress));
                const auto end  = base + static_cast<std::uint64_t>(information.RegionSize);
                if (end <= cursor)
                {
                    break;
                }

                if (information.State == MEM_FREE)
                {
                    std::uint64_t candidate = base > cursor ? base : cursor;
                    candidate               = (candidate + granularity - 1) & ~(granularity - 1);
                    if (candidate >= base && candidate <= highest && end - candidate >= span)
                    {
                        if (auto* mapped = map_at(process, candidate, span, protection); mapped != nullptr)
                        {
                            return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mapped));
                        }
                    }
                }
                cursor = end;
            }
            return 0;
        }

        // Maps `span` bytes within a near jump's reach of `site`, or returns 0 when
        // no free base that close exists. A slot a script publishes is only useful
        // to code that can name it, and code can reach no further than this.
        std::uint64_t map_near(HANDLE process, std::uint64_t span, std::uint64_t site, DWORD protection)
        {
            const AddressSpace  space = address_space();
            const std::uint64_t start = site & ~(space.granularity - 1);

            if (start >= space.lowest && start <= space.highest)
            {
                if (auto* mapped = map_at(process, start, span, protection); mapped != nullptr)
                {
                    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mapped));
                }
            }

            const std::uint64_t lowest = site > kReach ? std::max(space.lowest, site - kReach) : space.lowest;
            const std::uint64_t highest =
                space.highest > kReach && site < space.highest - kReach ? site + kReach : space.highest;

            if (start > space.granularity)
            {
                const std::uint64_t below =
                    map_down(process, start - space.granularity, span, lowest, space.granularity, protection);
                if (below != 0)
                {
                    return below;
                }
            }
            return map_up(process, start + space.granularity, span, highest, space.granularity, protection);
        }

        class WindowsTarget : public Target
        {
        public:
            WindowsTarget(std::uint32_t       pid,
                          HANDLE              process,
                          std::uint32_t       pointer_size,
                          std::vector<Module> modules,
                          std::vector<Region> regions)
                : pid_ {pid}, process_ {process}, pointer_size_ {pointer_size}, modules_ {std::move(modules)},
                  regions_ {std::move(regions)}
            {
            }

            WindowsTarget(const WindowsTarget&)            = delete;
            WindowsTarget& operator=(const WindowsTarget&) = delete;

            ~WindowsTarget() override
            {
                if (process_ != nullptr)
                {
                    ::CloseHandle(process_);
                }
            }

            [[nodiscard]] std::uint32_t pid() const override
            {
                return pid_;
            }

            [[nodiscard]] std::uint32_t pointer_size() const override
            {
                return pointer_size_;
            }

            [[nodiscard]] bool alive() const override
            {
                return ::WaitForSingleObject(process_, 0) != WAIT_OBJECT_0;
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

                SIZE_T     read   = 0;
                const auto source = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address));
                if (::ReadProcessMemory(process_, source, bytes.data(), size, &read) == 0 || read != size)
                {
                    return std::unexpected(describe_failure("read", address));
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

                SIZE_T written = 0;
                auto*  target  = reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
                if (::WriteProcessMemory(process_, target, bytes.data(), bytes.size(), &written) == 0
                    || written != bytes.size())
                {
                    return std::unexpected(describe_failure("write", address));
                }
                return {};
            }

            [[nodiscard]] std::expected<std::uint64_t, std::string> allocate(std::size_t   size,
                                                                             std::uint64_t site) const override
            {
                return mapping(size, site, PAGE_READWRITE);
            }

            [[nodiscard]] std::expected<std::uint64_t, std::string>
            allocate_executable(std::size_t size, std::uint64_t site) const override
            {
                return mapping(size, site, PAGE_EXECUTE_READWRITE);
            }

            [[nodiscard]] std::expected<void, std::string> deallocate(std::uint64_t address) const override
            {
                const auto found = std::ranges::find(regions_, address, &Region::start);
                if (found == regions_.end())
                {
                    return std::unexpected(
                        std::format("0x{:x} is not a mapping this trainer made in pid {}", address, pid_));
                }

                if (::VirtualFreeEx(
                        process_, reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), 0, MEM_RELEASE)
                    == 0)
                {
                    return std::unexpected(std::format("cannot unmap 0x{:x} in pid {}: Windows error {}",
                                                       address,
                                                       pid_,
                                                       static_cast<unsigned long>(::GetLastError())));
                }

                regions_.erase(found);
                return {};
            }

        private:
            // Maps `size` bytes rounded up to pages, remembering them as a region of
            // the target so the cave scanner and `deallocate` see them.
            [[nodiscard]] std::expected<std::uint64_t, std::string>
            mapping(std::size_t size, std::uint64_t site, DWORD protection) const
            {
                if (size == 0)
                {
                    return std::unexpected(std::string {"the size must be larger than 0"});
                }

                SYSTEM_INFO information {};
                ::GetSystemInfo(&information);
                const auto page =
                    static_cast<std::uint64_t>(information.dwPageSize == 0 ? 0x1000u : information.dwPageSize);
                const auto span = (static_cast<std::uint64_t>(size) + page - 1) / page * page;
                if (span > static_cast<std::uint64_t>((std::numeric_limits<SIZE_T>::max)()))
                {
                    return std::unexpected(
                        std::format("cannot map {} byte(s) in pid {}: the size is too large", size, pid_));
                }

                std::uint64_t address = 0;
                if (site != 0)
                {
                    address = map_near(process_, span, site, protection);
                    if (address == 0)
                    {
                        return std::unexpected(std::format(
                            "cannot map {} byte(s) within a near jump's reach of 0x{:x} in pid {}", size, site, pid_));
                    }
                }
                else
                {
                    auto* mapped = map_at(process_, 0, span, protection);
                    if (mapped == nullptr)
                    {
                        return std::unexpected(std::format("cannot map {} byte(s) in pid {}: Windows error {}",
                                                           size,
                                                           pid_,
                                                           static_cast<unsigned long>(::GetLastError())));
                    }
                    address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mapped));
                }

                const bool executable = protection != PAGE_READWRITE;
                const auto at         = std::ranges::lower_bound(regions_, address, {}, &Region::start);
                regions_.insert(at, Region {address, address + span, true, true, executable, {}});
                return address;
            }

            [[nodiscard]] std::string describe_failure(std::string_view action, std::uint64_t address) const
            {
                return std::format("cannot {} 0x{:x} in pid {}: Windows error {}",
                                   action,
                                   address,
                                   pid_,
                                   static_cast<unsigned long>(::GetLastError()));
            }

            std::uint32_t               pid_ {};
            HANDLE                      process_ {nullptr};
            std::uint32_t               pointer_size_ {8};
            std::vector<Module>         modules_;
            mutable std::vector<Region> regions_;
        };
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

        // SYNCHRONIZE is what `WindowsTarget::alive` waits on; without it
        // `WaitForSingleObject` fails and a live target looks exited.
        HANDLE process = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE
                                           | PROCESS_VM_OPERATION | SYNCHRONIZE,
                                       FALSE,
                                       pid);
        if (process == nullptr)
        {
            const auto error = static_cast<unsigned long>(::GetLastError());
            return std::unexpected(AttachError {
                std::format("cannot open pid {}: Windows error {} (try running as administrator)", pid, error),
                error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND});
        }

        BOOL          wow64        = FALSE;
        std::uint32_t pointer_size = 8;
        if (::IsWow64Process(process, &wow64) != 0 && wow64 != 0)
        {
            pointer_size = 4;
        }

        auto modules = query_modules(process);
        auto regions = query_regions(process);
        return std::make_unique<WindowsTarget>(pid, process, pointer_size, std::move(modules), std::move(regions));
    }

    std::filesystem::path own_executable_path()
    {
        std::vector<wchar_t> buffer(MAX_PATH);
        while (true)
        {
            const auto length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
            {
                return {};
            }
            if (length < buffer.size())
            {
                return std::filesystem::path {
                    std::wstring {buffer.data(), length}
                };
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    bool stdin_is_terminal()
    {
        return ::_isatty(::_fileno(stdin)) != 0;
    }

    std::optional<int> ensure_console_window(bool& window_list)
    {
        window_list = false;

        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr || ::GetProcAddress(ntdll, "wine_get_version") == nullptr)
        {
            return std::nullopt;
        }

        // Wine gives a console started from a Unix terminal a hidden window and
        // keeps reading the terminal itself, and the process cannot move to a
        // window in place: only a process started with CREATE_NEW_CONSOLE gets
        // one. The run is therefore handed to a child of that kind, and the
        // parent waits so the command's lifetime and exit code stay the same.
        wchar_t    guard[2] {};
        const bool windowed = ::GetEnvironmentVariableW(L"SLOPKIT_TRAINER_WINDOW", guard, 2) != 0;
        const HWND window   = ::GetConsoleWindow();
        if (windowed || (window != nullptr && ::IsWindowVisible(window) != 0))
        {
            window_list = true;
            return std::nullopt;
        }

        std::wstring command_line {::GetCommandLineW()};
        static_cast<void>(::SetEnvironmentVariableW(L"SLOPKIT_TRAINER_WINDOW", L"1"));
        STARTUPINFOW        startup {};
        PROCESS_INFORMATION child {};
        startup.cb         = sizeof(startup);
        const BOOL created = ::CreateProcessW(nullptr,
                                              command_line.data(),
                                              nullptr,
                                              nullptr,
                                              FALSE,
                                              CREATE_NEW_CONSOLE,
                                              nullptr,
                                              nullptr,
                                              &startup,
                                              &child);
        static_cast<void>(::SetEnvironmentVariableW(L"SLOPKIT_TRAINER_WINDOW", nullptr));
        if (created == 0)
        {
            return std::nullopt;
        }

        ::CloseHandle(child.hThread);
        static_cast<void>(::WaitForSingleObject(child.hProcess, INFINITE));
        DWORD exit_code = 0;
        static_cast<void>(::GetExitCodeProcess(child.hProcess, &exit_code));
        ::CloseHandle(child.hProcess);
        return static_cast<int>(exit_code);
    }

}
