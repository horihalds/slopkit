#include "ui/components/code_patch.hpp"

#include <iterator>
#include <utility>

namespace slopkit::ui::components
{

    const CodePatch& CodePatchTable::apply_nop(std::uint64_t          begin,
                                               std::size_t            length,
                                               std::vector<std::byte> original,
                                               QString                original_text)
    {
        CodePatch patch;
        patch.begin          = begin;
        patch.length         = length;
        patch.original_bytes = std::move(original);
        patch.original_text  = std::move(original_text);
        patch.nop            = true;
        return patches_.insert_or_assign(begin, std::move(patch)).first->second;
    }

    const CodePatch& CodePatchTable::apply_edit(std::uint64_t          begin,
                                                std::size_t            length,
                                                std::vector<std::byte> original,
                                                QString                original_text)
    {
        CodePatch patch;
        patch.begin          = begin;
        patch.length         = length;
        patch.original_bytes = std::move(original);
        patch.original_text  = std::move(original_text);
        return patches_.insert_or_assign(begin, std::move(patch)).first->second;
    }

    bool CodePatchTable::restore(std::uint64_t begin)
    {
        return patches_.erase(begin) != 0;
    }

    const CodePatch* CodePatchTable::covering(std::uint64_t address) const
    {
        // The record whose first byte is the greatest at or below `address`; it
        // covers the address when its region reaches that far.
        const auto after = patches_.upper_bound(address);
        if (after == patches_.begin())
        {
            return nullptr;
        }
        const CodePatch& candidate = std::prev(after)->second;
        return address < candidate.end() ? &candidate : nullptr;
    }

    void CodePatchTable::note_target(process::ProcessId pid)
    {
        if (pid == pid_)
        {
            return;
        }
        pid_ = pid;
        patches_.clear();
    }

    std::size_t CodePatchTable::size() const noexcept
    {
        return patches_.size();
    }

    bool CodePatchTable::empty() const noexcept
    {
        return patches_.empty();
    }

    std::vector<std::byte> nop_bytes(std::size_t length)
    {
        return std::vector<std::byte>(length, std::byte {0x90});
    }

} // namespace slopkit::ui::components
