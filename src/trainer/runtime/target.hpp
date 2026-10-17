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
        bool          executable {false};
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

        [[nodiscard]] virtual std::uint32_t pid() const          = 0;
        [[nodiscard]] virtual std::uint32_t pointer_size() const = 0;

        // Whether the process behind this handle is still running. A target that
        // cannot say keeps the previous answer, so an inconclusive probe never
        // detaches a live trainer.
        [[nodiscard]] virtual bool alive() const
        {
            return true;
        }

        [[nodiscard]] virtual const std::vector<Module>& modules() const = 0;
        [[nodiscard]] virtual const std::vector<Region>& regions() const = 0;

        [[nodiscard]] virtual std::expected<std::vector<std::byte>, std::string> read(std::uint64_t address,
                                                                                      std::size_t   size) const = 0;
        [[nodiscard]] virtual std::expected<void, std::string> write(std::uint64_t              address,
                                                                     std::span<const std::byte> bytes) const    = 0;

        // Maps `size` bytes in the target that its own code may write, at an address
        // the backend picks, or refuses when it cannot map memory in another process.
        // A run of zero bytes borrowed from the target is only *probably* free — a
        // module's zero-initialized data is zero at startup and written as the target
        // runs — so memory a script publishes must be mapped where that is possible.
        [[nodiscard]] virtual std::expected<std::uint64_t, std::string> allocate(std::size_t) const
        {
            return std::unexpected(std::string {"this target cannot map memory"});
        }

        // Unmaps an address `allocate` returned; anything else is refused.
        [[nodiscard]] virtual std::expected<void, std::string> deallocate(std::uint64_t) const
        {
            return std::unexpected(std::string {"this target cannot unmap memory"});
        }

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

    // The path of the running executable, where the generator appended the payload.
    [[nodiscard]] std::filesystem::path own_executable_path();

    // Whether the interactive mode has a terminal to draw in; without one the list
    // is printed once and the trainer exits.
    [[nodiscard]] bool stdin_is_terminal();

    // Hands the run to itself in a console window and returns that run's exit
    // code when the trainer only borrowed a Wine console from a Unix terminal.
    // Sets `window_list` when the interactive list has to be painted with the
    // Win32 console API instead of FTXUI, which such a console cannot drive.
    [[nodiscard]] std::optional<int> ensure_console_window(bool& window_list);

}
