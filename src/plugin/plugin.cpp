#include "plugin/plugin.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>

#include <dlfcn.h>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::plugin
{

    namespace
    {
        std::string last_error()
        {
            const char* error = ::dlerror();
            return error != nullptr ? std::string(error) : std::string("unknown dynamic loader error");
        }

        process::ModuleKind module_kind_from_abi(int32_t kind)
        {
            switch (kind)
            {
            case SLOPKIT_MODULE_ELF:
                return process::ModuleKind::elf;
            case SLOPKIT_MODULE_PE:
                return process::ModuleKind::pe;
            default:
                return process::ModuleKind::anonymous;
            }
        }
    } // namespace

    namespace detail
    {
        namespace
        {
            void* host_alloc(std::size_t size, void* /*user_data*/)
            {
                return std::malloc(size);
            }

            void host_dealloc(void* memory, void* /*user_data*/)
            {
                std::free(memory);
            }

            log::Level to_log_level(int32_t level)
            {
                switch (level)
                {
                case SLOPKIT_LOG_DEBUG:
                    return log::Level::debug;
                case SLOPKIT_LOG_WARN:
                    return log::Level::warning;
                case SLOPKIT_LOG_ERROR:
                    return log::Level::error;
                case SLOPKIT_LOG_INFO:
                default:
                    return log::Level::info;
                }
            }

            // Attributes a plugin's message to the plugin that emitted it, falling
            // back to the library file name before the descriptor is read.
            void host_log(int32_t level, const char* message, void* user_data)
            {
                const auto* context    = static_cast<const PluginLogContext*>(user_data);
                std::string attributor = "plugin";
                if (context != nullptr)
                {
                    attributor = context->plugin_id.empty() ? context->path.filename().string() : context->plugin_id;
                }

                const std::string_view detail =
                    message != nullptr ? std::string_view {message} : std::string_view {"(no message)"};
                log::Logger::instance().log(to_log_level(level),
                                            log::category::plugin,
                                            attributor.empty() ? std::string {detail}
                                                               : std::format("{}: {}", attributor, detail));
            }
        } // namespace

        slopkit_host_services make_host_services(PluginLogContext* context)
        {
            slopkit_host_services services {};
            services.abi_version = SLOPKIT_PLUGIN_ABI_VERSION;
            services.struct_size = sizeof(slopkit_host_services);
            services.user_data   = context;
            services.alloc       = host_alloc;
            services.dealloc     = host_dealloc;
            services.log         = host_log;
            return services;
        }
    } // namespace detail

    // --- DynamicLibrary -----------------------------------------------------

    DynamicLibrary::DynamicLibrary(DynamicLibrary&&) noexcept            = default;
    DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&&) noexcept = default;
    DynamicLibrary::~DynamicLibrary()                                    = default;

    void DynamicLibrary::HandleDeleter::operator()(void* handle) const noexcept
    {
        if (handle != nullptr)
        {
            ::dlclose(handle);
        }
    }

    std::expected<DynamicLibrary, std::string> DynamicLibrary::open(const std::filesystem::path& path)
    {
        ::dlerror(); // Clear any pending error.
        void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr)
        {
            return std::unexpected(last_error());
        }

        DynamicLibrary library;
        library.handle_.reset(handle);
        library.path_ = path;
        return library;
    }

    void* DynamicLibrary::symbol(const char* name) const noexcept
    {
        return handle_ != nullptr ? ::dlsym(handle_.get(), name) : nullptr;
    }

    bool DynamicLibrary::is_open() const noexcept
    {
        return handle_ != nullptr;
    }

    const std::filesystem::path& DynamicLibrary::path() const noexcept
    {
        return path_;
    }

    // --- Plugin -------------------------------------------------------------

    Plugin::Plugin(DynamicLibrary library) : library_(std::move(library))
    {
        log_context_.path      = library_.path();
        log_context_.plugin_id = library_.path().filename().string();
        services_              = detail::make_host_services(&log_context_);
    }

    std::expected<std::unique_ptr<Plugin>, std::string> Plugin::load(const std::filesystem::path& path)
    {
        auto library = DynamicLibrary::open(path);
        if (!library)
        {
            return std::unexpected("cannot open library: " + library.error());
        }

        auto plugin = std::make_unique<Plugin>(std::move(*library));

        using EntryPoint = const slopkit_plugin_vtable* (*)(const slopkit_host_services*);
        auto* entry      = reinterpret_cast<EntryPoint>(plugin->library_.symbol("slopkit_plugin_entry"));
        if (entry == nullptr)
        {
            return std::unexpected("missing slopkit_plugin_entry symbol");
        }

        const slopkit_plugin_vtable* vtable = nullptr;
        try
        {
            vtable = entry(&plugin->services_);
        }
        catch (...)
        {
            return std::unexpected("slopkit_plugin_entry threw an exception");
        }
        if (vtable == nullptr)
        {
            return std::unexpected("slopkit_plugin_entry returned null");
        }

        const auto major = static_cast<std::uint32_t>(vtable->abi_version >> 16);
        if (major != SLOPKIT_PLUGIN_ABI_VERSION_MAJOR)
        {
            return std::unexpected("incompatible ABI major version " + std::to_string(major));
        }
        const auto minor = static_cast<std::uint32_t>(vtable->abi_version & 0xffff);
        if (minor < SLOPKIT_PLUGIN_ABI_VERSION_MINOR)
        {
            return std::unexpected("incompatible ABI version " + std::to_string(major) + "." + std::to_string(minor)
                                   + " (host requires " + std::to_string(SLOPKIT_PLUGIN_ABI_VERSION_MAJOR) + "."
                                   + std::to_string(SLOPKIT_PLUGIN_ABI_VERSION_MINOR) + ")");
        }
        if (vtable->struct_size < sizeof(slopkit_plugin_vtable))
        {
            return std::unexpected("vtable struct too small");
        }
        if (vtable->info == nullptr || vtable->precedence == nullptr || vtable->list_processes == nullptr
            || vtable->open_session == nullptr || vtable->close_session == nullptr || vtable->read_memory == nullptr
            || vtable->write_memory == nullptr || vtable->list_modules == nullptr || vtable->list_threads == nullptr
            || vtable->list_regions == nullptr || vtable->access_methods == nullptr)
        {
            return std::unexpected("vtable has null function pointers");
        }

        const slopkit_plugin_info* info = nullptr;
        try
        {
            info = vtable->info();
        }
        catch (...)
        {
            return std::unexpected("plugin info() threw an exception");
        }
        if (info == nullptr || info->id == nullptr || info->id[0] == '\0')
        {
            return std::unexpected("plugin did not report an id");
        }

        plugin->vtable_                = vtable;
        plugin->id_                    = info->id;
        plugin->log_context_.plugin_id = info->id;
        plugin->name_                  = info->name != nullptr ? info->name : "";
        plugin->version_               = info->version != nullptr ? info->version : "";
        plugin->description_           = info->description != nullptr ? info->description : "";
        plugin->precedence_            = vtable->precedence();
        plugin->access_methods_        = static_cast<process::AccessMethod>(vtable->access_methods());
        return plugin;
    }

    std::string_view Plugin::id() const noexcept
    {
        return id_;
    }

    std::string_view Plugin::name() const noexcept
    {
        return name_;
    }

    std::string_view Plugin::version() const noexcept
    {
        return version_;
    }

    std::string_view Plugin::description() const noexcept
    {
        return description_;
    }

    std::int32_t Plugin::precedence() const noexcept
    {
        return precedence_;
    }

    process::AccessMethod Plugin::access_methods() const noexcept
    {
        return access_methods_;
    }

    const std::filesystem::path& Plugin::path() const noexcept
    {
        return library_.path();
    }

    void* Plugin::alloc(std::size_t size) const
    {
        if (services_.alloc == nullptr)
        {
            return nullptr;
        }
        return services_.alloc(size, services_.user_data);
    }

    void Plugin::dealloc(void* memory) const
    {
        if (memory == nullptr || services_.dealloc == nullptr)
        {
            return;
        }
        services_.dealloc(memory, services_.user_data);
    }

    process::AccessError Plugin::classify(const slopkit_result& result)
    {
        switch (result.code)
        {
        case SLOPKIT_ERR_PERMISSION_DENIED:
            return process::AccessError::permission_denied;
        case SLOPKIT_ERR_NOT_FOUND:
            return process::AccessError::not_found;
        case SLOPKIT_ERR_UNSUPPORTED:
            return process::AccessError::unsupported;
        case SLOPKIT_ERR_IO:
            return process::AccessError::io_error;
        case SLOPKIT_ERR_INVALID_ABI:
            return process::AccessError::invalid_abi;
        case SLOPKIT_ERR_INVALID_ARGUMENT:
            return process::AccessError::invalid_argument;
        default:
            return process::AccessError::internal;
        }
    }

    std::expected<std::vector<process::ProcessInfo>, process::AccessError> Plugin::list_processes()
    {
        slopkit_process_info* array = nullptr;
        std::size_t           count = 0;
        slopkit_result        result {};
        try
        {
            result = vtable_->list_processes(&array, &count);
        }
        catch (...)
        {
            return std::unexpected(process::AccessError::internal);
        }

        if (result.code != SLOPKIT_OK)
        {
            dealloc(array);
            return std::unexpected(classify(result));
        }

        std::vector<process::ProcessInfo> processes;
        processes.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            process::ProcessInfo info;
            info.pid = array[i].pid;
            if (array[i].name != nullptr)
            {
                info.name = array[i].name;
            }
            if (array[i].exe_path != nullptr)
            {
                info.exe_path = array[i].exe_path;
            }
            info.plugin_id = id_;
            info.claimants.push_back(id_);
            processes.push_back(std::move(info));
        }
        dealloc(array);
        return processes;
    }

    std::expected<PluginSession, process::AccessError> Plugin::open_session(process::ProcessId pid)
    {
        void*          handle = nullptr;
        slopkit_result result {};
        try
        {
            result = vtable_->open_session(pid, &handle);
        }
        catch (...)
        {
            return std::unexpected(process::AccessError::internal);
        }

        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(classify(result));
        }
        if (handle == nullptr)
        {
            return std::unexpected(process::AccessError::internal);
        }
        return PluginSession(this, handle);
    }

    // --- PluginSession ------------------------------------------------------

    PluginSession::PluginSession(const Plugin* plugin, void* handle) noexcept : plugin_(plugin), handle_(handle) {}

    PluginSession::PluginSession(PluginSession&& other) noexcept : plugin_(other.plugin_), handle_(other.handle_)
    {
        other.plugin_ = nullptr;
        other.handle_ = nullptr;
    }

    PluginSession& PluginSession::operator=(PluginSession&& other) noexcept
    {
        if (this != &other)
        {
            close();
            plugin_       = other.plugin_;
            handle_       = other.handle_;
            other.plugin_ = nullptr;
            other.handle_ = nullptr;
        }
        return *this;
    }

    PluginSession::~PluginSession()
    {
        close();
    }

    void PluginSession::close() noexcept
    {
        if (plugin_ != nullptr && handle_ != nullptr)
        {
            try
            {
                plugin_->vtable_->close_session(handle_);
            }
            catch (...)
            {
                // Plugins must not throw across the ABI; ignore if they do.
            }
        }
        plugin_ = nullptr;
        handle_ = nullptr;
    }

    bool PluginSession::valid() const noexcept
    {
        return plugin_ != nullptr && handle_ != nullptr;
    }

    std::expected<std::vector<std::byte>, process::AccessError>
    PluginSession::read(std::uint64_t address, std::size_t size, process::AccessMethod& used)
    {
        used = process::AccessMethod::none;
        if (!valid())
        {
            return std::unexpected(process::AccessError::internal);
        }

        std::vector<std::byte> buffer(size);
        std::size_t            read   = 0;
        std::uint32_t          method = 0;
        slopkit_result         result {};
        try
        {
            result = plugin_->vtable_->read_memory(handle_, address, buffer.data(), size, &read, &method);
        }
        catch (...)
        {
            return std::unexpected(process::AccessError::internal);
        }
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(Plugin::classify(result));
        }

        used = static_cast<process::AccessMethod>(method);
        buffer.resize(std::min(read, size));
        return buffer;
    }

    std::expected<std::size_t, process::AccessError>
    PluginSession::read_into(std::uint64_t address, std::span<std::byte> buffer, process::AccessMethod& used)
    {
        used = process::AccessMethod::none;
        if (!valid())
        {
            return std::unexpected(process::AccessError::internal);
        }

        std::size_t    read   = 0;
        std::uint32_t  method = 0;
        slopkit_result result {};
        try
        {
            result = plugin_->vtable_->read_memory(handle_, address, buffer.data(), buffer.size(), &read, &method);
        }
        catch (...)
        {
            return std::unexpected(process::AccessError::internal);
        }
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(Plugin::classify(result));
        }

        used = static_cast<process::AccessMethod>(method);
        return std::min(read, buffer.size());
    }

    std::expected<std::size_t, process::AccessError>
    PluginSession::write(std::uint64_t address, std::span<const std::byte> data, process::AccessMethod& used)
    {
        used = process::AccessMethod::none;
        if (!valid())
        {
            return std::unexpected(process::AccessError::internal);
        }

        std::size_t    written = 0;
        std::uint32_t  method  = 0;
        slopkit_result result {};
        try
        {
            result = plugin_->vtable_->write_memory(handle_, address, data.data(), data.size(), &written, &method);
        }
        catch (...)
        {
            return std::unexpected(process::AccessError::internal);
        }
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(Plugin::classify(result));
        }

        used = static_cast<process::AccessMethod>(method);
        return written;
    }

    std::expected<std::vector<process::ModuleInfo>, process::AccessError> PluginSession::modules()
    {
        if (!valid())
        {
            return std::unexpected(process::AccessError::internal);
        }

        slopkit_module_info* array = nullptr;
        std::size_t          count = 0;
        slopkit_result       result {};
        try
        {
            result = plugin_->vtable_->list_modules(handle_, &array, &count);
        }
        catch (...)
        {
            return std::unexpected(process::AccessError::internal);
        }
        if (result.code != SLOPKIT_OK)
        {
            plugin_->dealloc(array);
            return std::unexpected(Plugin::classify(result));
        }

        std::vector<process::ModuleInfo> modules;
        modules.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            process::ModuleInfo module;
            module.base    = array[i].base;
            module.size    = array[i].size;
            module.offset  = array[i].offset;
            module.entry   = array[i].entry;
            module.kind    = module_kind_from_abi(array[i].kind);
            module.is_main = array[i].is_main != 0;
            if (array[i].name != nullptr)
            {
                module.name = array[i].name;
            }
            if (array[i].path != nullptr)
            {
                module.path = array[i].path;
            }
            modules.push_back(std::move(module));
        }
        plugin_->dealloc(array);
        return modules;
    }

    std::expected<std::vector<process::ThreadInfo>, process::AccessError> PluginSession::threads()
    {
        if (!valid())
        {
            return std::unexpected(process::AccessError::internal);
        }

        slopkit_thread_info* array = nullptr;
        std::size_t          count = 0;
        slopkit_result       result {};
        try
        {
            result = plugin_->vtable_->list_threads(handle_, &array, &count);
        }
        catch (...)
        {
            return std::unexpected(process::AccessError::internal);
        }
        if (result.code != SLOPKIT_OK)
        {
            plugin_->dealloc(array);
            return std::unexpected(Plugin::classify(result));
        }

        std::vector<process::ThreadInfo> threads;
        threads.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            process::ThreadInfo thread;
            thread.tid = array[i].tid;
            if (array[i].name != nullptr)
            {
                thread.name = array[i].name;
            }
            threads.push_back(std::move(thread));
        }
        plugin_->dealloc(array);
        return threads;
    }

    bool PluginSession::supports_debug() const noexcept
    {
        if (plugin_ == nullptr || handle_ == nullptr || plugin_->vtable_ == nullptr)
        {
            return false;
        }
        const auto* vtable = plugin_->vtable_;
        return vtable->debug_attach != nullptr && vtable->debug_detach != nullptr && vtable->debug_continue != nullptr
            && vtable->debug_step != nullptr && vtable->debug_interrupt != nullptr
            && vtable->debug_get_registers != nullptr && vtable->debug_set_register != nullptr
            && vtable->debug_set_software_breakpoint != nullptr && vtable->debug_set_hardware_breakpoint != nullptr
            && vtable->debug_backtrace != nullptr;
    }

    const slopkit_plugin_vtable* PluginSession::vtable() const noexcept
    {
        return valid() ? plugin_->vtable_ : nullptr;
    }

    void* PluginSession::handle() const noexcept
    {
        return handle_;
    }

    void PluginSession::dealloc(void* memory) noexcept
    {
        if (plugin_ != nullptr)
        {
            plugin_->dealloc(memory);
        }
    }

    std::expected<std::vector<process::RegionInfo>, process::AccessError> PluginSession::regions()
    {
        if (!valid())
        {
            return std::unexpected(process::AccessError::internal);
        }

        slopkit_region_info* array = nullptr;
        std::size_t          count = 0;
        slopkit_result       result {};
        try
        {
            result = plugin_->vtable_->list_regions(handle_, &array, &count);
        }
        catch (...)
        {
            return std::unexpected(process::AccessError::internal);
        }
        if (result.code != SLOPKIT_OK)
        {
            plugin_->dealloc(array);
            return std::unexpected(Plugin::classify(result));
        }

        std::vector<process::RegionInfo> regions;
        regions.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            process::RegionInfo region;
            region.start      = array[i].start;
            region.end        = array[i].end;
            region.offset     = array[i].offset;
            region.readable   = array[i].readable != 0;
            region.writable   = array[i].writable != 0;
            region.executable = array[i].executable != 0;
            region.shared     = array[i].shared != 0;
            if (array[i].path != nullptr)
            {
                region.path = array[i].path;
            }
            regions.push_back(std::move(region));
        }
        plugin_->dealloc(array);
        return regions;
    }

    bool PluginSession::supports_suspend() const noexcept
    {
        if (plugin_ == nullptr || handle_ == nullptr || plugin_->vtable_ == nullptr)
        {
            return false;
        }
        const auto* vtable = plugin_->vtable_;
        return vtable->suspend_target != nullptr && vtable->resume_target != nullptr;
    }

    std::expected<void, process::AccessError> PluginSession::suspend_target()
    {
        if (!valid())
        {
            return std::unexpected(process::AccessError::internal);
        }
        if (plugin_->vtable_->suspend_target == nullptr)
        {
            return std::unexpected(process::AccessError::unsupported);
        }

        slopkit_result result {};
        try
        {
            result = plugin_->vtable_->suspend_target(handle_);
        }
        catch (...)
        {
            return std::unexpected(process::AccessError::internal);
        }
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(Plugin::classify(result));
        }
        return {};
    }

    std::expected<void, process::AccessError> PluginSession::resume_target()
    {
        if (!valid())
        {
            return std::unexpected(process::AccessError::internal);
        }
        if (plugin_->vtable_->resume_target == nullptr)
        {
            return std::unexpected(process::AccessError::unsupported);
        }

        slopkit_result result {};
        try
        {
            result = plugin_->vtable_->resume_target(handle_);
        }
        catch (...)
        {
            return std::unexpected(process::AccessError::internal);
        }
        if (result.code != SLOPKIT_OK)
        {
            return std::unexpected(Plugin::classify(result));
        }
        return {};
    }

} // namespace slopkit::plugin
