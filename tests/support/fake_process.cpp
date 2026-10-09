#include "support/fake_process.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <QApplication>

// Defined in test_main.cpp; the single QApplication every widget suite shares.
QApplication& slopkit_test_application();

namespace slopkit::test
{

    QApplication& application()
    {
        return slopkit_test_application();
    }

    std::vector<process::ProcessInfo> sample_processes()
    {
        process::ProcessInfo first;
        first.pid       = 10;
        first.name      = "alpha";
        first.exe_path  = "/usr/bin/alpha";
        first.plugin_id = "fake";
        first.claimants = {"fake"};

        process::ProcessInfo second;
        second.pid       = 20;
        second.name      = "beta";
        second.exe_path  = "/usr/bin/beta";
        second.plugin_id = "fake";
        second.claimants = {"fake"};

        return {std::move(first), std::move(second)};
    }

    process::ModuleInfo module_image(std::string_view name, std::uint64_t base, std::uint64_t size)
    {
        process::ModuleInfo module;
        module.kind = process::ModuleKind::elf;
        module.name = std::string(name);
        module.base = base;
        module.size = size;
        return module;
    }

    QString scratch_settings_file(std::string_view suite, std::string_view name)
    {
        const auto      directory = std::filesystem::path(SLOPKIT_TMP_DIR) / std::filesystem::path(suite);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto path = directory / std::filesystem::path(name);
        std::filesystem::remove(path, error);
        return QString::fromStdString(path.string());
    }

    std::expected<std::vector<std::byte>, process::AccessError> FakeMemory::read(std::uint64_t address,
                                                                                 std::size_t   size) const
    {
        if (!flat.empty() || base != 0)
        {
            if (address < base || address - base + size > flat.size())
            {
                return std::unexpected(process::AccessError::not_found);
            }
            const auto offset = static_cast<std::size_t>(address - base);
            return std::vector<std::byte>(flat.begin() + static_cast<std::ptrdiff_t>(offset),
                                          flat.begin() + static_cast<std::ptrdiff_t>(offset + size));
        }
        if (const auto entry = windows.find(address); entry != windows.end())
        {
            std::vector<std::byte> bytes = entry->second;
            bytes.resize(size);
            return bytes;
        }
        if (!bytes.empty())
        {
            std::vector<std::byte> result(size, std::byte {0});
            for (std::size_t index = 0; index < size; ++index)
            {
                if (const auto entry = bytes.find(address + index); entry != bytes.end())
                {
                    result[index] = entry->second;
                }
            }
            return result;
        }
        // A window-only fake reads an unmapped address as an empty buffer.
        return std::vector<std::byte> {};
    }

    std::expected<std::size_t, process::AccessError> FakeMemory::write(std::uint64_t              address,
                                                                       std::span<const std::byte> data)
    {
        if (!flat.empty() || base != 0)
        {
            if (address < base || address - base + data.size() > flat.size())
            {
                return std::unexpected(process::AccessError::not_found);
            }
            const auto offset = static_cast<std::size_t>(address - base);
            std::copy(data.begin(), data.end(), flat.begin() + static_cast<std::ptrdiff_t>(offset));
            return data.size();
        }
        if (!bytes.empty())
        {
            for (std::size_t index = 0; index < data.size(); ++index)
            {
                bytes[address + index] = data[index];
            }
            return data.size();
        }
        // A window-only fake is read-only.
        return std::size_t {};
    }

    FakeBackend::FakeBackend() : memory(std::make_shared<FakeMemory>()) {}

    FakeBackend::FakeBackend(std::shared_ptr<FakeMemory> shared) : memory(std::move(shared)) {}

    process::ProcessId FakeBackend::pid() const noexcept
    {
        return fake_pid;
    }

    std::string_view FakeBackend::plugin_id() const noexcept
    {
        return fake_plugin_id;
    }

    process::AccessMethod FakeBackend::advertised_methods() const noexcept
    {
        return process::AccessMethod::procfs_mem;
    }

    process::AccessMethod FakeBackend::last_method() const noexcept
    {
        return process::AccessMethod::procfs_mem;
    }

    std::expected<std::vector<std::byte>, process::AccessError> FakeBackend::read(std::uint64_t address,
                                                                                  std::size_t   size)
    {
        ++reads;
        if (read_error.has_value())
        {
            return std::unexpected(*read_error);
        }
        if (unreadable->contains(address))
        {
            return std::unexpected(process::AccessError::not_found);
        }
        return memory->read(address, size);
    }

