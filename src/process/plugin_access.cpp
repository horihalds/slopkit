#include "process/plugin_access.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slopkit::process
{

    namespace
    {
        // Bridges a plugin::PluginSession to the SessionBackend the UI sees and
        // remembers the access method the plugin actually used.
        class PluginSessionBackend final : public SessionBackend
        {
        public:
            PluginSessionBackend(plugin::PluginSession session,
                                 ProcessId             pid,
                                 AccessMethod          advertised,
                                 std::string           plugin_id)
                : session_(std::move(session)), pid_(pid), advertised_(advertised), plugin_id_(std::move(plugin_id))
            {
            }

            [[nodiscard]] ProcessId pid() const noexcept override
            {
                return pid_;
            }

            [[nodiscard]] std::string_view plugin_id() const noexcept override
            {
                return plugin_id_;
            }

            [[nodiscard]] AccessMethod advertised_methods() const noexcept override
            {
                return advertised_;
            }

            [[nodiscard]] AccessMethod last_method() const noexcept override
            {
                return last_method_;
            }

            std::expected<std::vector<std::byte>, AccessError> read(std::uint64_t address, std::size_t size) override
            {
                AccessMethod used   = AccessMethod::none;
                auto         result = session_.read(address, size, used);
                if (result)
                {
                    last_method_ = used;
                }
                return result;
            }

            std::expected<std::size_t, AccessError> write(std::uint64_t              address,
                                                          std::span<const std::byte> data) override
            {
                AccessMethod used   = AccessMethod::none;
                auto         result = session_.write(address, data, used);
                if (result)
                {
                    last_method_ = used;
                }
                return result;
            }

            std::expected<std::vector<ModuleInfo>, AccessError> modules() override
            {
                return session_.modules();
            }

            std::expected<std::vector<ThreadInfo>, AccessError> threads() override
            {
                return session_.threads();
            }

        private:
            plugin::PluginSession session_;
            ProcessId             pid_ {};
            AccessMethod          advertised_ {AccessMethod::none};
            std::string           plugin_id_;
            AccessMethod          last_method_ {AccessMethod::none};
        };
    } // namespace

    std::expected<std::vector<ProcessInfo>, AccessError> PluginAccess::list_processes()
    {
        return host_.list_processes();
    }

    std::expected<Session, AccessError> PluginAccess::attach(ProcessId pid, std::string_view plugin_id)
    {
        auto* plugin = host_.find(plugin_id);
        if (plugin == nullptr)
        {
            return std::unexpected(AccessError::not_found);
        }

        auto session = plugin->open_session(pid);
        if (!session)
        {
            return std::unexpected(session.error());
        }

        auto backend = std::make_unique<PluginSessionBackend>(
            std::move(*session), pid, plugin->access_methods(), std::string(plugin_id));
        return Session(std::move(backend));
    }

} // namespace slopkit::process
