#pragma once

#include <array>
#include <cstdint>

#include "process/attachment.hpp"

namespace slopkit::ui::dialogs
{

    // A hex dump of the attached target's memory. The disassembler pane is out
    // of scope, so this ships the byte and ASCII view only.
    class MemoryViewer
    {
    public:
        explicit MemoryViewer(process::AttachedTarget& target);

        void draw(bool& open);

        // Opens the viewer at `address`, aligned down to a row boundary.
        void set_address(std::uint64_t address);

    private:
        process::AttachedTarget& target_;
        std::uint64_t            base_ {};
        std::array<char, 32>     goto_buffer_ {};
    };

} // namespace slopkit::ui::dialogs