    std::expected<std::size_t, process::AccessError> FakeBackend::write(std::uint64_t              address,
                                                                        std::span<const std::byte> data)
    {
        ++writes;
        return memory->write(address, data);
    }

    std::expected<std::vector<process::ModuleInfo>, process::AccessError> FakeBackend::modules()
    {
        if (!module_list.empty())
        {
            return module_list;
        }
        std::vector<process::ModuleInfo> result(module_count);
        if (!result.empty())
        {
            // A file-backed main image at the flat window's base, so the
            // readability probe has a real address to read.
            auto& main   = result.front();
            main.base    = memory->base;
            main.size    = memory->flat.size();
            main.kind    = process::ModuleKind::elf;
            main.is_main = true;
        }
        return result;
    }

    std::expected<std::vector<process::ThreadInfo>, process::AccessError> FakeBackend::threads()
    {
        return std::vector<process::ThreadInfo>(thread_count);
    }

    std::expected<std::vector<process::RegionInfo>, process::AccessError> FakeBackend::regions()
    {
        return region_list;
    }

    bool FakeBackend::supports_suspend() const noexcept
    {
        return can_suspend;
    }

    std::expected<void, process::AccessError> FakeBackend::suspend()
    {
        ++suspends;
        if (suspend_calls)
        {
            ++(*suspend_calls);
        }
        if (suspend_error.has_value())
        {
            return std::unexpected(*suspend_error);
        }
        return {};
    }

    std::expected<void, process::AccessError> FakeBackend::resume()
    {
        ++resumes;
        if (resume_calls)
        {
            ++(*resume_calls);
        }
        if (resume_error.has_value())
        {
            return std::unexpected(*resume_error);
        }
        return {};
    }

    bool FakeBackend::supports_allocation() const noexcept
    {
        return can_allocate;
    }

    std::expected<std::uint64_t, process::AccessError> FakeBackend::allocate(std::size_t   size,
                                                                             std::uint64_t near_address)
    {
        ++allocates;
        allocate_requests.emplace_back(near_address, size);
        if (!can_allocate)
        {
            return std::unexpected(process::AccessError::unsupported);
        }
        if (allocate_error.has_value())
        {
            return std::unexpected(*allocate_error);
        }

        constexpr std::size_t page    = 0x1000;
        const auto            rounded = (size + page - 1) / page * page;

        std::uint64_t address = 0;
        if (!memory->flat.empty() || memory->base != 0)
        {
            // Carve the block out of the flat window so a script can read and
            // write it; keep every block page-aligned.
            const std::size_t offset = (memory->flat.size() + page - 1) / page * page;
            memory->flat.resize(offset + rounded, std::byte {0});
            address = memory->base + offset;
        }
        else
        {
            address = allocation_base;
            while (allocations.contains(address))
            {
                address += page;
            }
        }

        allocations[address] = rounded;
        return address;
    }

    std::expected<void, process::AccessError> FakeBackend::free(std::uint64_t address)
    {
        ++frees;
        if (free_error.has_value())
        {
            return std::unexpected(*free_error);
        }
        if (allocations.erase(address) == 0)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        return {};
    }

    std::expected<std::vector<process::ProcessInfo>, process::AccessError> FakeAccess::list_processes()
    {
        ++list_calls;
        return processes;
    }

    std::expected<process::Session, process::AccessError> FakeAccess::attach(process::ProcessId, std::string_view)
    {
        ++attach_calls;
        if (attach_fails)
        {
            return std::unexpected(process::AccessError::permission_denied);
        }
        auto backend             = std::make_unique<FakeBackend>(memory);
        backend->fake_pid        = fake_pid;
        backend->unreadable      = unreadable;
        backend->read_error      = read_error;
        backend->module_list     = modules;
        backend->region_list     = regions;
        backend->can_suspend     = can_suspend;
        backend->suspend_error   = suspend_error;
        backend->resume_error    = resume_error;
        backend->suspend_calls   = suspend_calls;
        backend->resume_calls    = resume_calls;
        backend->can_allocate    = can_allocate;
        backend->allocation_base = allocation_base;
        backend->allocate_error  = allocate_error;
        backend->free_error      = free_error;
        return process::Session {std::move(backend)};
    }

    process::AttachedTarget fake_target()
    {
        process::AttachedTarget target;
        target.pid          = 42;
        target.name         = "fake";
        target.exe_path     = "/usr/bin/fake";
        target.plugin_id    = "fake";
        target.method       = process::AccessMethod::procfs_mem;
        target.session_live = true;
        return target;
    }

} // namespace slopkit::test
