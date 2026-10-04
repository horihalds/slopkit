#include "app/sandbox.hpp"

#include <format>

#include <QProcess>
#include <QString>

#include "app/cli.hpp"

namespace slopkit::app
{
    namespace
    {
        std::filesystem::path sandbox_candidate(const std::filesystem::path& directory)
        {
            const std::filesystem::path base = directory.empty() ? executable_directory() : directory;
            return base / "slopkit-sandbox";
        }
    } // namespace

    std::optional<std::filesystem::path> sandbox_binary_path(const std::filesystem::path& directory)
    {
        const std::filesystem::path candidate = sandbox_candidate(directory);
        std::error_code             error;
        if (std::filesystem::is_regular_file(candidate, error))
        {
            return candidate;
        }
        return std::nullopt;
    }

    std::expected<std::int64_t, std::string> launch_sandbox(const std::filesystem::path& directory)
    {
        const std::filesystem::path candidate = sandbox_candidate(directory);
        if (!sandbox_binary_path(directory).has_value())
        {
            return std::unexpected(
                std::format("slopkit-sandbox was not found next to slopkit ({})", candidate.string()));
        }

        qint64 pid = 0;
        if (!QProcess::startDetached(QString::fromStdString(candidate.string()), {}, QString(), &pid))
        {
            return std::unexpected(std::format("slopkit-sandbox could not be started ({})", candidate.string()));
        }
        return static_cast<std::int64_t>(pid);
    }

} // namespace slopkit::app
