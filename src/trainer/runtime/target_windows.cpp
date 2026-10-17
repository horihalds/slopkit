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

        private:
            [[nodiscard]] std::string describe_failure(std::string_view action, std::uint64_t address) const
            {
                return std::format("cannot {} 0x{:x} in pid {}: Windows error {}",
                                   action,
                                   address,
                                   pid_,
                                   static_cast<unsigned long>(::GetLastError()));
            }

            std::uint32_t       pid_ {};
            HANDLE              process_ {nullptr};
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
            if (!found)
            {
                return std::unexpected(AttachError {
                    std::format("no running process is named \"{}\"; pass --pid or --name", request.name), true});
            }
            pid = *found;
        }

        HANDLE process = ::OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
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

}
