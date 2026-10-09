#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/plugin_api.h"
#include "process/types.hpp"

namespace slopkit::plugin
{

    // RAII wrapper over a dlopen handle. Non-copyable, movable.
    class DynamicLibrary
    {
    public:
        DynamicLibrary() = default;
        DynamicLibrary(DynamicLibrary&&) noexcept;
        DynamicLibrary& operator=(DynamicLibrary&&) noexcept;
        DynamicLibrary(const DynamicLibrary&)            = delete;
        DynamicLibrary& operator=(const DynamicLibrary&) = delete;
        ~DynamicLibrary();

        static std::expected<DynamicLibrary, std::string> open(const std::filesystem::path& path);

        [[nodiscard]] void*                        symbol(const char* name) const noexcept;
        [[nodiscard]] bool                         is_open() const noexcept;
        [[nodiscard]] const std::filesystem::path& path() const noexcept;

    private:
        struct HandleDeleter
        {
            void operator()(void* handle) const noexcept;
        };

        std::unique_ptr<void, HandleDeleter> handle_;
        std::filesystem::path                path_;
    };

    class Plugin;

    // Host-side log context handed to a plugin as host_services::user_data so
    // host_log can attribute a record to it. Owned by the Plugin.
    struct PluginLogContext
    {
        std::string           plugin_id; // empty until the descriptor is read
        std::filesystem::path path;
    };

    namespace detail
    {
        // The one factory for host services: fills abi_version, struct_size,
        // alloc/dealloc/log and points user_data at `context`.
        slopkit_host_services make_host_services(PluginLogContext* context);
    } // namespace detail

    // RAII attachment to a target held by a plugin.
    class PluginSession
    {
    public:
        PluginSession() = default;
        PluginSession(const Plugin* plugin, void* handle) noexcept;
        PluginSession(PluginSession&&) noexcept;
        PluginSession& operator=(PluginSession&&) noexcept;
        PluginSession(const PluginSession&)            = delete;
        PluginSession& operator=(const PluginSession&) = delete;
        ~PluginSession();

        [[nodiscard]] bool valid() const noexcept;

        std::expected<std::vector<std::byte>, process::AccessError>
        read(std::uint64_t address, std::size_t size, process::AccessMethod& used);
        std::expected<std::size_t, process::AccessError>
        read_into(std::uint64_t address, std::span<std::byte> buffer, process::AccessMethod& used);
        std::expected<std::size_t, process::AccessError>
        write(std::uint64_t address, std::span<const std::byte> data, process::AccessMethod& used);
        std::expected<std::vector<process::ModuleInfo>, process::AccessError> modules();
        std::expected<std::vector<process::ThreadInfo>, process::AccessError> threads();
        std::expected<std::vector<process::RegionInfo>, process::AccessError> regions();

        // True when the plugin implements the ABI 1.5 suspend operations.
        [[nodiscard]] bool                        supports_suspend() const noexcept;
        // Stops / resumes every thread of the session's target.
        std::expected<void, process::AccessError> suspend_target();
        std::expected<void, process::AccessError> resume_target();

        // True when the plugin implements the ABI 1.6 allocation operations.
        [[nodiscard]] bool                                 supports_allocation() const noexcept;
        // Maps `size` bytes (page-rounded) of read/write/execute memory in the
        // target, as close to `near_address` as it can (0 = anywhere): the hint
        // itself, else free space below it, else above it, else anywhere. Returns
        // the mapping base.
        std::expected<std::uint64_t, process::AccessError> allocate_memory(std::size_t   size,
                                                                           std::uint64_t near_address);
        // Unmaps a mapping an earlier allocate_memory of this session returned.
        std::expected<void, process::AccessError>          free_memory(std::uint64_t address);

        // True when the plugin implements the ABI 1.4 debug operations.
        [[nodiscard]] bool                         supports_debug() const noexcept;
        // Raw ABI view, for the debug backend that maps the debug_* operations
        // onto host types. Null when the session is invalid.
        [[nodiscard]] const slopkit_plugin_vtable* vtable() const noexcept;
        [[nodiscard]] void*                        handle() const noexcept;
        // Releases a buffer a debug_* operation returned through the plugin
        // allocator.
        void                                       dealloc(void* memory) noexcept;

    private:
        void close() noexcept;

        const Plugin* plugin_ {nullptr};
        void*         handle_ {nullptr};
    };

    // A loaded plugin: descriptor plus typed calls that translate slopkit_result
    // into std::expected and manage buffers through the host allocator.
    class Plugin
    {
    public:
        static std::expected<std::unique_ptr<Plugin>, std::string> load(const std::filesystem::path& path);

        explicit Plugin(DynamicLibrary library);
        Plugin(const Plugin&)            = delete;
        Plugin& operator=(const Plugin&) = delete;
        ~Plugin()                        = default;

        [[nodiscard]] std::string_view             id() const noexcept;
        [[nodiscard]] std::string_view             name() const noexcept;
        [[nodiscard]] std::string_view             version() const noexcept;
        [[nodiscard]] std::string_view             description() const noexcept;
        [[nodiscard]] std::int32_t                 precedence() const noexcept;
        [[nodiscard]] process::AccessMethod        access_methods() const noexcept;
        [[nodiscard]] const std::filesystem::path& path() const noexcept;

        std::expected<std::vector<process::ProcessInfo>, process::AccessError> list_processes();
        std::expected<PluginSession, process::AccessError>                     open_session(process::ProcessId pid);

    private:
        friend class PluginSession;

        void*                       alloc(std::size_t size) const;
        void                        dealloc(void* memory) const;
        static process::AccessError classify(const slopkit_result& result);

        DynamicLibrary               library_;
        const slopkit_plugin_vtable* vtable_ {nullptr};
        PluginLogContext             log_context_;
        slopkit_host_services        services_ {};
        std::string                  id_;
        std::string                  name_;
        std::string                  version_;
        std::string                  description_;
        std::int32_t                 precedence_ {0};
        process::AccessMethod        access_methods_ {process::AccessMethod::none};
    };

} // namespace slopkit::plugin
