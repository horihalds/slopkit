#pragma once

#include <cstddef>
#include <cstdint>
#include <flat_map>
#include <vector>

#include "process/types.hpp"

#include <QString>

namespace slopkit::ui::components
{
    struct CodePatch
    {
        std::uint64_t          begin {};
        std::size_t            length {};
        std::vector<std::byte> original_bytes;
        QString                original_text;
        bool                   nop {};

        [[nodiscard]] std::uint64_t end() const noexcept
        {
            return begin + length;
        }
    };

    class CodePatchTable
    {
    public:
        const CodePatch&
        apply_nop(std::uint64_t begin, std::size_t length, std::vector<std::byte> original, QString original_text);
        const CodePatch&
        apply_edit(std::uint64_t begin, std::size_t length, std::vector<std::byte> original, QString original_text);
        bool                           restore(std::uint64_t begin);
        [[nodiscard]] const CodePatch* covering(std::uint64_t address) const;
        void                           note_target(process::ProcessId pid);
        [[nodiscard]] std::size_t      size() const noexcept;
        [[nodiscard]] bool             empty() const noexcept;

    private:
        std::flat_map<std::uint64_t, CodePatch> patches_;
        process::ProcessId                      pid_ {0};
    };

    [[nodiscard]] std::vector<std::byte> nop_bytes(std::size_t length);

}
