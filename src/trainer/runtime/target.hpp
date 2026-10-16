#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace slopkit::trainer
{
    struct Module
    {
        std::string   name;
        std::uint64_t base {};
    };

    struct Region
    {
        std::uint64_t start {};
        std::uint64_t end {};
        bool          readable {false};
        bool          writable {false};
        std::string   name;
    };

    // The part of slopkit the runtime needs to reproduce: attached-process memory
    // access plus the module list expressions are resolved against.
    class Target
    {
    public:
        Target()                         = default;
        Target(const Target&)            = delete;
        Target& operator=(const Target&) = delete;
        virtual ~Target()                = default;

        [[nodiscard]] virtual std::uint32_t              pid() const          = 0;
        [[nodiscard]] virtual std::uint32_t              pointer_size() const = 0;
        [[nodiscard]] virtual const std::vector<Module>& modules() const      = 0;
        [[nodiscard]] virtual const std::vector<Region>& regions() const      = 0;

        [[nodiscard]] virtual std::expected<std::vector<std::byte>, std::string> read(std::uint64_t address,
                                                                                      std::size_t   size) const = 0;
        [[nodiscard]] virtual std::expected<void, std::string> write(std::uint64_t              address,
                                                                     std::span<const std::byte> bytes) const    = 0;

        [[nodiscard]] bool readable(std::uint64_t address, std::size_t size) const
        {
            for (const auto& region : regions())
            {
                if (region.readable && address >= region.start && address + size <= region.end)
                {
                    return true;
                }
            }
            return false;
        }
    };

    struct AttachRequest
    {
        std::uint32_t pid {};
        std::string   name;
    };

    struct AttachError
    {
        std::string message;
        // A process that simply is not running is not an access failure; main
        // reports the two with different exit codes.
        bool        not_found {false};
    };

    [[nodiscard]] std::expected<std::unique_ptr<Target>, AttachError> attach(const AttachRequest& request);

    enum class InputStatus
    {
        line,
        idle,
        closed,
    };

    struct InputResult
    {
        InputStatus status {InputStatus::idle};
        std::string line;
    };

    // The path of the running executable, where the generator appended the payload.
    [[nodiscard]] std::filesystem::path own_executable_path();

    // Reads one console line if one arrives within `timeout_ms`; `idle` lets the
    // caller redraw, and `closed` ends the interactive loop.
    [[nodiscard]] InputResult read_line(std::uint32_t timeout_ms);

    [[nodiscard]] bool stdin_is_terminal();

    void clear_screen();

}
