#include "trainer/runtime/logging.hpp"

#include <cstddef>
#include <filesystem>
#include <format>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::trainer
{
    namespace
    {
        // The application's own rolling limit, so a verbose session beside the
        // trainer cannot grow without bound.
        constexpr std::size_t kLogFileLimit = 1024 * 1024;
    }

    std::filesystem::path verbose_log_path(const std::filesystem::path& executable)
    {
        std::filesystem::path path = executable;
        path += ".log";
        return path;
    }

    std::filesystem::path enable_verbose_logging(const std::filesystem::path& executable)
    {
        const auto path   = verbose_log_path(executable);
        auto&      logger = log::Logger::instance();
        logger.set_minimum_level(log::Level::debug);
        logger.add_sink(log::rolling_file_sink(path, kLogFileLimit));
        log::info(log::category::app, std::format("slopkit-trainer verbose logging to {}", path.string()));
        return path;
    }
}
